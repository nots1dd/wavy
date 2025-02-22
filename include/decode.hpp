#pragma once

#include "../include/macros.hpp"
#include <iostream>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

// Custom AVIO read function
static int custom_read_packet(void* opaque, uint8_t* buf, int buf_size)
{
  auto*         segments      = static_cast<std::vector<std::string>*>(opaque);
  static size_t segment_index = 0;
  static size_t read_offset   = 0;

  if (segment_index >= segments->size())
  {
    return AVERROR_EOF; // No more data
  }

  size_t segment_size  = (*segments)[segment_index].size();
  size_t bytes_to_copy = std::min(static_cast<size_t>(buf_size), segment_size - read_offset);

  memcpy(buf, (*segments)[segment_index].data() + read_offset, bytes_to_copy);
  read_offset += bytes_to_copy;

  if (read_offset >= segment_size)
  {
    // Move to the next segment
    segment_index++;
    read_offset = 0;
  }

  return bytes_to_copy;
}

/**
 * @class MediaDecoder
 * @brief Decodes transport stream audio for playback from a vector
 */
class MediaDecoder
{
public:
  MediaDecoder() {}

  /**
   * @brief Decodes TS content to raw audio output
   * @param ts_segments Vector of transport stream segments
   * @return true if successful, false otherwise
   */
  bool decode(const std::vector<std::string>& ts_segments, std::vector<unsigned char>& output_audio)
  {
    AVFormatContext* input_ctx = avformat_alloc_context();
    AVIOContext*     avio_ctx  = nullptr;
    int              ret;

    // Buffer for custom AVIO
    unsigned char* avio_buffer = static_cast<unsigned char*>(av_malloc(4096));
    if (!avio_buffer)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to allocate AVIO buffer\n");
      return false;
    }

    // Create custom AVIOContext
    avio_ctx = avio_alloc_context(avio_buffer, 4096, 0, (void*)&ts_segments, &custom_read_packet,
                                  nullptr, nullptr);
    if (!avio_ctx)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to allocate AVIO context\n");
      av_free(avio_buffer);
      return false;
    }

    input_ctx->pb = avio_ctx;

    // Open input
    if ((ret = avformat_open_input(&input_ctx, nullptr, nullptr, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Cannot open input from memory buffer\n");
      av_free(avio_ctx->buffer);
      avio_context_free(&avio_ctx);
      avformat_free_context(input_ctx);
      return false;
    }

    // Get stream information
    if ((ret = avformat_find_stream_info(input_ctx, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Cannot find stream info\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    // Log detected format
    if (input_ctx->iformat && input_ctx->iformat->name)
    {
      av_log(nullptr, AV_LOG_INFO, "Detected format: %s\n", input_ctx->iformat->name);
    }
    else
    {
      av_log(nullptr, AV_LOG_WARNING, "Could not detect format\n");
    }

    // Check if input is MPEG-TS or fMP4 (m4s)
    bool is_mpegts = strcmp(input_ctx->iformat->name, "mpegts") == 0;
    bool is_m4s    = strstr(input_ctx->iformat->name, "mp4") != nullptr;

    if (is_mpegts)
    {
      av_log(nullptr, AV_LOG_INFO, "Input is an MPEG transport stream\n");
    }
    else if (is_m4s)
    {
      av_log(nullptr, AV_LOG_INFO, "Input is a fragmented MP4 (m4s) file\n");
    }
    else
    {
      av_log(nullptr, AV_LOG_WARNING, "Unknown or unsupported format detected\n");
    }

    // Find audio stream
    int audio_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_idx < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Cannot find audio stream\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    AVStream*          audio_stream = input_ctx->streams[audio_stream_idx];
    AVCodecParameters* codec_params = audio_stream->codecpar;

    // Check if the codec is FLAC (for fMP4/m4s streams)
    bool is_flac = (codec_params->codec_id == AV_CODEC_ID_FLAC);
    if (is_m4s || is_flac)
    {
      av_log(nullptr, AV_LOG_INFO, "Detected FLAC encoding in fragmented MP4 (m4s)\n");
    }

    // Open decoder
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec)
    {
      av_log(nullptr, AV_LOG_ERROR, "Unsupported codec\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to allocate codec context\n");
      avformat_close_input(&input_ctx);
      return false;
    }

    if ((ret = avcodec_parameters_to_context(codec_ctx, codec_params)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to copy codec parameters to context\n");
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&input_ctx);
      return false;
    }

    if ((ret = avcodec_open2(codec_ctx, codec, nullptr)) < 0)
    {
      av_log(nullptr, AV_LOG_ERROR, "Failed to open codec\n");
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&input_ctx);
      return false;
    }

    // Extract raw audio data
    AVPacket packet;
    AVFrame* frame = av_frame_alloc();
    while (av_read_frame(input_ctx, &packet) >= 0)
    {
      if (packet.stream_index == audio_stream_idx)
      {
        output_audio.insert(output_audio.end(), packet.data, packet.data + packet.size);
      }
      av_packet_unref(&packet);
    }

    // Cleanup
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&input_ctx);

    return true;
  }
};

/*auto main() -> int*/
/*{*/
/*  GlobalState gs;*/
/**/
/*  std::vector<std::string> ts_segments = gs.transport_segments; */
/**/
/*  if (ts_segments.empty())*/
/*  {*/
/*    av_log(nullptr, AV_LOG_ERROR, "No transport stream segments provided\n");*/
/*    return 1;*/
/*  }*/
/**/
/*  TSDecoder decoder;*/
/**/
/*  if (!decoder.decode_ts(ts_segments))*/
/*  {*/
/*    av_log(nullptr, AV_LOG_ERROR, "Decoding failed\n");*/
/*    return 1;*/
/*  }*/
/**/
/*  return 0;*/
/*}*/
