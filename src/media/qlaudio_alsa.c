/**
 * @file    qlaudio_alsa.c
 * @brief   QLMedia 的 ALSA 音频输出（**dlopen**，不依赖 libasound 开发头）。
 *
 * 为什么用 dlopen：发行版里 `libasound.so.2` 几乎总是存在，但 -dev 头不一定装。
 * 这里只用到 8 个稳定了二十年的符号，自己声明原型/枚举比引一个构建依赖划算：
 * 没有 ALSA 的机器上 `open()` 直接失败，`QLMedia` 自动退化成静音 + 系统时钟。
 */
#include <dlfcn.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "qlmedia_internal.h"

#if QL_FEAT_MEDIA

/* ALSA 的公开常量（stable ABI） */
enum {
    ALSA_STREAM_PLAYBACK = 0,
    ALSA_FORMAT_S16_LE = 2,
    ALSA_ACCESS_RW_INTERLEAVED = 3,
};

typedef struct snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

typedef struct {
    snd_pcm_t *pcm;
    int        channels;
    uint32_t   underruns;
} QLAlsa;

typedef struct {
    int loaded;
    int (*pcm_open)(snd_pcm_t **pcm, const char *name, int stream, int mode);
    int (*set_params)(snd_pcm_t *pcm, int format, int access, unsigned channels,
                      unsigned rate, int soft_resample, unsigned latency_us);
    snd_pcm_sframes_t (*writei)(snd_pcm_t *pcm, const void *buf,
                                snd_pcm_uframes_t frames);
    int (*delay)(snd_pcm_t *pcm, snd_pcm_sframes_t *frames);
    int (*prepare)(snd_pcm_t *pcm);
    int (*drop)(snd_pcm_t *pcm);
    int (*pause)(snd_pcm_t *pcm, int enable);
    int (*recover)(snd_pcm_t *pcm, int err, int silent);
    int (*pcm_close)(snd_pcm_t *pcm);
} QLAlsaApi;

static QLAlsaApi s_api;

static void *alsa_sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (p == NULL)
        QL_LOGW("alsa: missing symbol %s（音频输出关闭）", name);
    return p;
}

static int alsa_load(void)
{
    static int tried;          /* 0 = 未尝试，1 = 成功，-1 = 失败 */
    if (tried != 0)
        return tried > 0;

    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL)
        lib = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);

    if (lib != NULL) {
        *(void **)&s_api.pcm_open = alsa_sym(lib, "snd_pcm_open");
        *(void **)&s_api.set_params = alsa_sym(lib, "snd_pcm_set_params");
        *(void **)&s_api.writei = alsa_sym(lib, "snd_pcm_writei");
        *(void **)&s_api.delay = alsa_sym(lib, "snd_pcm_delay");
        *(void **)&s_api.prepare = alsa_sym(lib, "snd_pcm_prepare");
        *(void **)&s_api.drop = alsa_sym(lib, "snd_pcm_drop");
        *(void **)&s_api.pause = alsa_sym(lib, "snd_pcm_pause");
        *(void **)&s_api.recover = alsa_sym(lib, "snd_pcm_recover");
        *(void **)&s_api.pcm_close = alsa_sym(lib, "snd_pcm_close");
    }

    const int ok = (s_api.pcm_open != NULL && s_api.set_params != NULL &&
                    s_api.writei != NULL && s_api.delay != NULL &&
                    s_api.prepare != NULL && s_api.drop != NULL &&
                    s_api.pcm_close != NULL);
    s_api.loaded = ok;
    tried = ok ? 1 : -1;
    return ok;
}

static int alsa_open(void **ctx, int rate, int channels)
{
    if (ctx == NULL || !alsa_load())
        return -1;
    if (rate <= 0)
        rate = 48000;
    if (channels <= 0)
        channels = 2;

    QLAlsa *a = (QLAlsa *)ql_mem_calloc(1, sizeof(*a));
    if (a == NULL)
        return -1;

    if (s_api.pcm_open(&a->pcm, "default", ALSA_STREAM_PLAYBACK, 0) < 0) {
        ql_mem_free(a);
        return -1;
    }
    /* 50ms 延迟：小到暂停/seek 反应快，大到不容易欠载 */
    if (s_api.set_params(a->pcm, ALSA_FORMAT_S16_LE, ALSA_ACCESS_RW_INTERLEAVED,
                         (unsigned)channels, (unsigned)rate, 1, 50000) < 0) {
        s_api.pcm_close(a->pcm);
        ql_mem_free(a);
        return -1;
    }
    a->channels = channels;
    *ctx = a;
    return 0;
}

static void alsa_close_sink(void *ctx)
{
    QLAlsa *a = (QLAlsa *)ctx;
    if (a == NULL)
        return;
    if (a->pcm != NULL)
        s_api.pcm_close(a->pcm);
    ql_mem_free(a);
}

static int alsa_write(void *ctx, const int16_t *pcm, int frames)
{
    QLAlsa *a = (QLAlsa *)ctx;
    if (a == NULL || pcm == NULL || frames <= 0)
        return -1;

    int done = 0;
    while (done < frames) {
        const snd_pcm_sframes_t n =
            s_api.writei(a->pcm, pcm + (size_t)done * (size_t)a->channels,
                         (snd_pcm_uframes_t)(frames - done));
        if (n < 0) {
            if (s_api.recover != NULL &&
                s_api.recover(a->pcm, (int)n, 1) >= 0) {
                if ((int)n == -32 /* -EPIPE */)
                    a->underruns++;
                continue;
            }
            a->underruns++;
            return -1;
        }
        if (n == 0)
            break;
        done += (int)n;
    }
    return done;
}

static int alsa_delay(void *ctx)
{
    QLAlsa *a = (QLAlsa *)ctx;
    if (a == NULL || s_api.delay == NULL)
        return 0;
    snd_pcm_sframes_t d = 0;
    if (s_api.delay(a->pcm, &d) < 0)
        return 0;
    if (d < 0)
        d = 0;
    if (d > 0x7fffffff)
        d = 0x7fffffff;
    return (int)d;
}

static int alsa_pause(void *ctx, int paused)
{
    QLAlsa *a = (QLAlsa *)ctx;
    if (a == NULL || s_api.pause == NULL)
        return -1;
    return (s_api.pause(a->pcm, paused ? 1 : 0) >= 0) ? 0 : -1;
}

static void alsa_flush(void *ctx)
{
    QLAlsa *a = (QLAlsa *)ctx;
    if (a == NULL)
        return;
    (void)s_api.drop(a->pcm);
    (void)s_api.prepare(a->pcm);
}

static uint32_t alsa_underruns(void *ctx)
{
    QLAlsa *a = (QLAlsa *)ctx;
    return (a != NULL) ? a->underruns : 0;
}

static const QLAudioSink s_alsa = {
    .name = "alsa",
    .open = alsa_open,
    .close = alsa_close_sink,
    .write = alsa_write,
    .delay_frames = alsa_delay,
    .pause = alsa_pause,
    .flush = alsa_flush,
    .underruns = alsa_underruns,
};

const QLAudioSink *ql_audio_sink_alsa(void)
{
    return &s_alsa;
}

#else

const QLAudioSink *ql_audio_sink_alsa(void)
{
    return NULL;
}

#endif /* QL_FEAT_MEDIA */
