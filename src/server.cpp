#if __cplusplus < 202002L
#error "Wavy-Server requires C++20 or later."
#endif

#include "../include/logger.hpp"
#include "../include/macros.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast.hpp>
#include <boost/filesystem.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "../include/decompression.h"
#include "../include/toml/toml_parser.hpp"

/*
 * SERVER
 *
 * The server's main responsibility the way we see it is:
 *
 * -> Accepting requests through secure SSL transfer with valid certification (TBD) to upload files
 * (POST)
 * -> Upload of GZip file (which supposedly contains .m3u8 and .ts files with apt references and
 * hierarchial format as expected of HLS encoding)
 * -> Validation of the above GZip file's contents to match expected encoded files
 * -> Assign client a UUID upon validation
 * -> Create and expose the client's uploaded content through UUID
 * -> Serve required files to receiver through GET
 *
 *  Server HLS Storage Organization:
 *
 *  hls_storage/
 *  ├── 192.168.1.10/                                    # AUDIO OWNER IP Address 192.168.1.10 (example)
 *  │   ├── 1435f431-a69a-4027-8661-44c31cd11ef6/        # Randomly generated audio id
 *  │   │   ├── index.m3u8
 *  │   │   ├── hls_mp3_64.m3u8                          # HLS MP3 encoded playlist (64-bit)
 *  │   │   ├── hls_mp3_64_0.ts                          # First transport stream of hls_mp3_64 playlist                
 *  │   │   ├── ...                                      # Similarly for 128 and 256 bitrates
 *  │   │   ├── metadata.toml                            # Metadata and other song information
 *  │   ├── e5fdeca5-57c8-47b4-b9c6-60492ddf11ae/
 *  │   │   ├── index.m3u8
 *  │   │   ├── hls_flac_64.m3u8                         # HLS FLAC encoded playlist (64-bit)
 *  │   │   ├── hls_flac_64_0.ts                         # First transport stream of hls_mp3_64 playlist 
 *  │   │   ├── ...                                      # Similarly for 128 and 256 bitrates
 *  │   │   ├── metadata.toml                            # Metadata and other song information
 *  │    
 *
 * Boost libs ensures that every operation (if not then most) in the server occurs asynchronously
 * without any concurrency issues
 *
 * Although not tried and tested with multiple clients, it should give expected results.
 *
 * Boost should also ensure safety and shared lifetimes of a lot of critical objects.
 *
 * @NOTE:
 *
 * ==> This file **CANNOT** be compiled for < -std=c++20
 *
 * Look at Makefile for more information
 *
 * ==> macros::SERVER_STORAGE_DIR and macros::SERVER_TEMP_STORAGE_DIR) are in the same parent
 * directory as boost has problems renaming content to different directories (gives a very big
 * cross-link error)
 *
 */

namespace fs    = boost::filesystem;
namespace beast = boost::beast;
namespace http  = beast::http;
using boost::asio::ip::tcp;

auto is_valid_extension(const std::string& filename) -> bool
{
  return filename.ends_with(macros::PLAYLIST_EXT) ||
         filename.ends_with(macros::TRANSPORT_STREAM_EXT) ||
         filename.ends_with(macros::M4S_FILE_EXT);
}

auto validate_m3u8_format(const std::string& content) -> bool
{
  return content.find(macros::PLAYLIST_GLOBAL_HEADER) != std::string::npos;
}

auto validate_ts_file(const std::vector<uint8_t>& data) -> bool
{
  return !data.empty() && data[0] == TRANSPORT_STREAM_START_BYTE; // MPEG-TS sync byte
}

auto validate_m4s(const std::string& m4s_path) -> bool
{
  std::ifstream file(m4s_path, std::ios::binary);
  if (!file.is_open())
  {
    LOG_ERROR << SERVER_VALIDATE_LOG << "Failed to open .m4s file: " << m4s_path;
    return false;
  }

  // Read the first 12 bytes (enough to check for 'ftyp' and a major brand)
  std::vector<uint8_t> header(12);
  file.read(reinterpret_cast<char*>(header.data()), header.size());

  if (file.gcount() < 12)
  {
    LOG_ERROR << SERVER_VALIDATE_LOG << ".m4s file too small: " << m4s_path;
    return false;
  }

  // First 4 bytes: Box size (big-endian)
  uint32_t box_size = boost::endian::big_to_native(*reinterpret_cast<uint32_t*>(header.data()));

  // Next 4 bytes: Box type (should be 'ftyp')
  std::string box_type(reinterpret_cast<char*>(header.data() + 4), 4);

  if (box_type != "ftyp")
  {
    LOG_ERROR << SERVER_VALIDATE_LOG << "Missing 'ftyp' header in .m4s: " << m4s_path;
    return false;
  }

  // Ensure the file contains 'moof' (Movie Fragment) and 'mdat' (Media Data)
  file.seekg(0, std::ios::beg);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  if (content.find("moof") == std::string::npos || content.find("mdat") == std::string::npos)
  {
    LOG_ERROR << SERVER_VALIDATE_LOG
              << "Invalid .m4s segment (missing 'moof' or 'mdat'): " << m4s_path;
    return false;
  }

  LOG_INFO << SERVER_VALIDATE_LOG << "Valid .m4s segment: " << m4s_path;
  return true;
}

auto extract_payload(const std::string& payload_path, const std::string& extract_path) -> bool
{
  LOG_INFO << SERVER_EXTRACT_LOG << "Extracting PAYLOAD: " << payload_path;

  struct archive*       a   = archive_read_new();
  struct archive*       ext = archive_write_disk_new();
  struct archive_entry* entry;

  archive_read_support_filter_gzip(a);
  archive_read_support_format_tar(a);
  archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_PERM);

  if (archive_read_open_filename(a, payload_path.c_str(), 10240) != ARCHIVE_OK)
  {
    LOG_ERROR << SERVER_EXTRACT_LOG << "Failed to open archive: " << archive_error_string(a);
    archive_read_free(a);
    archive_write_free(ext);
    return false;
  }

  bool valid_files_found = false;

  while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
  {
    std::string filename    = archive_entry_pathname(entry);
    std::string output_file = extract_path + "/" + filename;

    LOG_INFO << SERVER_EXTRACT_LOG
             << "Extracting file: " << fs::relative(output_file, macros::SERVER_STORAGE_DIR);

    archive_entry_set_pathname(entry, output_file.c_str());

    if (archive_write_header(ext, entry) == ARCHIVE_OK)
    {
      std::ofstream ofs(output_file, std::ios::binary);
      if (!ofs)
      {
        LOG_ERROR << SERVER_EXTRACT_LOG << "Failed to open file for writing: " << output_file;
        continue;
      }

      char    buffer[8192]; // maybe better off using std::array<>?
      ssize_t len;
      while ((len = archive_read_data(a, buffer, sizeof(buffer))) > 0)
      {
        ofs.write(buffer, len);
      }
      ofs.close();

      valid_files_found = true;

      // If the extracted file is a .zst file, decompress it
      if (output_file.substr(output_file.find_last_of(".") + 1) == macros::ZSTD_FILE_EXT)
      {
        LOG_INFO << "[ZSTD] Decompressing .zst file: "
                 << fs::relative(output_file, macros::SERVER_TEMP_STORAGE_DIR);
        if (!ZSTD_decompress_file(output_file.c_str()))
        {
          LOG_ERROR << "[ZSTD] Failed to decompress .zst file: " << output_file;
          continue;
        }

        std::string decompressed_filename =
          output_file.substr(0, output_file.find_last_of(".")); // remove .zst extension
        LOG_INFO << SERVER_EXTRACT_LOG << "Decompressed file: "
                 << fs::relative(decompressed_filename, macros::SERVER_TEMP_STORAGE_DIR);

        if (std::remove(output_file.c_str()) == 0)
        {
          LOG_INFO << "[ZSTD] Deleted the original .zst file: "
                   << fs::relative(output_file, macros::SERVER_TEMP_STORAGE_DIR);
        }
        else
        {
          LOG_ERROR << "[ZSTD] Failed to delete .zst file: "
                    << fs::relative(output_file, macros::SERVER_TEMP_STORAGE_DIR);
        }
      }
    }
  }

  archive_read_free(a);
  archive_write_free(ext);

  return valid_files_found;
}

auto extract_and_validate(const std::string& gzip_path, const std::string& audio_id,
                          const std::string& ip_id) -> bool
{
  LOG_INFO << SERVER_EXTRACT_LOG << "Validating and extracting GZIP file: " << gzip_path;

  if (!fs::exists(gzip_path))
  {
    LOG_ERROR << SERVER_EXTRACT_LOG << "File does not exist: " << gzip_path;
    return false;
  }

  std::string temp_extract_path =
    macros::to_string(macros::SERVER_TEMP_STORAGE_DIR) + "/" + audio_id;
  fs::create_directories(temp_extract_path);

  if (!extract_payload(gzip_path, temp_extract_path))
  {
    LOG_ERROR << SERVER_EXTRACT_LOG << "Extraction failed!";
    return false;
  }

  LOG_INFO << SERVER_EXTRACT_LOG << "Extraction complete, validating files...";

  // Move valid files to storage
  std::string storage_path =
    macros::to_string(macros::SERVER_STORAGE_DIR) + "/" + ip_id + "/" + audio_id;
  fs::create_directories(storage_path);

  int valid_file_count = 0;

  for (const fs::directory_entry& file : fs::directory_iterator(temp_extract_path))
  {
    std::string          fname = file.path().filename().string();
    std::ifstream        infile(file.path().string(), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(infile)), {});

    if (fname.ends_with(macros::PLAYLIST_EXT))
    {
      if (!validate_m3u8_format(std::string(data.begin(), data.end())))
      {
        LOG_WARNING << SERVER_EXTRACT_LOG << "Invalid M3U8 file, removing: " << fname;
        fs::remove(file.path());
        continue;
      }
    }
    else if (fname.ends_with(macros::TRANSPORT_STREAM_EXT))
    {
      if (!validate_ts_file(data))
      {
        LOG_WARNING << SERVER_EXTRACT_LOG << "Invalid TS file, removing: " << fname;
        fs::remove(file.path());
        continue;
      }
    }
    else if (fname.ends_with(macros::M4S_FILE_EXT))
    {
      if (!validate_m4s(file.path().string())) // Validate .m4s
      {
        LOG_WARNING << SERVER_EXTRACT_LOG << "Possibly invalid M4S segment: " << fname;
      }
    }
    else if (fname.ends_with(macros::MP4_FILE_EXT))
    {
      LOG_DEBUG << SERVER_EXTRACT_LOG << "Found MP4 file: " << fname;
    }
    else
    {
      LOG_WARNING << SERVER_EXTRACT_LOG << "Skipping unknown file: " << fname;
      fs::remove(file.path());
      continue;
    }

    // Move validated file to storage
    fs::rename(file.path(), storage_path + "/" + fname);
    LOG_INFO << SERVER_EXTRACT_LOG << "File stored in HLS storage: " << fname;
    valid_file_count++;
  }

  if (valid_file_count == 0)
  {
    LOG_ERROR << SERVER_EXTRACT_LOG << "No valid files remain after validation, extraction failed!";
    return false;
  }

  LOG_INFO << SERVER_EXTRACT_LOG << "Extraction and validation successful.";
  return true;
}

class HLS_Session : public std::enable_shared_from_this<HLS_Session>
{
public:
  explicit HLS_Session(boost::asio::ssl::stream<tcp::socket> socket, const std::string ip)
      : socket_(std::move(socket)), ip_id_(std::move(ip))
  {
  }

  void start()
  {
    LOG_INFO << SERVER_LOG << "Starting new session";
    do_handshake();
  }

private:
  boost::asio::ssl::stream<tcp::socket> socket_;
  beast::flat_buffer                    buffer_;
  http::request<http::string_body>      request_;
  std::string                           ip_id_;

  void do_handshake()
  {
    auto self(shared_from_this());
    socket_.async_handshake(boost::asio::ssl::stream_base::server,
                            [this, self](boost::system::error_code ec)
                            {
                              if (ec)
                              {
                                LOG_ERROR << SERVER_LOG << "SSL handshake failed: " << ec.message();
                                return;
                              }
                              LOG_INFO << SERVER_LOG << "SSL handshake successful";
                              do_read();
                            });
  }

  void resolve_ip()
  {
    try
    {
      ip_id_ = socket_.lowest_layer().remote_endpoint().address().to_string();
      LOG_INFO << SERVER_LOG << "Resolved IP: " << ip_id_;
    }
    catch (const std::exception& e)
    {
      LOG_ERROR << SERVER_LOG << "Failed to resolve IP: " << e.what();
      send_response(macros::to_string(macros::SERVER_ERROR_500));
      return;
    }

    do_read();
  }

  void do_read()
  {
    auto self(shared_from_this());

    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    parser->body_limit(WAVY_SERVER_AUDIO_SIZE_LIMIT * 1024 *
                       1024); // for now 200MiB is alright, when lossless codecs come
                              // in the picture we will have to think about it.

    http::async_read(
      socket_, buffer_, *parser,
      [this, self, parser](boost::system::error_code ec, std::size_t bytes_transferred)
      {
        if (ec)
        {
          LOG_ERROR << SERVER_LOG << "Read error: " << ec.message();
          if (ec == http::error::body_limit)
          {
            LOG_ERROR << SERVER_LOG << "Upload size exceeded the limit!";
            send_response(macros::to_string(macros::SERVER_ERROR_413));
          }
          return;
        }
        /* bytes_to_mib is a C FFI from common.h */
        LOG_INFO << SERVER_LOG << "Received " << bytes_to_mib(bytes_transferred) << " MiB ("
                 << bytes_transferred << ") bytes";
        request_ = parser->release();
        process_request();
      });

    // Timeout to prevent indefinite read
    boost::asio::socket_base::keep_alive option(true);
    socket_.next_layer().set_option(option);
  }

  void handle_list_ips()
  {
    LOG_INFO << "[List IPs] Handling IP listing request";

    std::string storage_path = macros::to_string(macros::SERVER_STORAGE_DIR);
    if (!fs::exists(storage_path) || !fs::is_directory(storage_path))
    {
      LOG_ERROR << "[List IPs] Storage directory not found: " << storage_path;
      send_response(macros::to_string(macros::SERVER_ERROR_500));
      return;
    }

    std::ostringstream response_stream;
    bool               entries_found = false;

    for (const fs::directory_entry& ip_entry : fs::directory_iterator(storage_path))
    {
      if (fs::is_directory(ip_entry.status()))
      {
        std::string ip_id = ip_entry.path().filename().string();
        response_stream << ip_id << ":\n"; // IP-ID Header

        bool audio_found = false;
        for (const fs::directory_entry& audio_entry : fs::directory_iterator(ip_entry.path()))
        {
          if (fs::is_directory(audio_entry.status()))
          {
            response_stream << "  - " << audio_entry.path().filename().string() << "\n"; // Audio-ID
            audio_found = true;
          }
        }

        if (!audio_found)
        {
          response_stream << "  (No audio IDs found)\n";
        }

        entries_found = true;
      }
    }

    if (!entries_found)
    {
      LOG_WARNING << "[List IPs] No IPs or Audio-IDs found in storage";
      send_response(macros::to_string(macros::SERVER_ERROR_404));
      return;
    }

    // Return the list of IP-IDs and their respective Audio-IDs
    send_response("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n" + response_stream.str());
  }

  void process_request()
  {
    if (request_.method() == http::verb::post)
    {
      // Check if this is a TOML file upload request.
      // For example, if the request target is "/toml/upload"...
      std::string target(request_.target().begin(), request_.target().end());
      if (target == "/toml/upload")
      {
        // Remove padding text before parsing
        std::string body = request_.body();
        auto        pos  = body.find(macros::NETWORK_TEXT_DELIM);
        if (pos != std::string::npos)
        {
          body = body.substr(pos + macros::NETWORK_TEXT_DELIM.length());
        }

        // Remove bottom padding text
        std::string bottom_delimiter = "--------------------------";
        auto        bottom_pos       = body.find(bottom_delimiter);
        if (bottom_pos != std::string::npos)
        {
          body = body.substr(0, bottom_pos);
        }

        // Call your TOML parsing function.
        auto toml_data_opt = parseAudioMetadataFromDataString(body);
        if (toml_data_opt.path.empty())
        {
          LOG_ERROR << "[TOML] Failed to parse TOML data";
          send_response(macros::to_string(macros::SERVER_ERROR_400));
          return;
        }

        send_response("HTTP/1.1 200 OK\r\n\r\nTOML parsed\r\n");
        return;
      }
      handle_upload();
    }
    else if (request_.method() == http::verb::get)
    {
      if (request_.target() == macros::SERVER_PATH_HLS_CLIENTS) // Request for all client IDs
      {
        handle_list_ips();
      }
      else
      {
        handle_download();
      }
    }
    else
    {
      send_response(macros::to_string(macros::SERVER_ERROR_405));
    }
  }

  void handle_upload()
  {
    LOG_INFO << SERVER_UPLD_LOG << "Handling GZIP file upload";

    std::string audio_id  = boost::uuids::to_string(boost::uuids::random_generator()());
    std::string gzip_path = macros::to_string(macros::SERVER_TEMP_STORAGE_DIR) + "/" + audio_id +
                            macros::to_string(macros::COMPRESSED_ARCHIVE_EXT);

    fs::create_directories(macros::SERVER_TEMP_STORAGE_DIR);
    std::ofstream output_file(gzip_path, std::ios::binary);
    if (!output_file)
    {
      LOG_ERROR << SERVER_UPLD_LOG << "Failed to open output file for writing: " << gzip_path;
      send_response(macros::to_string(macros::SERVER_ERROR_500));
      return;
    }

    output_file.write(request_.body().data(), request_.body().size());

    if (!output_file.good())
    {
      LOG_ERROR << SERVER_UPLD_LOG << "Failed to write data to file: " << gzip_path;
      send_response(macros::to_string(macros::SERVER_ERROR_500));
      return;
    }

    output_file.close();

    if (!fs::exists(gzip_path) || fs::file_size(gzip_path) == 0)
    {
      LOG_ERROR << SERVER_UPLD_LOG << "GZIP upload failed: File is empty or missing!";
      send_response("HTTP/1.1 400 Bad Request\r\n\r\nGZIP upload failed");
      return;
    }

    LOG_INFO << SERVER_UPLD_LOG << "File successfully written: " << gzip_path;

    if (extract_and_validate(gzip_path, audio_id, ip_id_))
    {
      send_response("HTTP/1.1 200 OK\r\nClient-ID: " + audio_id + "\r\n\r\n");
    }
    else
    {
      LOG_ERROR << SERVER_UPLD_LOG << "Extraction or validation failed!";
      send_response(macros::to_string(macros::SERVER_ERROR_400));
    }

    fs::remove(gzip_path);
  }

  /*
   * NOTE:
   *
   * Currently this is a **PERSISTENT** download approach.
   *
   * So previous clients storage is also present regardless of whether the server was restarted or
   * not.
   *
   * This means that unless the server's filesystem purposefully removes the hls storage, the
   * client's extracted data will *ALWAYS* be present in the server.
   *
   * TLDR: Previous server instances of client storage is persistent and can be accessed until they
   * are removed from server's filesystem.
   *
   */
  void handle_download()
  {
    // Parse request target (expected: /hls/<audio_id>/<filename>)
    std::string              target(request_.target().begin(), request_.target().end());
    std::vector<std::string> parts;
    std::istringstream       iss(target);
    std::string              token;

    while (std::getline(iss, token, '/'))
    {
      if (!token.empty())
      {
        parts.push_back(token);
      }
    }

    if (parts.size() < 4 || parts[0] != "hls")
    {
      LOG_ERROR << SERVER_DWNLD_LOG << "Invalid request path: " << target;
      send_response(macros::to_string(macros::SERVER_ERROR_400));
      return;
    }

    std::string ip_addr  = parts[1];
    std::string audio_id = parts[2];
    std::string filename = parts[3];

    // Construct the file path
    std::string file_path = macros::to_string(macros::SERVER_STORAGE_DIR) + "/" + ip_addr + "/" +
                            audio_id + "/" + filename;

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path))
    {
      LOG_ERROR << SERVER_DWNLD_LOG << "File not found: " << file_path;
      send_response(macros::to_string(macros::SERVER_ERROR_404));
      return;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file)
    {
      LOG_ERROR << SERVER_DWNLD_LOG << "Failed to open file: " << file_path;
      send_response(macros::to_string(macros::SERVER_ERROR_500));
      return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string file_content = buffer.str();

    std::string content_type = macros::to_string(macros::CONTENT_TYPE_OCTET_STREAM);
    if (filename.ends_with(macros::PLAYLIST_EXT))
    {
      content_type = "application/vnd.apple.mpegurl";
    }
    else if (filename.ends_with(macros::TRANSPORT_STREAM_EXT))
    {
      content_type = "video/mp2t";
    }

    // Use a shared_ptr to keep the response alive until async_write completes
    auto response = std::make_shared<http::response<http::string_body>>();
    response->version(request_.version());
    response->result(http::status::ok);
    response->set(http::field::server, "Wavy Server");
    response->set(http::field::content_type, content_type);
    response->body() = std::move(file_content);
    response->prepare_payload();

    auto self = shared_from_this(); // Keep session alive
    http::async_write(socket_, *response,
                      [this, self, response](boost::system::error_code ec, std::size_t)
                      {
                        if (ec)
                        {
                          LOG_ERROR << SERVER_DWNLD_LOG << "Write error: " << ec.message();
                        }
                        socket_.lowest_layer().close();
                      });

    LOG_INFO << SERVER_DWNLD_LOG << "[OWNER:" << ip_addr << "] Served: " << filename << " ("
             << audio_id << ")";
  }

  void send_response(const std::string& msg)
  {
    LOG_DEBUG << SERVER_LOG << "Attempting to send " << msg;
    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(msg),
                             [this, self, msg_size = msg.size()](boost::system::error_code ec,
                                                                 std::size_t bytes_transferred)
                             {
                               // Always perform shutdown, even on error
                               auto do_shutdown = [this, self]()
                               {
                                 socket_.async_shutdown(
                                   [this, self](boost::system::error_code shutdown_ec)
                                   {
                                     if (shutdown_ec)
                                     {
                                       LOG_ERROR << SERVER_LOG
                                                 << "Shutdown error: " << shutdown_ec.message();
                                     }
                                     socket_.lowest_layer().close();
                                   });
                               };

                               if (ec)
                               {
                                 LOG_ERROR << SERVER_LOG << "Write error: " << ec.message();
                                 do_shutdown();
                                 return;
                               }

                               if (bytes_transferred != msg_size)
                               {
                                 LOG_ERROR << SERVER_LOG
                                           << "Incomplete write: " << bytes_transferred << " of "
                                           << msg_size << " bytes";
                                 do_shutdown();
                                 return;
                               }

                               do_shutdown();
                             });
  }
};

class HLS_Server
{
public:
  HLS_Server(boost::asio::io_context& io_context, boost::asio::ssl::context& ssl_context,
             short port)
      : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), ssl_context_(ssl_context),
        signals_(io_context, SIGINT, SIGTERM, SIGHUP)
  {
    ensure_single_instance();
    LOG_INFO << SERVER_LOG << "Starting HLS server on port " << port;

    signals_.async_wait(
      [this](boost::system::error_code /*ec*/, int /*signo*/)
      {
        LOG_INFO << SERVER_LOG << "Termination signal received. Cleaning up...";
        cleanup();
        std::exit(0);
      });

    start_accept();
  }

  ~HLS_Server() { cleanup(); }

private:
  tcp::acceptor              acceptor_;
  boost::asio::ssl::context& ssl_context_;
  boost::asio::signal_set    signals_;
  int                        lock_fd_ = -1;

  void start_accept()
  {
    acceptor_.async_accept(
      [this](boost::system::error_code ec, tcp::socket socket)
      {
        if (ec)
        {
          LOG_ERROR << SERVER_LOG << "Accept failed: " << ec.message();
          return;
        }

        std::string ip = socket.remote_endpoint().address().to_string();
        LOG_INFO << SERVER_LOG << "Accepted new connection from " << ip;

        auto session = std::make_shared<HLS_Session>(
          boost::asio::ssl::stream<tcp::socket>(std::move(socket), ssl_context_), ip);
        session->start();
        start_accept();
      });
  }

  void ensure_single_instance()
  {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, macros::to_string(macros::SERVER_LOCK_FILE).c_str(),
                 sizeof(addr.sun_path) - 1);

    lock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lock_fd_ == -1)
    {
      throw std::runtime_error("Failed to create UNIX socket for locking");
    }

    if (bind(lock_fd_, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
      close(lock_fd_);
      throw std::runtime_error("Another instance is already running!");
    }

    LOG_INFO << SERVER_LOG << "Lock acquired: " << macros::SERVER_LOCK_FILE;
  }

  void cleanup()
  {
    if (lock_fd_ != -1)
    {
      close(lock_fd_);
      unlink(macros::to_string(macros::SERVER_LOCK_FILE).c_str());
      LOG_INFO << SERVER_LOG << "Lock file removed: " << macros::SERVER_LOCK_FILE;
      lock_fd_ = -1;
    }
  }
};

auto main() -> int
{
  try
  {
    logger::init_logging();
    boost::asio::io_context   io_context;
    boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);

    ssl_context.set_options(
      boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
      boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::single_dh_use);

    ssl_context.use_certificate_file(macros::to_string(macros::SERVER_CERT),
                                     boost::asio::ssl::context::pem);
    ssl_context.use_private_key_file(macros::to_string(macros::SERVER_PRIVATE_KEY),
                                     boost::asio::ssl::context::pem);

    HLS_Server server(io_context, ssl_context, WAVY_SERVER_PORT_NO);
    io_context.run();
  }
  catch (std::exception& e)
  {
    LOG_ERROR << SERVER_LOG << "Exception: " << e.what();
  }

  return 0;
}
