#include <plugins/plugin.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>

#include "tms_ffmpeg_h264.h"
#include "tms_ffmpeg_mp4.h"
#include "tms_ffmpeg_pcma.h"
#include "tms_ffmpeg_stream.h"

/***********************************
 * 解析mp4文件，通过janus进行转发
 * 
 * 输出h264和pcma
 * 支持播放控制，暂停，恢复，停止 
 ***********************************/
/* 初始化播放器上下文对象 */
static int tms_init_play_context(TmsPlayContext *play)
{
  play->start_time_us = av_gettime_relative(); // 单位是微秒
  play->end_time_us = 0;
  play->pause_duration_us = 0;
  play->nb_packets = 0;
  play->nb_video_packets = 0;
  play->nb_audio_packets = 0;
  play->nb_audio_frames = 0;
  play->nb_pcma_frames = 0;

  return 0;
}
/* 释放输入流中的资源 */
static void tms_free_input_streams(TmsInputStream **ists, int nb_streams)
{
  int i = 0;
  for (; i < nb_streams; i++)
  {
    free(ists[i]);
  }
}
/* 打开指定的文件，获得媒体流信息 */
static int tms_open_file(char *filename, AVFormatContext **ictx, AVBSFContext **h264bsfc, Resampler *resampler, PCMAEnc *pcma_enc, TmsInputStream **ists, int *out_nb_streams)
{
  int ret = 0;

  /* 打开指定的媒体文件 */
  if ((ret = avformat_open_input(ictx, filename, NULL, NULL)) < 0)
  {
    JANUS_LOG(LOG_VERB, "无法打开媒体文件 %s\n", filename);
    return -1;
  }

  /* 获得指定的视频文件的信息 */
  if ((ret = avformat_find_stream_info(*ictx, NULL)) < 0)
  {
    JANUS_LOG(LOG_VERB, "无法获取媒体文件信息 %s\n", filename);
    return -1;
  }

  int nb_streams = (*ictx)->nb_streams;

  JANUS_LOG(LOG_VERB, "媒体文件 %s nb_streams = %d , duration = %s\n", filename, nb_streams, av_ts2str((*ictx)->duration));

  int i = 0;
  for (; i < nb_streams; i++)
  {
    TmsInputStream *ist = malloc(sizeof(TmsInputStream));
    tms_init_input_stream(*ictx, i, ist);
    tms_dump_stream_format(ist);

    ists[i] = ist;

    if (ist->codec->type == AVMEDIA_TYPE_VIDEO)
    {
      const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
      ret = av_bsf_alloc(filter, h264bsfc);
      avcodec_parameters_copy((*h264bsfc)->par_in, ist->st->codecpar);
      av_bsf_init(*h264bsfc);
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      if ((ret = tms_init_pcma_encoder(pcma_enc)) < 0)
      {
        return -1;
      }
      /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
      if ((ret = tms_init_audio_resampler(ist->dec_ctx, pcma_enc->cctx, resampler)) < 0)
      {
        return -1;
      }
    }
  }

  *out_nb_streams = nb_streams;

  return 0;
}

/*************************************
 * 执行入口 
 *************************************/
int tms_ffmpeg_mp4_main(char *filename)
{
  int ret = 0;

  TmsInputStream *ists[2]; // 记录媒体流信息
  AVFormatContext *ictx = NULL;
  AVBSFContext *h264bsfc; // mp4转h264，将sps和pps放到推送流中

  /* 音频重采样 */
  Resampler resampler = {.max_nb_samples = 0};
  PCMAEnc pcma_enc = {.nb_samples = 0};
  int nb_streams = 0; // 媒体流的数

  if ((ret = tms_open_file(filename, &ictx, &h264bsfc, &resampler, &pcma_enc, ists, &nb_streams)) < 0)
  {
    goto clean;
  }

  /* 初始化播放状态 */
  TmsPlayContext play;
  if ((ret = tms_init_play_context(&play)) < 0)
  {
    goto clean;
  }

  /* 解析文件开始播放 */
  AVPacket *pkt = av_packet_alloc(); // ffmpeg媒体包
  AVFrame *frame = av_frame_alloc(); // ffmpeg媒体帧

  while (1)
  {
    /**
     * 从文件中读取编码数据包
     */
    play.nb_packets++;
    if ((ret = av_read_frame(ictx, pkt)) == AVERROR_EOF)
    {
      play.end_time_us = av_gettime_relative();
      break;
    }
    else if (ret < 0)
    {
      JANUS_LOG(LOG_VERB, "读取媒体包 #%d 失败 %s\n", play.nb_packets, av_err2str(ret));
      av_packet_unref(pkt);
      goto clean;
    }
    /**
     * 分别处理音视频包
     */
    TmsInputStream *ist = ists[pkt->stream_index];
    if (ist->codec->type == AVMEDIA_TYPE_VIDEO)
    {
      if ((ret = tms_handle_video_packet(&play, ist, pkt, h264bsfc)) < 0)
      {
        av_packet_unref(pkt);
        goto clean;
      }
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      if ((ret = tms_handle_audio_packet(&play, ist, &resampler, &pcma_enc, pkt, frame)) < 0)
      {
        av_packet_unref(pkt);
        goto clean;
      }
    }

    av_packet_unref(pkt);
  }

end:
  /* Log end */
  JANUS_LOG(LOG_VERB, "完成文件播放 %s，共读取 %d 个包，包含：%d 个视频包，%d 个音频包，%d 个音频帧，转换 %d 个PCMA音频帧，用时：%ld微秒\n", filename, play.nb_packets, play.nb_video_packets, play.nb_audio_packets, play.nb_audio_frames, play.nb_pcma_frames, play.end_time_us - play.start_time_us);

clean:
  if (nb_streams > 0)
    tms_free_input_streams(ists, nb_streams);

  if (frame)
    av_frame_free(&frame);

  if (pkt)
    av_packet_free(&pkt);

  if (h264bsfc)
    av_bsf_free(&h264bsfc);

  if (resampler.swrctx)
    swr_free(&resampler.swrctx);

  if (ictx)
    avformat_close_input(&ictx);

  JANUS_LOG(LOG_VERB, "退出播放mp4\n");

  return 0;
}