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
    int              vwait_key;      /* seek 之后先解到关键帧再出图 */
    int              vdrop;          /* 等关键帧时丢掉的帧数（防永久等待） */
    int64_t          seek_target_us; /* 本次 seek 的目标（回退时用） */
    int64_t          vskip_to_us;    /* 从 0 重解：跳过此前的帧（<0 = 不跳） */

    int64_t          duration_ms;
    int              rate, channels;

    int16_t         *pending;
    int              pending_frames;
    int              pending_pos;
    int              pending_cap;

    /* 容器内嵌字幕（打开时一次性解好；与播放解复用无关） */
    struct SubCue { char *text; int64_t start_ms, end_ms; } *subs;
    int              sub_count, sub_cap;
} FFMedia;

static void sub_add(FFMedia *f, const char *text, int64_t start_ms, int64_t end_ms)
{
    if (text == NULL || text[0] == '\0' || f->sub_count >= QL_CFG_MEDIA_CUES)
        return;
    if (f->sub_count >= f->sub_cap) {
        const int cap = (f->sub_cap > 0) ? f->sub_cap * 2 : 32;
        struct SubCue *p = (struct SubCue *)ql_mem_alloc((size_t)cap * sizeof(*p));
        if (p == NULL)
            return;
        if (f->sub_count > 0)
            memcpy(p, f->subs, (size_t)f->sub_count * sizeof(*p));
        ql_mem_free(f->subs);
        f->subs = p;
        f->sub_cap = cap;
    }
    const size_t n = strlen(text);
    char *copy = (char *)ql_mem_alloc(n + 1);
    if (copy == NULL)
        return;
    memcpy(copy, text, n + 1);
    f->subs[f->sub_count].text = copy;
    f->subs[f->sub_count].start_ms = start_ms;
    f->subs[f->sub_count].end_ms = end_ms;
    f->sub_count++;
}

/* 把 ASS / mov_text 的字幕文本清成纯文本 */
static char *sub_clean(const char *s, int ass)
{
    if (s == NULL)
        return NULL;
    if (ass) {
        int commas = 0;
        /* FFmpeg 的 ASS rect 字段：ReadOrder,Layer,Style,Name,MarginL,R,
         * MarginV,Effect,Text —— 第 9 个字段才是文本（8 个逗号）。 */
        while (*s != '\0' && commas < 8) {
            if (*s == ',')
                commas++;
            s++;
        }
    }
    const size_t n = strlen(s);
    char *out = (char *)ql_mem_alloc(n + 1);
    if (out == NULL)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '{') {                     /* ASS 覆写块 */
            while (i < n && s[i] != '}')
                i++;
            continue;
        }
        if (s[i] == '<') {                     /* mov_text 内联标签 */
            while (i < n && s[i] != '>')
                i++;
            continue;
        }
        if (s[i] == '\\' && i + 1 < n && (s[i + 1] == 'N' || s[i + 1] == 'n')) {
            out[o++] = '\n';
            i++;
            continue;
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return out;
}

/*
 * 用**第二个 AVFormatContext** 把字幕轨完整解一遍。
 * 不能借用播放中的 demux：那会把 audio/video 的包全吃掉。
 */
static void ff_extract_subs(FFMedia *f, const char *src)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, src, NULL, NULL) < 0)
        return;
    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto done;

    int si = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            si = (int)i;
            break;
        }
    if (si < 0)
        goto done;

    AVStream *st = fmt->streams[si];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == NULL)
        goto done;
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (dec == NULL)
        goto done;
    if (avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
        avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        goto done;
    }

    AVPacket *pkt = av_packet_alloc();
    if (pkt != NULL) {
        while (f->sub_count < QL_CFG_MEDIA_CUES && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index != si) {
                av_packet_unref(pkt);
                continue;
            }
            AVSubtitle sub;
            memset(&sub, 0, sizeof(sub));
            int got = 0;
            if (avcodec_decode_subtitle2(dec, &sub, &got, pkt) >= 0 && got) {
                int64_t base_ms = 0;
                if (pkt->pts != AV_NOPTS_VALUE)
                    base_ms = av_rescale_q(pkt->pts, st->time_base,
                                           (AVRational){ 1, 1000 });
                for (unsigned k = 0; k < sub.num_rects; k++) {
                    const AVSubtitleRect *r = sub.rects[k];
                    const char *raw = (r->type == SUBTITLE_ASS && r->ass != NULL)
                                          ? r->ass
                                          : r->text;
                    char *clean = sub_clean(raw, r->type == SUBTITLE_ASS);
                    if (clean != NULL) {
                        int64_t a = base_ms + sub.start_display_time;
                        int64_t b = base_ms + sub.end_display_time;
                        if (b <= a)
                            b = a + 2000;
                        sub_add(f, clean, a, b);
                        ql_mem_free(clean);
                    }
                }
            }
            avsubtitle_free(&sub);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avcodec_free_context(&dec);

done:
    avformat_close_input(&fmt);
}

static int ff_subtitle_count(void *h)
{
    return ((FFMedia *)h)->sub_count;
}

static int ff_subtitle_get(void *h, int index, const char **text,
                           int64_t *start_ms, int64_t *end_ms)
{
    FFMedia *f = (FFMedia *)h;
    if (index < 0 || index >= f->sub_count)
        return -1;
    if (text != NULL) *text = f->subs[index].text;
    if (start_ms != NULL) *start_ms = f->subs[index].start_ms;
    if (end_ms != NULL) *end_ms = f->subs[index].end_ms;
    return 0;
}

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
    /* 坏包/跳转后的残帧：跳过这一包，别把整个播放打成 ERROR */
    if (r >= 0 || r == AVERROR(EAGAIN) || r == AVERROR_INVALIDDATA)
        return 1;
    return -1;
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

    ff_extract_subs(f, src);

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
    for (int i = 0; i < f->sub_count; i++)
        ql_mem_free(f->subs[i].text);
    ql_mem_free(f->subs);
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

/*
 * 有些录屏/WebM 全程只有开头一个关键帧。向后 seek 之后解码器一直
 * "Discarding interframe without a prior keyframe"，直到 EOF —— 表现就是
 * 跳转后黑屏/直接结束。兜底：回到 0 从头解码，丢掉目标之前的帧。
 */
static void ff_restart_from_zero(FFMedia *f)
{
    if (f->vstream < 0 || f->adec != NULL)
        return;                       /* A/V 文件不走这条路（避免音画不同步） */
    (void)av_seek_frame(f->fmt, f->vstream, 0, AVSEEK_FLAG_BACKWARD);
    if (f->vdec != NULL)
        avcodec_flush_buffers(f->vdec);
    if (f->vframe != NULL)
        av_frame_unref(f->vframe);
    q_clear(&f->vq);
    f->demux_eof = 0;
    f->vdrain_sent = f->vdrain_done = 0;
    f->vwait_key = 1;
    f->vdrop = 0;
    f->vskip_to_us = f->seek_target_us;
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
            int64_t ts = f->vframe->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = 0;
            const int64_t ts_us =
                av_rescale_q(ts, f->fmt->streams[f->vstream]->time_base,
                             (AVRational){ 1, 1000000 });

            /*
             * 跳转键帧：WebM/录屏的 GOP 可能很长，seek 落在 GOP 中间时
             * 解码器会一直 "Discarding interframe without a prior keyframe"。
             */
            if (f->vwait_key) {
                const int key = (f->vframe->flags & AV_FRAME_FLAG_KEY) != 0;
                if (!key) {
                    if (f->vdrop < 300) {
                        f->vdrop++;
                        av_frame_unref(f->vframe);
                        continue;
                    }
                    /* 找不到关键帧：回到 0 重解，跳过目标之前的帧 */
                    av_frame_unref(f->vframe);
                    ff_restart_from_zero(f);
                    continue;
                }
                f->vwait_key = 0;
                f->vdrop = 0;
            }
            if (f->vskip_to_us >= 0) {
                if (ts_us + 5000 < f->vskip_to_us) {
                    av_frame_unref(f->vframe);
                    continue;
                }
                f->vskip_to_us = -1;
            }
            uint8_t *planes[4] = { dst, NULL, NULL, NULL };
            int strides[4] = { f->vw * 4, 0, 0, 0 };
            sws_scale(f->vsws, (const uint8_t *const *)f->vframe->data,
                      f->vframe->linesize, 0, f->vh, planes, strides);
            *pts_us = ts_us;
            if (out_w != NULL) *out_w = f->vw;
            if (out_h != NULL) *out_h = f->vh;
            av_frame_unref(f->vframe);
            return 1;
        }
        if (ret == AVERROR_INVALIDDATA) {
            /* 解码器丢了一帧（常见于 seek 落在 GOP 中间）：继续往下读 */
            av_frame_unref(f->vframe);
            continue;
        }
        if (ret == AVERROR_EOF) {
            if (f->vwait_key || f->vskip_to_us >= 0) {
                /* 还没找到可解码的关键帧：回退到 0 从头解 */
                avcodec_flush_buffers(f->vdec);
                f->vdrain_sent = f->vdrain_done = 0;
                ff_restart_from_zero(f);
                if (f->vskip_to_us >= 0)
                    continue;
            }
            f->vdrain_done = 1;
            return 0;
        }
        if (ret != AVERROR(EAGAIN))
            return -1;

        const int sent = ff_send_next(f, &f->vq, f->vdec, &f->vdrain_sent);
        if (sent < 0)
            return -1;
        if (sent == 0 && f->vq.count == 0 && f->demux_eof && f->vdrain_sent) {
            if (f->vwait_key || f->vskip_to_us >= 0) {
                avcodec_flush_buffers(f->vdec);
                f->vdrain_sent = f->vdrain_done = 0;
                ff_restart_from_zero(f);
                if (f->vskip_to_us >= 0)
                    continue;
            }
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
    f->vwait_key = (f->vdec != NULL);
    f->vdrop = 0;
    f->seek_target_us = ms * 1000;
    /*
     * 不管 demux 落在关键帧 0 还是目标附近，目标之前的帧都不该出图：
     * 稀疏关键帧的录屏 seek 常常只能回到 0，靠这里"跳过"到目标。
     */
    f->vskip_to_us = f->seek_target_us;
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
    .subtitle_count = ff_subtitle_count,
    .subtitle_get = ff_subtitle_get,
};

const QLMediaBackend *ql_media_backend_ffmpeg(void)
{
    return &s_ffmpeg_backend;
}

#endif /* QL_FEAT_MEDIA_FFMPEG */
