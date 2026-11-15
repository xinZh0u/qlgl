/**
 * @file    qlmedia_ffmpeg.c
 * @brief   QLMedia 的 FFmpeg 解码后端（M2：音频 + 视频）。
 *
 * 输出格式固定，界面层不需要认识 FFmpeg：
 *   - 音频：S16 交错 / 48kHz / 2ch（swresample 负责重采样与上/下混）；
 *   - 视频：RGBA8888（sws_scale），一帧一块紧凑缓冲。
 *
 * 解复用与解码都在**同一个工作线程**里被调用（QLMedia 的 worker），
 * 所以这里不需要锁；音/视频各有一条有界包队列，避免一个流把另一个饿死。
 */
#include <qlgl/qlgl.h>

#include "qlmedia_internal.h"

#if QL_FEAT_MEDIA_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define OUT_RATE 48000
#define OUT_CH   2
#define AQ_CAP   32
#define VQ_CAP   64

typedef struct {
    AVPacket *p[AQ_CAP > VQ_CAP ? AQ_CAP : VQ_CAP];
    int cap, head, tail, count;
} PktQ;

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *adec, *vdec;
    int              astream, vstream;   /* -1 = 没有 */

    AVFrame         *aframe, *vframe;
    SwrContext      *swr;
    struct SwsContext *vsws;
    int              vw, vh;

    PktQ             aq, vq;
    int              demux_eof;
    int              adrain_sent, adrain_done;
    int              vdrain_sent, vdrain_done;

    int64_t          duration_ms;
    int              rate, channels;

    int16_t         *pending;
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

static void q_init(PktQ *q, int cap)
{
    memset(q, 0, sizeof(*q));
    q->cap = cap;
}

static void q_clear(PktQ *q)
{
    while (q->count > 0) {
        av_packet_free(&q->p[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q->head = q->tail = q->count = 0;
}

static void q_push(PktQ *q, AVPacket *p)
{
    if (q->count >= q->cap) {
        /* 太慢：丢最旧的包，保证内存有界（视频马赛克好过 OOM） */
        av_packet_free(&q->p[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q->p[q->tail] = p;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
}

static AVPacket *q_pop(PktQ *q)
{
    if (q->count == 0)
        return NULL;
    AVPacket *p = q->p[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return p;
}

/* 再读一个包，按流塞进对应的队列；返回 1 = 读到了东西 */
static int ff_demux_more(FFMedia *f)
{
    if (f->demux_eof)
        return 0;
    AVPacket *p = av_packet_alloc();
    if (p == NULL) {
        f->demux_eof = 1;
        return 0;
    }
    const int r = av_read_frame(f->fmt, p);
    if (r < 0) {
        av_packet_free(&p);
        f->demux_eof = 1;
        return 0;
    }
    if (p->stream_index == f->astream)
        q_push(&f->aq, p);
    else if (p->stream_index == f->vstream)
        q_push(&f->vq, p);
    else
        av_packet_free(&p);
    return 1;
}

/* 给某个队列补包；EOF 时给解码器发一次 NULL 让它吐完 */
static void ff_fill_queue(FFMedia *f, PktQ *q, AVCodecContext *dec, int *sent)
{
    while (q->count == 0 && ff_demux_more(f))
        ;
    if (q->count == 0 && !*sent) {
        *sent = 1;
        (void)avcodec_send_packet(dec, NULL);
    }
}

static int ff_send_next(FFMedia *f, PktQ *q, AVCodecContext *dec, int *sent)
{
    if (q->count == 0)
        ff_fill_queue(f, q, dec, sent);
    AVPacket *p = q_pop(q);
    if (p == NULL)
        return 0;
    const int r = avcodec_send_packet(dec, p);
    av_packet_free(&p);
    return (r >= 0 || r == AVERROR(EAGAIN)) ? 1 : -1;
}

static int ff_open_decoder(AVFormatContext *fmt, int stream, AVCodecContext **out)
{
    if (stream < 0)
        return 0;
    AVStream *st = fmt->streams[stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == NULL)
        return -1;
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (dec == NULL)
        return -1;
    if (avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
        avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        return -1;
    }
    dec->pkt_timebase = st->time_base;
    *out = dec;
    return 0;
}

static void *ff_open(const char *src, char *err, size_t err_cap)
{
    FFMedia *f = (FFMedia *)ql_mem_calloc(1, sizeof(*f));
    if (f == NULL) {
        ff_set_err(err, err_cap, "out of memory", 0);
        return NULL;
    }
    f->astream = f->vstream = -1;

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

    for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
        const enum AVMediaType t = f->fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_AUDIO && f->astream < 0)
            f->astream = (int)i;
        else if (t == AVMEDIA_TYPE_VIDEO && f->vstream < 0)
            f->vstream = (int)i;
    }
    if (f->astream < 0 && f->vstream < 0) {
        ff_set_err(err, err_cap, "no audio/video stream in this file", 0);
        goto fail;
    }

    if (ff_open_decoder(f->fmt, f->astream, &f->adec) < 0 &&
        f->astream >= 0) {
        ff_set_err(err, err_cap, "cannot open audio decoder", 0);
        goto fail;
    }
    if (ff_open_decoder(f->fmt, f->vstream, &f->vdec) < 0 && f->vstream >= 0) {
        QL_LOGW("media: cannot open video decoder, audio only");
        f->vstream = -1;
    }

    f->aframe = av_frame_alloc();
    f->vframe = av_frame_alloc();
    if (f->aframe == NULL || f->vframe == NULL) {
        ff_set_err(err, err_cap, "out of memory (AVFrame)", 0);
        goto fail;
    }
    q_init(&f->aq, AQ_CAP);
    q_init(&f->vq, VQ_CAP);

    /* 音频：S16 / 48k / 2ch */
    if (f->adec != NULL) {
        AVChannelLayout out_layout, in_layout;
        av_channel_layout_default(&out_layout, OUT_CH);
        if (f->adec->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &f->adec->ch_layout);
        else
            av_channel_layout_default(&in_layout, 2);

        ret = swr_alloc_set_opts2(&f->swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                  &in_layout, f->adec->sample_fmt,
                                  f->adec->sample_rate > 0 ? f->adec->sample_rate
                                                           : OUT_RATE,
                                  0, NULL);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (ret < 0 || f->swr == NULL || swr_init(f->swr) < 0) {
            ff_set_err(err, err_cap, "swr init failed", ret);
            goto fail;
        }
        f->rate = OUT_RATE;
        f->channels = OUT_CH;
    } else {
        f->rate = OUT_RATE;
        f->channels = OUT_CH;
    }

    /* 视频：解码尺寸 + RGBA8888 转换器 */
    if (f->vdec != NULL) {
        f->vw = f->vdec->width > 0 ? f->vdec->width : f->vdec->coded_width;
        f->vh = f->vdec->height > 0 ? f->vdec->height : f->vdec->coded_height;
        if (f->vw <= 0 || f->vh <= 0) {
            QL_LOGW("media: video has no size, disabled");
            avcodec_free_context(&f->vdec);
            f->vstream = -1;
        } else {
            f->vsws = sws_getContext(f->vw, f->vh, f->vdec->pix_fmt, f->vw, f->vh,
                                     AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
            if (f->vsws == NULL) {
                QL_LOGW("media: sws init failed, video disabled");
                avcodec_free_context(&f->vdec);
                f->vstream = -1;
            }
        }
    }

    AVStream *st = f->fmt->streams[f->astream >= 0 ? f->astream : f->vstream];
    if (st->duration > 0)
        f->duration_ms = av_rescale_q(st->duration, st->time_base,
                                      (AVRational){ 1, 1000 });
    else if (f->fmt->duration > 0)
        f->duration_ms = f->fmt->duration / 1000;
    return f;

fail:
    if (f->swr != NULL) swr_free(&f->swr);
    if (f->vsws != NULL) sws_freeContext(f->vsws);
    if (f->aframe != NULL) av_frame_free(&f->aframe);
    if (f->vframe != NULL) av_frame_free(&f->vframe);
    q_clear(&f->aq);
    q_clear(&f->vq);
    if (f->adec != NULL) avcodec_free_context(&f->adec);
    if (f->vdec != NULL) avcodec_free_context(&f->vdec);
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
    if (f->vsws != NULL) sws_freeContext(f->vsws);
    if (f->aframe != NULL) av_frame_free(&f->aframe);
    if (f->vframe != NULL) av_frame_free(&f->vframe);
    q_clear(&f->aq);
    q_clear(&f->vq);
    if (f->adec != NULL) avcodec_free_context(&f->adec);
    if (f->vdec != NULL) avcodec_free_context(&f->vdec);
    if (f->fmt != NULL) avformat_close_input(&f->fmt);
    ql_mem_free(f->pending);
    ql_mem_free(f);
}

static int64_t ff_duration_ms(void *h) { return ((FFMedia *)h)->duration_ms; }
static int ff_has_audio(void *h) { return ((FFMedia *)h)->adec != NULL; }
static int ff_has_video(void *h) { return ((FFMedia *)h)->vdec != NULL; }
static int ff_rate(void *h) { return ((FFMedia *)h)->rate; }
static int ff_channels(void *h) { return ((FFMedia *)h)->channels; }

static int ff_video_size(void *h, int *w, int *hh)
{
    FFMedia *f = (FFMedia *)h;
    if (f->vdec == NULL)
        return -1;
    if (w != NULL) *w = f->vw;
    if (hh != NULL) *hh = f->vh;
    return 0;
}

static int ff_convert_audio(FFMedia *f)
{
    const int cap = OUT_RATE * 4;
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
                                (const uint8_t *const *)f->aframe->extended_data,
                                f->aframe->nb_samples);
    if (got < 0)
        return -1;
    f->pending_frames = got;
    f->pending_pos = 0;
    return got;
}

static int ff_read_audio(void *h, int16_t *buf, int max_frames)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL || f->adec == NULL || buf == NULL || max_frames <= 0)
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
        if (f->adrain_done)
            break;

        int ret = avcodec_receive_frame(f->adec, f->aframe);
        if (ret == 0) {
            const int got = ff_convert_audio(f);
            av_frame_unref(f->aframe);
            if (got < 0)
                return -1;
            continue;
        }
        if (ret == AVERROR_EOF) {
            f->adrain_done = 1;
            continue;
        }
        if (ret != AVERROR(EAGAIN))
            return -1;

        const int sent = ff_send_next(f, &f->aq, f->adec, &f->adrain_sent);
        if (sent < 0)
            return -1;
        if (sent == 0 && f->aq.count == 0 && f->demux_eof && f->adrain_sent) {
            f->adrain_done = 1;
        }
    }
    return produced;
}

static int ff_read_video(void *h, uint8_t *dst, int dst_bytes,
                         int *out_w, int *out_h, int64_t *pts_us)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL || f->vdec == NULL || dst == NULL)
        return -1;
    const int need = f->vw * f->vh * 4;
    if (dst_bytes < need)
        return -1;

    for (;;) {
        int ret = avcodec_receive_frame(f->vdec, f->vframe);
        if (ret == 0) {
            uint8_t *planes[4] = { dst, NULL, NULL, NULL };
            int strides[4] = { f->vw * 4, 0, 0, 0 };
            sws_scale(f->vsws, (const uint8_t *const *)f->vframe->data,
                      f->vframe->linesize, 0, f->vh, planes, strides);
            int64_t ts = f->vframe->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = 0;
            *pts_us = av_rescale_q(ts, f->fmt->streams[f->vstream]->time_base,
                                   (AVRational){ 1, 1000000 });
            if (out_w != NULL) *out_w = f->vw;
            if (out_h != NULL) *out_h = f->vh;
            av_frame_unref(f->vframe);
            return 1;
        }
        if (ret == AVERROR_EOF) {
            f->vdrain_done = 1;
            return 0;
        }
        if (ret != AVERROR(EAGAIN))
            return -1;

        const int sent = ff_send_next(f, &f->vq, f->vdec, &f->vdrain_sent);
        if (sent < 0)
            return -1;
        if (sent == 0 && f->vq.count == 0 && f->demux_eof && f->vdrain_sent) {
            f->vdrain_done = 1;
            return 0;
        }
    }
}

static QLErr ff_seek(void *h, int64_t ms)
{
    FFMedia *f = (FFMedia *)h;
    if (f == NULL)
        return QL_ERR_INVAL;
    if (ms < 0)
        ms = 0;

    const int any = (f->astream >= 0) ? f->astream : f->vstream;
    AVStream *st = f->fmt->streams[any];
    const int64_t ts = av_rescale_q(ms * 1000, (AVRational){ 1, 1000000 },
                                    st->time_base);
    if (av_seek_frame(f->fmt, any, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return QL_ERR_IO;

    if (f->adec != NULL) avcodec_flush_buffers(f->adec);
    if (f->vdec != NULL) avcodec_flush_buffers(f->vdec);
    if (f->aframe != NULL) av_frame_unref(f->aframe);
    if (f->vframe != NULL) av_frame_unref(f->vframe);
    q_clear(&f->aq);
    q_clear(&f->vq);
    f->demux_eof = 0;
    f->adrain_sent = f->adrain_done = 0;
    f->vdrain_sent = f->vdrain_done = 0;
    f->pending_frames = 0;
    f->pending_pos = 0;
    if (f->swr != NULL) {
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
    .video_size = ff_video_size,
    .read_video = ff_read_video,
    .seek = ff_seek,
};

const QLMediaBackend *ql_media_backend_ffmpeg(void)
{
    return &s_ffmpeg_backend;
}

#endif /* QL_FEAT_MEDIA_FFMPEG */
