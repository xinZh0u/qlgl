/**
 * @file    qlmedia.h
 * @brief   媒体播放（`<audio>` / 将来 `<video>`）：状态机 + 解码后端 + 音频输出。
 *
 * 分层（与 `QLPlatform` 同构）：
 *
 *     QLMedia（状态机 / 时钟 / 事件）
 *        ├─ QLMediaBackend（解码接缝：ffmpeg 默认）
 *        └─ QLAudioSink    （设备接缝：alsa 默认，NULL = 静音）
 *
 * 设计见 `doc/媒体播放设计.md`。M1 只做音频：
 *   - `ql_media_open()` 同步读容器头（不解码）；
 *   - `ql_media_play()` 起工作线程，解码 → 重采样 → 写音频设备；
 *   - 时钟 = 已写帧数 − 设备延迟，`ql_media_position_ms()` 随时可读；
 *   - 事件由 `ql_media_poll()` 在主循环里派发（不在工作线程回调）。
 *
 * 所有函数都可以在主线程调用；内部只投递命令，不阻塞等解码。
 * 没有媒体能力（`QL_FEAT_MEDIA=0`）时这些函数返回 `QL_ERR_UNSUPPORTED` / NULL。
 */
#ifndef QLGL_MEDIA_H
#define QLGL_MEDIA_H

#include "qlgl.h"

typedef struct QLMedia QLMedia;

typedef enum {
    QL_MEDIA_IDLE = 0,
    QL_MEDIA_OPENING,
    QL_MEDIA_READY,
    QL_MEDIA_PLAYING,
    QL_MEDIA_PAUSED,
    QL_MEDIA_ENDED,
    QL_MEDIA_ERROR,
} QLMediaState;

typedef enum {
    QL_MEDIA_EV_READY = 0,   /* 元数据就绪（时长/采样率可读） */
    QL_MEDIA_EV_STATE,       /* 状态变了（play/pause/end） */
    QL_MEDIA_EV_ENDED,       /* 播放到结尾（loop 时不会发） */
    QL_MEDIA_EV_ERROR,       /* 解码/设备错误，细节见 ql_media_error() */
} QLMediaEventType;

typedef void (*ql_media_event_cb)(QLMedia *m, QLMediaEventType ev, void *user);

/* ----- 解码后端 -----
 * `read_audio()` 输出 **S16 交错**、`rate()`/`channels()` 指定的格式；
 * 返回帧数，0 = EOF，<0 = 错误。M1 只要求音频流。
 */
typedef struct QLMediaBackend {
    const char *name;
    void   *(*open)(const char *src, char *err, size_t err_cap);
    void    (*close)(void *h);
    int64_t (*duration_ms)(void *h);
    int     (*has_audio)(void *h);
    int     (*has_video)(void *h);
    int     (*rate)(void *h);
    int     (*channels)(void *h);
    int     (*read_audio)(void *h, int16_t *buf, int max_frames);
    QLErr   (*seek)(void *h, int64_t ms);
} QLMediaBackend;

/* ----- 音频输出 -----
 * `write()` 阻塞到设备吃下这些帧（或返回 <0 = 失败）；
 * `delay_frames()` 返回设备里还没播完的帧数（做时钟用）。
 * `pause()` 可选：返回 0 表示暂停成功；<0 表示不支持，调用方会退化成
 * `flush()` + 重新起播。`flush()` 丢弃设备里未播内容（seek/暂停用）。
 */
typedef struct QLAudioSink {
    const char *name;
    int  (*open)(void **ctx, int rate, int channels);
    void (*close)(void *ctx);
    int  (*write)(void *ctx, const int16_t *pcm, int frames);
    int  (*delay_frames)(void *ctx);
    int  (*pause)(void *ctx, int paused);   /* 可选，可为 NULL */
    void (*flush)(void *ctx);
    uint32_t (*underruns)(void *ctx);       /* 可选 */
} QLAudioSink;

/* 后端/设备注册（传 NULL 恢复默认）；必须在 ql_media_open() 之前设置 */
void ql_media_set_backend(const QLMediaBackend *backend);
const QLMediaBackend *ql_media_backend(void);
void ql_audio_set_sink(const QLAudioSink *sink);
const QLAudioSink *ql_audio_sink(void);

/* 默认实现（弱符号：板级/测试可以覆盖） */
const QLMediaBackend *ql_media_backend_default(void);
const QLAudioSink    *ql_audio_sink_default(void);

/* ----- 生命周期与控制 ----- */
QLMedia *ql_media_open(const char *src);
void     ql_media_close(QLMedia *m);

QLErr ql_media_play(QLMedia *m);
QLErr ql_media_pause(QLMedia *m);
QLErr ql_media_seek(QLMedia *m, int64_t ms);
QLErr ql_media_set_volume(QLMedia *m, int percent);   /* 0..100 */
void  ql_media_set_muted(QLMedia *m, int muted);
void  ql_media_set_loop(QLMedia *m, int loop);

/* ----- 状态（常量时间可读） ----- */
int64_t       ql_media_duration_ms(const QLMedia *m);
int64_t       ql_media_position_ms(const QLMedia *m);
QLMediaState  ql_media_state(const QLMedia *m);
int           ql_media_has_audio(const QLMedia *m);
int           ql_media_has_video(const QLMedia *m);
uint32_t      ql_media_underruns(const QLMedia *m);
const char   *ql_media_error(const QLMedia *m);

/* 事件：`ql_media_poll()` 在主循环里排空队列并回调，返回 1 = 继续 */
void ql_media_set_event_cb(QLMedia *m, ql_media_event_cb cb, void *user);
int  ql_media_poll(QLMedia *m);

#endif /* QLGL_MEDIA_H */
