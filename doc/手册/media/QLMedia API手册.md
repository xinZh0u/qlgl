# QLMedia API 手册：媒体播放（M1：音频）

> 状态：**M1（音频）已实现（2026-11-15）**。视频 / 字幕见
> [媒体播放设计](../../媒体播放设计.md) 的 M2+。
> 最小例程：`example/44_audio_player`。

## 0. 十行版本

```c
qlgl_init();
QLMedia *m = ql_media_open("song.mp3");
ql_media_play(m);

while (ql_media_state(m) != QL_MEDIA_ENDED) {
    ql_media_poll(m);        /* 派发 READY/STATE/ENDED/ERROR 事件 */
    /* ... 每帧的其它逻辑 ... */
}
ql_media_close(m);
```

没有界面也能用；有界面时把 `ql_media_poll()` 放进主循环即可。

## 1. 状态机

```
IDLE ──open()──▶ READY ──play()──▶ PLAYING ──pause()──▶ PAUSED
                    ▲                 │                    │
                    │                 │ play()（恢复）      │
                    │                 ▼                    ▼
                    └──────────── ENDED ◀── 播到结尾 ──────┘
                     （loop=1 时回 0，不进入 ENDED）
```

- `ql_media_open()` **同步**读容器头（不解码），失败返回 `NULL`；
- `ENDED` 之后再 `play()` 会从 0 重新开始；
- `ERROR` 之后只能 `close()`；错误文本用 `ql_media_error(m)`。

## 2. API

```c
typedef struct QLMedia QLMedia;

QLMedia *ql_media_open(const char *src);      /* 本地文件；失败 NULL */
void     ql_media_close(QLMedia *m);

QLErr ql_media_play(QLMedia *m);              /* 开始 / 恢复 / ENDED 后重播 */
QLErr ql_media_pause(QLMedia *m);
QLErr ql_media_seek(QLMedia *m, int64_t ms);
QLErr ql_media_set_volume(QLMedia *m, int percent);   /* 0..100 */
void  ql_media_set_muted(QLMedia *m, int muted);
void  ql_media_set_loop(QLMedia *m, int loop);

int64_t      ql_media_duration_ms(const QLMedia *m);
int64_t      ql_media_position_ms(const QLMedia *m);
QLMediaState ql_media_state(const QLMedia *m);
int          ql_media_has_audio(const QLMedia *m);
int          ql_media_has_video(const QLMedia *m);   /* M1 恒 0 */
uint32_t     ql_media_underruns(const QLMedia *m);   /* 设备欠载次数 */
const char  *ql_media_error(const QLMedia *m);

void ql_media_set_event_cb(QLMedia *m, ql_media_event_cb cb, void *user);
int  ql_media_poll(QLMedia *m);               /* 主循环里调；返回 1 */
```

### 2.1 事件

| 事件 | 时机 |
|---|---|
| `QL_MEDIA_EV_READY` | `open()` 成功（时长/采样率可读） |
| `QL_MEDIA_EV_STATE` | 状态变化（PLAYING / PAUSED / ENDED…） |
| `QL_MEDIA_EV_ENDED` | 播放到结尾（`loop=1` 时不发） |
| `QL_MEDIA_EV_ERROR` | 解码/设备错误（细节在 `ql_media_error()`） |

回调**只在 `ql_media_poll()` 里被调用**（主线程），不会从工作线程冒出来。

### 2.2 位置与时钟

`position = 起点 + (已写帧数 − 设备延迟) / 采样率`：

- 播放中读的是"耳朵听到的位置"（用 ALSA 的 `snd_pcm_delay`）；
- 暂停时冻结在暂停瞬间；
- `seek()` 是异步的：位置立即跳到目标，实际音频在新位置之前可能还差几十毫秒。

### 2.3 视频（M2）

```c
/* 每来一帧 serial +1；没有视频返回 0 */
uint32_t ql_media_video_serial(const QLMedia *m);
/* 原始解码尺寸 */
int ql_media_video_size(const QLMedia *m, ql_coord_t *w, ql_coord_t *h);

/* 借用"最新一帧"；画完必须 release（否则解码端会停） */
const QLMediaVideoFrame *ql_media_acquire_frame(QLMedia *m);
void ql_media_frame_release(const QLMediaVideoFrame *frame);

/* 把媒体绑到 <video> 节点；每次 serial 变化后标脏该节点 */
QLErr ql_ui_bind_media(QLUIRoot *ui, QLUINodeRT *node, QLMedia *media);
void  ql_ui_invalidate(QLUIRoot *ui, QLUINodeRT *node);
```

主循环的写法：

```c
uint32_t serial = 0;
for (;;) {
    ql_media_poll(m);
    if (ql_media_video_serial(m) != serial) {
        serial = ql_media_video_serial(m);
        ql_ui_invalidate(ui, video_node);   /* 只重绘视频那一块 */
    }
    if (!ql_window_poll(win)) break;
}
```

帧是 **RGBA8888**、object-fit: contain（等比居中）。完整例子：
`example/45_video_player`。

### 2.4 字幕（M3）

```c
/* 外部 .vtt（<track src>）；path 是本地文件 */
QLErr ql_media_load_subtitle(QLMedia *m, const char *path);
/* 直接给 WebVTT 文本（构建期嵌入/网络下载都走这个） */
QLErr ql_media_set_subtitle_text(QLMedia *m, const char *vtt, size_t len);
void  ql_media_clear_subtitle(QLMedia *m);

/* 当前时间点的字幕；没有返回 NULL。指针归 QLMedia 所有 */
const char *ql_media_subtitle_text(QLMedia *m);
int         ql_media_subtitle_count(const QLMedia *m);
```

- 容器内嵌字幕（mp4 mov_text / mkv subrip / ASS）在 `ql_media_open()` 时自动解析；
- `<track>`：解析器会把 `src` 存进 `<track>` 节点的文本，应用自己读出来加载：
  ```c
  QLUINodeRT *tr = ql_ui_find_by_tag(ui, QL_UI_TAG_TRACK, 0);
  const char *src = ql_ui_node_text(tr);      /* "sub.vtt" */
  ql_media_load_subtitle(m, resolved_path);
  ```
  `example/45_video_player` 就是这么做的：`--vtt` > 视频同目录的 `track.vtt` >
  内置演示字幕，`--no-subtitle` 关闭；
- 只支持文本字幕：`<b>`/`<i>`/实体在解析时清成纯文本，不提供 `::cue` 样式。

## 3. 后端与设备（测试 / 板级）

M1 的两条接缝都可以替换，和 `QLPlatform` 同构：

```c
/* 解码：默认 ffmpeg；测试可用假后端 */
void ql_media_set_backend(const QLMediaBackend *b);

/* 出声：默认 alsa（dlopen libasound）；测试可用假 sink */
void ql_audio_set_sink(const QLAudioSink *s);
```

- `QLMediaBackend.read_audio()` 输出 **S16 交错**，采样率/声道由
  `rate()`/`channels()` 给出（FFmpeg 后端固定 48kHz 立体声）。
- `QLAudioSink.write()` 阻塞到设备吃下这些帧；`delay_frames()` 用于时钟；
  `pause()` 可选，不支持时核心会 `flush()` 后从指针位置重播。
- ALSA 打不开（没有声卡 / 没有 `libasound.so.2`）时自动退化成
  **静音 + 真实时钟**，视频（M2）仍然能按时间走。

弱符号覆盖：

```c
const QLMediaBackend *ql_media_backend_default(void);   /* 板级可定义强符号 */
const QLAudioSink    *ql_audio_sink_default(void);
```

## 4. 构建

| 开关 | 默认 | 作用 |
|---|---|---|
| `QLGL_BUILD_MEDIA` | ON | 构建 QLMedia；找不到 FFmpeg 就自动关闭 |
| `QL_FEAT_MEDIA` | 由 CMake 定义 | 引擎本体 |
| `QL_FEAT_MEDIA_FFMPEG` | 同 `QLGL_BUILD_MEDIA` | 默认解码后端 |
| `QL_FEAT_UI_MEDIA` | 同 `QL_FEAT_MEDIA` | `<audio>` 零盒子等标签语义 |

关掉后所有 API 返回 `QL_ERR_UNSUPPORTED` / `NULL`，`<audio>` 回到占位盒。

非 CMake 构建（Makefile/MCU 源码列表）要自己把 `src/media/qlmedia.c` 加进去：
它总是编译，媒体能力关闭时提供上述桩；`qlmedia_ffmpeg.c` / `qlaudio_alsa.c`
只在需要真实解码/出声时加入。

## 5. 限制（M1 + M2）

- 视频走 **CPU**：RGBA8888 + `sws_scale`，GPU 零拷贝/硬解在 M3；
- 只支持本地文件，网络流不做（`QL_FEAT_MEDIA_NET` 未实现）；
- 字幕只支持**文本**（WebVTT / 容器文本轨）；位图字幕（PGS/DVB）不支持；
- `open()` 是同步的：大文件的容器头解析会有几毫秒阻塞；
- 一个 `QLMedia` 一个工作线程；页面里十几个实例要自己控制数量；
- 视频尺寸在打开时固定，播放中换分辨率不支持（会在解码前按上限拒绝）；
- 只有开头一个关键帧的录屏（常见于 WebM）seek 会退化成"从头解码到目标"，
  跳转可能需要几百毫秒到几秒（画面会停在目标位置，属于预期）。

## 6. 与界面层的关系

M1 的 `QLMedia` 是**纯引擎**：它不知道 `<audio>` 节点在哪。
把媒体实例和界面节点绑定（`<audio src>` 自动打开、点击播放等）放在 M2，
和 `<video>` 一起做；现在用 C API 显式控制即可（见 `example/44_audio_player`）。
