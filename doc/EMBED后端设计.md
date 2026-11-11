# EMBED 后端设计（13 个标签怎么从占位变成真渲染）

> 状态：**设计；当前实现全部是占位（P7，2026-11-09）**。目标读者：要接
> ffmpeg / miniaudio / plutosvg / libcurl，或要决定某个平台该开哪些开关的人。
> 文档站上的覆盖度清单见 [HTML5 标签覆盖与裁剪](manual/ui/HTML5标签覆盖与裁剪.html)；
> 图像部分的做法直接参照 [图像管线设计](图像管线设计.html)，本文所有结构都与它同构。

这份文档定一件事：**`EMBED` 包这 13 个标签，哪些值得接外部库，哪些接了就是另一个产品。**

它同时纠一个说法。P7 落地时写的是"这 13 个永远只做占位"，那句话太重了 ——
真正的原则是：**能接外部 C 库就接，真的接不了才留占位；不同平台接不同的库；
PC / 移动默认开，MCU 默认关，但硬要在 MCU 上保留也允许。**
"占位"是**默认档**，不是**终点**。

---

## 0. 结论摘要

| 问题 | 决定 |
|---|---|
| 现在的状态 | **13 个全部是占位盒**（画标签名）。它们是"认识且不炸"，不是"已经做到头" |
| 谁来决定真渲染 | **编译期开关 + 平台后端**，结构和 `QLImageBackend` 完全同构：自有优先 → 后端 → `UNSUPPORTED` → 回落占位 |
| 需要外部库的 | 只有 **3 个**：`svg`（plutosvg）、`audio`（miniaudio + dr_libs）、`video`（pl_mpeg / ffmpeg） |
| 自己就能做的 | **4 个**，零外部依赖：`iframe`（构建期子文档）、`embed`/`object`（类型分派）、`canvas`（C API 渲染目标） |
| 库有但**不做**的 | `script`（引擎不是成本，DOM 是）、`slot`/`template`（没有消费者） |
| 没有可用 C 库的 | `math`（MathML 运行期布局器 = 比 TABLE 还大）；运行期不做，**构建期转 SVG/位图是可行旁路** |
| 平台差异 | **只是三套预设**，不是三套代码。任何组合都合法（和图像管线 §2 同一个哲学） |
| 线程 | **QLGL 不拥有线程。** audio/video 后端自带，库只暴露一个 `ql_ui_invalidate()` 跨线程重绘请求 |
| `noscript` | 现在的"隐藏"改成**配置项**（`QL_CFG_UI_NOSCRIPT_SHOW`），默认保持现状 |

**排序建议**（ROI 从高到低，见 §8）：`embed`/`object` 分派 + `canvas` C API → `svg` →
`iframe` → `audio` → `video` → `track`。`script`/`slot`/`template`/运行期 `math` **不做**。

---

## 1. 目标与非目标

**目标**

1. "这个标签我认识，但没接后端" 和 "这个标签我接不了" 是**两件不同的事**，必须在
   代码与文档上都分得开 —— 前者是开关，后者是红线。
2. 同一个标签在不同平台上可以是**不同的实现档次**（PC 全功能 / MCU 构建期预转 /
   完全没有），而**枚举值与 blob 不变**。
3. 加后端**不许改 HTML / CSS 面**。页面上写的还是 `<video src>`，变的只是接到哪个库。
4. 任何外部库都是**可选依赖**：`-DQL_EMBED_VIDEO_BACKEND=none` 之后不出现该库的符号，
   也不产生任何"半残"的代码路径。

**非目标（明确不做）**

1. **不做 DOM / JS。** 见 §7.1，这不是难度问题，是方向问题。
2. **不做插件 ABI。** `<object>` 不会变成"加载第三方 .so 画东西"。外来代码渲染
   天然无法有界，与本库红线直接冲突。
3. **不做网络栈。** `iframe` 的运行期 `src` 需要 fetch 时，平台后端自己给一个
   `ql_embed_fetch` 回调（PC 上可以是 libcurl，MCU 上就是 `NULL`）。库本身不带 HTTP。
4. **不为了某个标签引入一个线程框架。** 见 §5.1。

---

## 2. 四档分类

判据只有一个：**"真渲染"能不能在不动架构前提的情况下完成。**

| 档 | 含义 | 标签 |
|---|---|---|
| **A** | 自己能做，不需要外部库，也不动架构 | `iframe` `embed` `object` `canvas` `track` |
| **B** | 需要外部库，但接进来是加法 | `svg` `audio` `video` |
| **C** | 库存在，但接了要推翻"构建期定型 / 无运行期重排 / 脏区驱动" | `script` `slot` `template` |
| **D** | 没有可用的 C 库，自己写等于重做一个布局引擎 | `math` |

`track` 归 A 但**依附** B 的 media（独立存在无意义）。

---

## 3. 能力开关（不分平台）

### 3.1 为什么必须这么切

和图像管线 §2 同一条理由：**"PC 配置"与"MCU 配置"是两套预设，不是两套代码。**
一个 MCU 项目如果 flash 够、就是想放 MP3，`QL_EMBED_AUDIO_BACKEND=miniaudio` 也必须能编过。

另外**硬约束沿用**：关开关**不许改 `QLUITag` 枚举**（枚举值写进 blob）。裁的是
代码路径与"这个标签有没有后端"，不是标签编号。

### 3.2 开关结构

```c
/* 总开关：0 = 13 个全部走占位盒，行为与 P7 完全一致 */
#ifndef QL_FEAT_UI_EMBED
#define QL_FEAT_UI_EMBED            1
#endif

/* ---- A 档：不需要外部库 ---- */
#ifndef QL_FEAT_EMBED_IFRAME
#define QL_FEAT_EMBED_IFRAME        1   /* 构建期子文档 + 子视口 + 事件坐标路由 */
#endif
#ifndef QL_FEAT_EMBED_IFRAME_NET
#define QL_FEAT_EMBED_IFRAME_NET    0   /* 运行期 src：需要平台提供 fetch 回调 */
#endif
#ifndef QL_FEAT_EMBED_CANVAS
#define QL_FEAT_EMBED_CANVAS        1   /* 渲染回调挂载点（C API 形态） */
#endif
#ifndef QL_FEAT_EMBED_TRACK
#define QL_FEAT_EMBED_TRACK         0   /* WebVTT 字幕：依赖任一 media 后端 */
#endif

/* ---- B 档：后端三选一（choice 语义，宏名互斥由构建系统保证） ---- */
#define QL_EMBED_SVG_NONE           0
#define QL_EMBED_SVG_BUILDTIME      1
#define QL_EMBED_SVG_PLUTOSVG       2
#ifndef QL_EMBED_SVG_BACKEND
#define QL_EMBED_SVG_BACKEND        QL_EMBED_SVG_NONE
#endif

#define QL_EMBED_AUDIO_NONE         0
#define QL_EMBED_AUDIO_WAV_I2S      1
#define QL_EMBED_AUDIO_MINIAUDIO    2
#ifndef QL_EMBED_AUDIO_BACKEND
#define QL_EMBED_AUDIO_BACKEND      QL_EMBED_AUDIO_NONE
#endif

#define QL_EMBED_VIDEO_NONE         0
#define QL_EMBED_VIDEO_PLM          1
#define QL_EMBED_VIDEO_FFMPEG       2
#ifndef QL_EMBED_VIDEO_BACKEND
#define QL_EMBED_VIDEO_BACKEND      QL_EMBED_VIDEO_NONE
#endif

/* ---- 跨线程重绘请求（audio/video 后端需要，见 §5.1） ---- */
#ifndef QL_FEAT_UI_INVALIDATE_ANY_THREAD
#define QL_FEAT_UI_INVALIDATE_ANY_THREAD 0
#endif

/* ---- 与规范相反的那条，改成可选（见 §6.8） ---- */
#ifndef QL_CFG_UI_NOSCRIPT_SHOW
#define QL_CFG_UI_NOSCRIPT_SHOW     0
#endif
```

> **诚实标注**：以上除 `QL_CFG_UI_NOSCRIPT_SHOW` 外**代码里都还不存在**。
> 本文是设计，不是现状。现状只有 P7 的占位实现。

### 3.3 三套预设

| 开关 | **PC** | **移动** | **MCU** |
|---|---|---|---|
| `QL_FEAT_UI_EMBED` | 1 | 1 | 1 |
| `EMBED_IFRAME` | 1 | 1 | 1（仅构建期） |
| `EMBED_IFRAME_NET` | 1 | 1 | **0** |
| `EMBED_CANVAS` | 1 | 1 | 1 |
| `EMBED_TRACK` | 1 | 1 | 0 |
| `EMBED_SVG_BACKEND` | `PLUTOSVG` | `PLUTOSVG` | **`BUILDTIME`** |
| `EMBED_AUDIO_BACKEND` | `MINIAUDIO` | `MINIAUDIO`（AAudio/OpenSL） | `WAV_I2S` |
| `EMBED_VIDEO_BACKEND` | `FFMPEG` | `FFMPEG`（硬解） | **`NONE`** |
| `INVALIDATE_ANY_THREAD` | 1 | 1 | 1（中断里置位） |
| `NOSCRIPT_SHOW` | 0 | 0 | 0 |

**MCU 上 `svg` 不是"关掉"，是"构建期转 RAW"** —— 能力还在，代价搬到了开发机。
这正是图像管线 `raw-only` 预设的同一条思路，也是"硬要保留也可以"的落点。

---

## 4. 后端接口

沿用 `QLImageBackend` 的形状（见图像管线 §5.2），**成功/不支持/失败三态**：

```c
typedef enum {
    QL_EMBED_OK          =  0,
    QL_EMBED_UNSUPPORTED =  1,   /* 本后端不认这个输入 → 回落占位盒，不算错误 */
    QL_EMBED_ERROR       = -1,
} QLEmbedStatus;

typedef struct QLEmbedBackend {
    /* 后端名，只用于错误文案：'ffmpeg' / 'miniaudio' / 'plutosvg' */
    const char *name;

    /* 能否处理这个节点（按 src 的 MIME/魔数/扩展名判断）。不写文件、不解码。 */
    int  (*probe)(const QLUINode *node, const void *bytes, size_t n);

    /* 创建运行期实例（分配缓冲、起线程、开设备）。失败必须回滚干净。 */
    void *(*open)(QLUIRoot *ui, const QLUINode *node, const void *bytes, size_t n);

    /* 每帧：把当前画面画到 dst（video/canvas），或什么都不做（audio）。 */
    void  (*paint)(void *inst, QLDraw2D *d2d, QLRect dst);

    /* 时间推进（主循环调用，dt 毫秒）。audio 用它调进度、video 用它做音画同步。 */
    void  (*tick)(void *inst, uint32_t dt_ms);

    /* 事件（点击播放键 / seek）。返回 1 表示已消费。 */
    int   (*handle)(void *inst, const QLInputEvent *ev);

    void  (*close)(void *inst);
} QLEmbedBackend;

void ql_embed_set_backend(QLEmbedStatus (*fn)(QLEmbedBackend *out));
```

调度顺序与图像完全一致：

```
节点是 EMBED 且开了对应开关
      └─▶ 后端 probe(src)
              ├─ OK / UNSUPPORTED ─▶ 画占位盒（**不是错误**，P7 行为）
              └─ 后端 bind ─▶ 真渲染
```

**为什么 `UNSUPPORTED` 不是错误**：PC 上放了个 MCU 编不进来的容器格式，页面上
应该看到占位盒而不是整页失败。这是"明确失败优于半个正确"的例外，理由和
图像后端一样：**占位盒本身就是一次明确的、已经声明过的降级。**

---

## 5. 三个硬前提

### 5.1 线程：QLGL 不拥有线程

现状：**全库零线程**（`pthread` / `thrd_create` 一处都没有）。这是有意的：
MCU 上没有 pthread，引入线程抽象等于让所有平台背一份用不上的 port 层。

所以 audio/video 的做法是：**后端自带线程，库只提供唯一一个同步点。**

```c
/* 任何线程都可以调。内部只置一个原子 dirty 标志 + 记下矩形，
   主循环下一次醒来时看到就重绘。不阻塞、不分配、不加锁（relaxed 原子即可）。 */
void ql_ui_invalidate(QLUIRoot *ui, QLRect rect);
```

这样一个 API 换来三件事：

| 平台 | 谁来调 `ql_ui_invalidate` |
|---|---|
| PC | miniaudio 音频线程（播完、进度变化）/ ffmpeg 解码线程（新帧就绪） |
| 移动 | 同上（AAudio / OpenSL 回调） |
| MCU | I2S DMA 完成中断（置位） |

**这条 API 是 audio/video 的入场券**：没有它，后端线程解出新帧只能等主循环下次
因为别的原因醒来，视频会一顿一顿。它本身与 EMBED 无关，应该单独先落（很小的改动）。

### 5.2 路径填充：`<svg>` 的地基

现状 draw2d 只有：矩形、圆角矩形、直线、文字、blit。**没有任意多边形填充，
没有通用 transform 栈，渐变只有圆角矩形两色线性。**

这决定了 `svg` 的两个岔路：

| 路线 | 需要什么 | 结论 |
|---|---|---|
| `nanosvg` | 只解析，吐路径命令 → **要自己写扫描线光栅化 + 抗锯齿** | 不划算 |
| `plutosvg` + `plutovg` | 自带光栅器，输出一块 surface | **选这个**：把 surface 当 `QLImage` blit |

### 5.3 无 JS 连带的三条

`<canvas>` / `<slot>` / `<template>` / `<script>` 的"真渲染"全都以 JS 为前提。
§7 逐条说明为什么它们的替代方案（C API / 构建期展开）才是自洽的形态。

---

## 6. 逐个标签

### 6.1 `iframe` —— 构建期子文档（A 档）

**不需要网络、不需要浏览器、不需要库。** 做法：

1. `iframe src="a.html"` 在**构建期**解析成"子文档名"，进 doc 的资源表
   （和 `<img src>` 完全同一条路，同一个 `qltext cc --root` 递归）；
2. 运行期该节点持有一个 `<子 QLUIRoot, 滚动偏移>`，用现成的 `ql_draw2d_set_clip` 画；
3. **事件按坐标路由**：点 iframe 里的 `<input>`，焦点要进**子文档的输入池**。

第 3 条是唯一需要新机制的地方，成本全在这。其余部分（裁剪、滚动、渲染）都是现成的。

| 事项 | 决定 |
|---|---|
| 子文档深度 | 上限 `QL_CFG_UI_IFRAME_MAX_DEPTH`（默认 2）；超过报错，不递归爆栈 |
| 递归引用 | 构建期检测环（A 引用 B、B 引用 A），**明确报错** |
| 跨文档 CSS | **不穿透**。子文档的样式只受它自己的 `<style>` 与内联影响，父文档选择器够不到里面 |
| `sandbox` / `allow` | 不解析（没有执行能力，没有需要关掉的东西）。遇到不报错，忽略 |
| 运行期 `src` | 单列 `QL_FEAT_EMBED_IFRAME_NET`：要求平台给 `ql_embed_fetch(url, &bytes, &n)` 回调；**库自己不带 HTTP 栈** |
| MCU | 只有构建期路径；`src` 不是静态子文档就直接占位 |

**为什么不学浏览器搞"独立浏览上下文"**：那是给 JS 做同源隔离的。没有 JS，就没有
同源问题，也就不需要一个比"子视口"更重的东西。

### 6.2 `embed` / `object` —— 类型分派（A 档）

纯分派，几十行，白捡：

```
src/data 的魔数是图片 ──▶ 走 <img> 那条完整路径（构建期转码 / 运行期解码 / object-fit / 热区）
src 是某个 HTML 子文档 ─▶ 走 §6.1
其他（PDF / Flash / 未知）─▶ 占位盒
```

`<param>` 子元素按元数据跳过（和 `META` 包同样处理），不产生盒子。

**明确不做**：插件 ABI。`<object type="application/x-my-renderer">` 不会去
`dlopen` 任何东西 —— 外来代码渲染无法有界。

### 6.3 `canvas` —— C API 渲染目标（A 档）

HTML 里 canvas 的内容是脚本画的。**没有 JS，`<canvas>` 就永远是空白** ——
想按 HTML 语义"真实现"必须先有 script，绕回 §7.1 的死路。

但换一个入口就完全成立：**`<canvas>` 是给应用代码的渲染回调挂载点。**

```c
typedef void (*ql_canvas_paint_fn)(QLCanvas *cv, QLRect box, void *user);

/* 绑定一次，之后每次该节点进入重绘区都会被调 */
int ql_ui_canvas_bind(QLUIRoot *ui, QLUINodeIdx node,
                      ql_canvas_paint_fn fn, void *user);
```

现有的 `ql_canvas_*` 已经够画图表/仪表/波形（矩形、圆角、渐变、阴影、文字、blit）。
数据变了就 `ql_ui_invalidate(ui, box)` —— **天然遵守脏区红线**，因为它就是在
`ql_ui_render` 内部被调的，画在既定裁剪区内。

| 事项 | 决定 |
|---|---|
| 坐标系 | 传给回调的 `QLCanvas` 已经裁剪到盒子内，原点 0,0 = 盒子左上角 |
| 回调里能不能改布局 | **不能**，只有画布。要改布局请改状态再整体 invalidate |
| `width`/`height` 属性 | 只当作 UA 默认尺寸提示；实际盒子由 CSS 决定（与浏览器不同，**写在这里**） |
| 没有绑定时 | 占位盒（P7 行为） |

### 6.4 `svg` —— B 档，ROI 最高

| 路线 | 库 | 许可 | 说明 |
|---|---|---|---|
| **构建期光栅化** | plutosvg | MIT | `qltext` 里转成 **RAW 资源**（目标像素格式），运行期零解码器、零库。**MCU 唯一可行档** |
| 运行期光栅化 | plutosvg + plutovg | MIT | 挂一个"SVG 后端"，和 JPEG 后端同一条路；PC/移动默认 |
| 自己写 | nanosvg | zlib | 只解析，还要自己写扫描线光栅化 → **不选** |
| librsvg | librsvg | LGPL | 拖 glib + cairo + pango，与 MCU 方向冲突，**排除** |

**取舍要写清楚**：构建期光栅化意味着**位图尺寸固定**。元素被 CSS 拉到别的尺寸时
就是位图缩放（会糊）。所以：

- 构建期按 `svg` 元素的**最终 CSS 尺寸**转；
- 尺寸是响应式的就用 `--scale 2` 出一份 2x；
- 真需要任意缩放就开运行期后端。

**`svg` 内部标签（`path` `circle` `g` …）永远不进 `QLUITag`。** SVG 子树是
非 HTML 内容，解析期整段跳过（P7 已实现），交给后端按 SVG 自己的规范解析 ——
两套标签体系不混。

### 6.5 `audio` —— B 档，中等成本

| 环节 | 方案 | 成本 |
|---|---|---|
| 容器 + 解码 | `dr_wav` / `dr_mp3` / `dr_flac`（全单文件、公有领域）、`stb_vorbis`（OGG） | 低 |
| 输出设备 | **miniaudio**（MIT-0）：ALSA / PulseAudio / JACK / CoreAudio / WASAPI / AAudio / OpenSL / ESP32 一套通吃 | 低 |
| 线程 | miniaudio 自带音频线程 → 只需 `ql_ui_invalidate` | **见 §5.1** |
| 播放控件 | `<audio controls>` = 播放/暂停/进度/音量，本质是一个**新的 CONTROL 控件**（和 `<input>` 同级，要池） | 中 |
| MCU 档 | `QL_EMBED_AUDIO_WAV_I2S`：只有 `dr_wav` + I2S DMA 环形缓冲，中断里喂数据，零线程 | 中 |

**`<audio controls>` 是这一档真正的工作量**，不是解码。它要进 `CONTROL` 包，
要池、要事件、要进度条跟播放位置同步（走脏区，不许全帧重绘）。

不做 `<audio>` 的元素级 `play()`/`pause()` JS 接口 —— 没有 JS。播放控制在 C 侧：

```c
int ql_ui_media_play(QLUIRoot *ui, QLUINodeIdx node);
int ql_ui_media_pause(QLUIRoot *ui, QLUINodeIdx node);
int ql_ui_media_seek(QLUIRoot *ui, QLUINodeIdx node, uint32_t ms);
```

### 6.6 `video` —— B 档，成本最大

| 方案 | 能力 | 体积 | MCU | 许可 |
|---|---|---|---|---|
| **pl_mpeg**（单文件 MIT） | MPEG-1 视频 + Layer II 音频 | ~100 KB | ✅ 中端 ARM 能跑 | MIT |
| minimp4 + openh264 | H.264 / MP4 | ~1.5 MB | ❌ | BSD |
| **ffmpeg** | 一切 | 5–15 MB + 一堆依赖 | ❌ | **LGPL**；静态链接闭源产品有合规风险 |

**建议从 `pl_mpeg` 起步**：无依赖、无线程、输出 RGB24 直接塞 `QLImage`。
ffmpeg 作为 PC 上的"全格式后端"后加，但要接受体积与 LGPL 的链接要求。

视频还有**内核级新增**：逐帧 `blit_image` 需要**双缓冲**（GPU 上每帧一次纹理上传，
640×480 RGB565 = 600 KB/帧）。不做双缓冲会撕裂。这块要单独设计，且必须复用
现有的 `mode` 分派与图片批处理（见图像管线 §6，图片单独成批）。

音画同步：**以音频为时钟**。没有音频轨时退回单调时间。`tick(dt_ms)` 里做，不另起定时器。

### 6.7 `track` —— 依附于 media（A 档）

单独存在无意义（P7 已经 `display:none`）。media 落地后：

1. 解析 WebVTT（约 200 行：`WEBVTT` 头、`00:00:01.000 --> 00:00:03.000` 时间轴、
   cue 文本；**不解析 `<c>` / `<v>` 内联标记**，只当纯文本）；
2. 按当前播放位置挑 cue，叠画在 media 盒子底部；
3. 上限 `QL_CFG_UI_TRACK_MAX_CUES`（默认 256），超了明确报错。

`kind="chapters"` / `metadata` 忽略（不可见）。

### 6.8 `noscript` —— 需要改现状

P7 的实现是 `display:none`，理由是"网页里的 noscript 多是'请开启 JavaScript'噪音"。

按"能实现就实现"的原则，**逻辑上应该反过来**：本库永久无脚本 ⇒ 脚本"总是不可用"
⇒ 按规范 `noscript` 的内容**就是该显示的正文**。

改成配置项，默认保持现状：

```c
/* 0 = 隐藏（P7 现状，实用主义）；1 = 按规范显示（无脚本时显示内容） */
#ifndef QL_CFG_UI_NOSCRIPT_SHOW
#define QL_CFG_UI_NOSCRIPT_SHOW     0
#endif
```

---

## 7. 明确不做的，以及为什么

### 7.1 `script` —— 引擎不是成本，DOM 才是

引擎选择其实很宽：QuickJS（MIT，~210 KB）、JerryScript（Apache-2.0，MCU 级 ~100 KB）、
Duktape（MIT）。**任何一个都能塞进来。** 真正拦路的是四条，每条都直撞现有红线：

| JS 要做的事 | 撞的红线 |
|---|---|
| `el.style.color = 'red'` | **运行期样式重算** —— 现在样式是构建期算好的常量表 |
| `el.appendChild()` | **运行期创建节点** —— 现在节点表在 blob 里只读 |
| `requestAnimationFrame(loop)` | **持续全帧重绘** —— 现在是脏区驱动 |
| DOM 绑定本身（`querySelector` / 事件冒泡到 JS / `innerHTML`） | 比引擎大一个数量级 |

也就是说：**它不是"难"，是方向相反。**

硬要做，唯一自洽的形态是"脚本只能读写一份外挂状态 + 调 `ql_ui_invalidate`"，
那不是 DOM，是一个受限绑定层 —— 那已经和"支持 HTML 里的 `<script>`"是两件事了。
要那个能力，正确做法是**在 C 侧暴露 API**（§6.3 的 canvas 绑定就是这个思路）。

### 7.2 `slot` / `template` —— 没有消费者

两者都靠 shadow DOM + custom elements + JS 才能生效。没有 JS：

- `<template>` 的内容**没有任何东西会去克隆它**；
- `<slot>` **没有任何 shadow tree** 可以插。

**唯一的例外值得记一笔**：可以把它们做成**构建期**语法 —— `<template>` + `<slot>`
当成"宏和占位符"，构建期展开成普通节点。这不需要 JS、不需要运行期机制，
而且能解决"重复组件"这个真问题。但那是 QLGL 自己加的语言特性，**不是 HTML 语义**，
要做应该单独立项，不要混在 `EMBED` 包的分期里。

### 7.3 `math` —— 没有可用的 C 库，但有便宜旁路

MathML 需要：分数线的垂直对齐、根号的可伸缩符号（stretchy）、上下标、大运算符、
随内容长高的括号……这是一整套**数学排版算法**，规模 ≥ `TABLE` 包（10 个标签、5–8 天），
而 C 世界没有可用实现（KaTeX / MathJax 全是 JS）。

**旁路**：`<math>` 的内容在**构建期**用 TeX/KaTeX 渲染成 **SVG 或位图**，
然后走 §6.4 / 图像管线。运行期零成本、零库。

**前提是 SVG 管线先通** —— 这是"先做 svg"的第二个理由（第一个是图标到处都是）。

---

## 8. 分期与工作量

| 期 | 内容 | 依赖 | 估时 |
|---|---|---|---|
| **P7a** | `ql_ui_invalidate`（跨线程重绘请求）+ `QL_CFG_UI_NOSCRIPT_SHOW` | 无 | 0.5 天 |
| **P7b** | `embed`/`object` 类型分派（图片 → `<img>` 路径） | 无 | 0.5 天 |
| **P7c** | `canvas` 渲染回调（`ql_ui_canvas_bind`） | P7a | 1–2 天 |
| **P7d** | `svg` **构建期**光栅化（`qltext` + plutosvg → RAW） | 图像管线 | 3–5 天 |
| **P7e** | `svg` 运行期后端（挂 `QLEmbedBackend`） | P7d | 2–3 天 |
| **P7f** | `iframe` 构建期子文档 + 子视口 + 事件坐标路由 | 无 | 5–8 天 |
| **P7g** | `iframe` 运行期 `src`（平台 fetch 回调） | P7f | 1–2 天 |
| **P7h** | `audio` 解码（dr_*）+ `<audio controls>` 控件 | P7a | 5–8 天 |
| **P7i** | `audio` miniaudio 输出后端 | P7h | 2–3 天 |
| **P7j** | `video` pl_mpeg + 双缓冲 + 音画同步 | P7a, P7i | 8–12 天 |
| **P7k** | `video` ffmpeg 全格式后端 | P7j | 4–6 天 |
| **P7l** | `track` WebVTT | P7i 或 P7j | 2–3 天 |

合计约 **34–53 人日**。**必须做的只有 P7a–P7c（2–4 天）**，其余按产品需要挑。

**建议顺序**：P7a → P7b → P7c → P7d → P7f → P7e → P7h/i → P7j → P7l → P7g/k。

---

## 9. 风险与已知限制

| 风险 | 说明 | 应对 |
|---|---|---|
| **LGPL 合规** | ffmpeg 静态链接进闭源产品有法律风险 | 文档写死；默认后端是 pl_mpeg（MIT） |
| **体积** | ffmpeg 5–15 MB，miniaudio ~90 KB，plutosvg+plutovg ~150 KB | 每个后端一个独立开关；`none` 档零符号 |
| **MCU 上的 svg 尺寸固定** | 构建期光栅化 = 位图缩放会糊 | 支持 `--scale 2/3`；尺寸响应式就上运行期后端 |
| **iframe 事件路由的正确性** | 子文档的输入池、焦点、点击热区坐标都要跟着偏移 | 单独一套 `test_embed_iframe`（键盘/鼠标/焦点三组） |
| **video 撕裂** | 单缓冲逐帧 blit 在 GPU 上会看到半帧 | 双缓冲 + 只在 `tick` 后提交 |
| **audio 设备独占** | ALSA / AAudio 被别的进程占用时打不开 | `open()` 失败 → 明确错误 + 占位盒，不静默 |
| **`<canvas>` 回调里改布局** | 会导致渲染中途 doc 变化 | 回调只给 `QLCanvas*`，拿不到 `QLUIRoot*`（**类型上就禁止**） |
| **P7f 之前 `iframe` 只有占位** | 现状 | 文档写清楚，不假装 |

---

## 10. 与现有文档的关系

| 文档 | 关系 |
|---|---|
| [HTML5 标签覆盖与裁剪](manual/ui/HTML5标签覆盖与裁剪.html) | 权威标签表；`EMBED` 那 13 行现在全是 ✅（占位），接上后端后才升级实现档次 |
| [图像管线设计](图像管线设计.html) | **同构蓝本**：能力开关、平台后端、构建期转码三件事本文全部照抄，只是换了标签 |
| [QLUI API 手册](<manual/ui/QLUI API手册.html>) | 占位标签的 API 面（`ql_ui_invalidate` / `ql_ui_canvas_bind` / `ql_ui_media_*` 落地后补进去） |
| [QLUI 阅读指南](manual/ui/QLUI阅读指南.html) | §5.4.7 的 UA 默认表；接后端后占位盒仅在 `UNSUPPORTED` 时出现 |
| [开发规划](开发规划.html) | 分期表 P7a–P7l |
