/**
 * @file    qlmedia_ffmpeg.c
 * @brief   QLMedia 的 FFmpeg 解码后端（M1：音频）。
 *
 * 只做三件事：
 *   1. 容器/解码器打开（同步，`ql_media_open()` 时完成）；
 *   2. 解码 + 重采样成 **S16 交错 / 48kHz / 2ch**；
 *   3. seek（`av_seek_frame` + flush）。
 *
 * 网络流、硬件解码、字幕轨都不在这里（见 doc/媒体播放设计.md 的 M2+）。
 * 所有 AV* 对象都归本文件所有；`read_audio()` 只被工作线程调用。
 */
#include <qlgl/qlgl.h>

#include "qlmedia_internal.h"

#if QL_FEAT_MEDIA_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define OUT_RATE 48000
#define OUT_CH   2

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    int              stream;
    AVPacket        *pkt;
    AVFrame         *frame;
    SwrContext      *swr;

    int64_t          duration_ms;
    int              rate, channels;

    int              demux_eof;
    int              drain_sent;
    int              drain_done;

    int16_t         *pending;      /* 重采样后但还没交给调用方的样本 */
    int              pending_frames;
    int              pending_pos;
    int              pending_cap;
} FFMedia;

static void ff_set_err(char *err, size_t cap, const char *what, int avret)
{
    if (err == NULL || cap == 0)
        return;
    char tmp[96] = { 0 };
    if (avret != 0)
        av_strerror(avret, tmp, sizeof(tmp));
    snprintf(err, cap, "%s%s%s", what, (tmp[0] != '\0') ? ": " : "",
             (tmp[0] != '\0') ? tmp : "");
}

static void *ff_open(const char *src, char *err, size_t err_cap)
{
    FFMedia *f = (FFMedia *)ql_mem_calloc(1, sizeof(*f));
    if (f == NULL) {
        ff_set_err(err, err_cap, "out of memory", 0);
        return NULL;
    }

    int ret = avformat_open_input(&f->fmt, src, NULL, NULL);
    if (ret < 0) {
        ff_set_err(err, err_cap, "avformat_open_input failed", ret);
        goto fail;
    }
    ret = avformat_find_stream_info(f->fmt, NULL);
    if (ret < 0) {
        ff_set_err(err, err_cap, "avformat_find_stream_info failed", ret);
        goto fail;
    }

    f->stream = -1;
    for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
        if (f->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            f->stream = (int)i;
            break;
        }
    }
    if (f->stream < 0) {
        ff_set_err(err, err_cap, "no audio stream in this file", 0);
        goto fail;
    }

    AVStream *st = f->fmt->streams[f->stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == NULL) {
        ff_set_err(err, err_cap, "unsupported audio codec", 0);
        goto fail;
    }
    f->dec = avcodec_alloc_context3(codec);
    if (f->dec == NULL) {
        ff_set_err(err, err_cap, "avcodec_alloc_context3 failed", 0);
        goto fail;
    }
    ret = avcodec_parameters_to_context(f->dec, st->codecpar);
    if (ret < 0) {
        ff_set_err(err, err_cap, "avcodec_parameters_to_context failed", ret);
        goto fail;
    }
    f->dec->pkt_timebase = st->time_base;
    ret = avcodec_open2(f->dec, codec, NULL);
    if (ret < 0) {
        ff_set_err(err, err_cap, "avcodec_open2 failed", ret);
        goto fail;
    }

    f->pkt = av_packet_alloc();
    f->frame = av_frame_alloc();
    if (f->pkt == NULL || f->frame == NULL) {
        ff_set_err(err, err_cap, "out of memory (AVPacket/AVFrame)", 0);
        goto fail;
    }

    /* 输出格式固定：S16 交错 / 48kHz / 立体声 */
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, OUT_CH);

    AVChannelLayout in_layout;
    if (f->dec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &f->dec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);

    ret = swr_alloc_set_opts2(&f->swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                              &in_layout, f->dec->sample_fmt,
                              f->dec->sample_rate > 0 ? f->dec->sample_rate : OUT_RATE,
                              0, NULL);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || f->swr == NULL) {
        ff_set_err(err, err_cap, "swr_alloc_set_opts2 failed", ret);
        goto fail;
    }
    if (swr_init(f->swr) < 0) {
        ff_set_err(err, err_cap, "swr_init failed", 0);
        goto fail;
    }

    f->rate = OUT_RATE;
    f->channels = OUT_CH;

    if (st->duration > 0)
        f->duration_ms = av_rescale_q(st->duration, st->time_base,
                                      (AVRational){ 1, 1000 });
    else if (f->fmt->duration > 0)
        f->duration_ms = f->fmt->duration / 1000;   /* AV_TIME_BASE = 1e6 */
    return f;

fail:
    if (f->swr != NULL) swr_free(&f->swr);
    if (f->frame != NULL) av_frame_free(&f->frame);
    if (f->pkt != NULL) av_packet_free(&f->pkt);
    if (f->dec != NULL) avcodec_free_context(&f->dec);
    if (f->fmt != NULL) avformat_close_input(&f->fmt);
    ql_mem_free(f);
    return NULL;
}

static void ff_close(void *h)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL)
        return;
    if (f->swr != NULL) swr_free(&f->swr);
    if (f->frame != NULL) av_frame_free(&f->frame);
    if (f->pkt != NULL) av_packet_free(&f->pkt);
    if (f->dec != NULL) avcodec_free_context(&f->dec);
    if (f->fmt != NULL) avformat_close_input(&f->fmt);
    ql_mem_free(f->pending);
    ql_mem_free(f);
}

static int64_t ff_duration_ms(void *h) { return ((FFMedia *)h)->duration_ms; }
static int ff_has_audio(void *h) { (void)h; return 1; }
static int ff_has_video(void *h)
{
    FFMedia *f = (FFMedia *)h;
    for (unsigned i = 0; i < f->fmt->nb_streams; i++)
        if (f->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            return 1;
    return 0;
}
static int ff_rate(void *h) { return ((FFMedia *)h)->rate; }
static int ff_channels(void *h) { return ((FFMedia *)h)->channels; }

static int ff_convert_frame(FFMedia *f)
{
    const int cap = OUT_RATE * 4;      /* 一帧最多 4 秒，足够任何解码器 */
    if (f->pending_cap < cap) {
        ql_mem_free(f->pending);
        f->pending = (int16_t *)ql_mem_alloc((size_t)cap * sizeof(int16_t));
        if (f->pending == NULL) {
            f->pending_cap = 0;
            return -1;
        }
        f->pending_cap = cap;
    }

    uint8_t *out = (uint8_t *)f->pending;
    const int got = swr_convert(f->swr, &out, cap,
                                (const uint8_t *const *)f->frame->extended_data,
                                f->frame->nb_samples);
    if (got < 0)
        return -1;
    f->pending_frames = got;
    f->pending_pos = 0;
    return got;
}

static int ff_read_audio(void *h, int16_t *buf, int max_frames)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL || buf == NULL || max_frames <= 0)
        return -1;

    int produced = 0;
    while (produced < max_frames) {
        if (f->pending_pos < f->pending_frames) {
            int n = f->pending_frames - f->pending_pos;
            if (n > max_frames - produced)
                n = max_frames - produced;
            memcpy(buf + (size_t)produced * f->channels,
                   f->pending + (size_t)f->pending_pos * f->channels,
                   (size_t)n * f->channels * sizeof(int16_t));
            f->pending_pos += n;
            produced += n;
            continue;
        }
        if (f->drain_done)
            break;

        int ret = avcodec_receive_frame(f->dec, f->frame);
        if (ret == 0) {
            int got = ff_convert_frame(f);
            av_frame_unref(f->frame);
            if (got < 0)
                return -1;
            if (got == 0)
                continue;
            continue;
        }
        if (ret == AVERROR_EOF) {
            f->drain_done = 1;
            continue;
        }
        if (ret != AVERROR(EAGAIN))
            return -1;

        if (f->demux_eof) {
            /* 让解码器把内部缓冲吐完；只发一次 NULL，防解码器一直 EAGAIN */
            if (f->drain_sent) {
                f->drain_done = 1;
                continue;
            }
            f->drain_sent = 1;
            ret = avcodec_send_packet(f->dec, NULL);
            (void)ret;
            continue;
        }

        av_packet_unref(f->pkt);
        ret = av_read_frame(f->fmt, f->pkt);
        if (ret < 0) {
            f->demux_eof = 1;
            continue;
        }
        if (f->pkt->stream_index != f->stream) {
            av_packet_unref(f->pkt);
            continue;
        }
        ret = avcodec_send_packet(f->dec, f->pkt);
        av_packet_unref(f->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return -1;
    }

    return produced;
}

static QLErr ff_seek(void *h, int64_t ms)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL)
        return QL_ERR_INVAL;
    if (ms < 0)
        ms = 0;

    AVStream *st = f->fmt->streams[f->stream];
    const int64_t ts = av_rescale_q(ms * 1000, (AVRational){ 1, 1000000 },
                                    st->time_base);
    if (av_seek_frame(f->fmt, f->stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return QL_ERR_IO;

    avcodec_flush_buffers(f->dec);
    av_packet_unref(f->pkt);
    av_frame_unref(f->frame);
    f->demux_eof = 0;
    f->drain_sent = 0;
    f->drain_done = 0;
    f->pending_frames = 0;
    f->pending_pos = 0;
    if (f->swr != NULL) {
        /* 丢掉重采样器里的旧尾巴；失败也不致命，继续按新位置解 */
        swr_close(f->swr);
        (void)swr_init(f->swr);
    }
    return QL_OK;
}

static const QLMediaBackend s_ffmpeg_backend = {
    .name = "ffmpeg",
    .open = ff_open,
    .close = ff_close,
    .duration_ms = ff_duration_ms,
    .has_audio = ff_has_audio,
    .has_video = ff_has_video,
    .rate = ff_rate,
    .channels = ff_channels,
    .read_audio = ff_read_audio,
    .seek = ff_seek,
};

const QLMediaBackend *ql_media_backend_ffmpeg(void)
{
    return &s_ffmpeg_backend;
}

#endif /* QL_FEAT_MEDIA_FFMPEG */
