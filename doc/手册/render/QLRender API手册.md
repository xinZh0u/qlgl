# QLRender / GPU 后端 API 手册（v0.1）

配套：[QLRender 阅读指南](./QLRender阅读指南.md) · 定位：[Web 兼容设计](../../Web兼容设计.md) §7

---

## 1. 绘制接口 `QLDraw2D`

头文件 `<qlgl/qldraw2d.h>`。上层只依赖它，因此 CPU / GPU 可互换。

```c
typedef struct QLDraw2D {
    void (*set_clip)(QLDraw2D *self, QLRect clip);

    void (*fill_rect)(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha);
    void (*fill_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                            ql_color_t color, uint8_t alpha);
    void (*stroke_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                              ql_coord_t thickness, ql_color_t color);

    /* from/to 各带自己的 alpha：a1/a2 沿渐变插值（CSS 的 rgba 色标） */
    void (*fill_round_rect_gradient)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                     ql_color_t from, ql_color_t to, uint8_t dir,
                                     uint8_t a1, uint8_t a2);
    void (*fill_shadow)(QLDraw2D *self, QLRect inner, ql_coord_t radius, ql_coord_t blur,
                        ql_coord_t dx, ql_coord_t dy, ql_color_t color, uint8_t alpha);

    void (*draw_text)(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                      QLRect box, ql_color_t color, QLTextAlign align);

    void (*flush)(QLDraw2D *self);   /* 可选，GPU 批次提交 */
    void *user;
} QLDraw2D;
```

| 函数 | 约定 |
|---|---|
| `set_clip` | 绝对坐标；与表面边界求交；后续绘制都被裁到它里面 |
| `fill_rect` / `fill_round_rect` | `alpha` 0=跳过，255=不透明；圆角半径自动裁到短边一半 |
| `stroke_round_rect` | **向内描边**；`thickness` ≤ 0 时无操作 |
| `fill_round_rect_gradient` | `dir` 取 `QLGradientDir`；只有 2 个停靠点；`a1`/`a2` 是两端色标各自的 alpha（都传 255 = 与旧版等价） |
| `fill_shadow` | 以 `inner` 为轮廓向外扩散 `blur` 并偏移 `(dx,dy)`；**必须先画阴影再画本体** |

### 5.x 高 DPI（DPR）

```c
/* 缩放（26.6 定点）：QL_SCALE_ONE = 1.0，96 = 1.5 */
void ql_draw2d_set_scale(QLDraw2D *d2d, ql_coord_t scale_fx);
void ql_canvas_set_scale(QLCanvas *canvas, ql_coord_t scale_fx);
void ql_disp_set_scale(QLDispDriver *driver, ql_coord_t scale_fx);
void ql_gfx_gl_set_scale(QLGfxGL *gfx, ql_coord_t scale_fx);

/* CSS px 下的表面尺寸（布局用这个，别用 width/height） */
ql_coord_t ql_canvas_css_width(const QLCanvas *c);
ql_coord_t ql_canvas_css_height(const QLCanvas *c);
ql_coord_t ql_disp_css_width(const QLDispDriver *d);
ql_coord_t ql_disp_css_height(const QLDispDriver *d);

/* 绘制期平移（CSS px 26.6）：translate(0.5px) → 32。不影响布局 */
void ql_draw2d_set_offset(QLDraw2D *d2d, ql_coord_t x_fx, ql_coord_t y_fx);

/* 亚像素填充：边落在小数上时按面积覆盖率抗锯齿（低 6 位是亚像素） */
typedef struct { ql_coord_t x, y, w, h; } QLRectFx;
void ql_canvas_fill_rect_fx(QLCanvas *c, QLRectFx r, ql_color_t color, uint8_t alpha);
void ql_canvas_fill_round_rect_fx(QLCanvas *c, QLRectFx r, ql_coord_t radius_fx,
                                  ql_color_t color, uint8_t alpha);
void ql_canvas_stroke_rect_fx(QLCanvas *c, QLRectFx r, ql_coord_t radius_fx,
                              ql_coord_t thickness_fx, ql_color_t color);

/* 按"边"缩放：相邻矩形不会在接缝处裂开 */
QLRect ql_scale_rect(QLRect r, ql_coord_t scale_fx);
QLRect ql_unscale_rect(QLRect r, ql_coord_t scale_fx);
ql_coord_t ql_scale_len(ql_coord_t v, ql_coord_t scale_fx);
```
| `draw_text` | `len < 0` 表示 `'\0'` 结尾；只处理 `'\n'`；水平按 `align` 对齐 |
| `flush` | CPU 为 `NULL`；GPU 用它提交批次 |

公共排版逻辑：

```c
void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text,
                           int32_t len, QLRect box, ql_color_t color,
                           QLTextAlign align, ql_draw2d_glyph_fn emit_glyph);
```

后端只需实现"怎么把单个字形变成一次绘制"，排版由它统一处理。

---

## 2. CPU 后端

```c
ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas);
```

把既有 `QLCanvas` 包成 `QLDraw2D`。用于 MCU、无 GPU 环境、离屏导出。

```c
QLCanvas canvas;
ql_canvas_from_disp(&canvas, disp);
QLDraw2D d2d;
ql_draw2d_canvas_init(&d2d, &canvas);
ql_ui_render(ui, &d2d);        /* 或 ql_ui_render_canvas(ui, &canvas) */
```

---

## 3. GPU 后端（GLES 3.0 / EGL）

头文件 `<qlgl/qlgfx_gl.h>`，编译开关 `QLGL_ENABLE_GL`（CMake：`QLGL_BUILD_GL`）。

```c
typedef struct QLGfxGL QLGfxGL;

enum {
    QL_GFX_GL_WINDOW    = 0,        /* 开窗口，直接看到界面 */
    QL_GFX_GL_OFFSCREEN = 1u << 0,  /* pbuffer 离屏，测试/导出用 */
};

QLGfxGL  *ql_gfx_gl_create(const char *title, ql_coord_t w, ql_coord_t h, uint32_t flags);
void      ql_gfx_gl_destroy(QLGfxGL *gfx);

void      ql_gfx_gl_begin(QLGfxGL *gfx, ql_color_t clear_color);
void      ql_gfx_gl_end(QLGfxGL *gfx);
QLDraw2D *ql_gfx_gl_draw(QLGfxGL *gfx);

/* 保留式脏区（窗口动画局部重绘）——见 §3.4 */
ql_err_t  ql_gfx_gl_set_retained(QLGfxGL *gfx, int on);
int       ql_gfx_gl_begin_rects(QLGfxGL *gfx, ql_color_t clear_color,
                                const QLRect *rects, int count);

int       ql_gfx_gl_poll(QLGfxGL *gfx);          /* 窗口：1 继续 / 0 关闭 */
int       ql_gfx_gl_wait(QLGfxGL *gfx, int timeout_ms);  /* 阻塞等事件（事件驱动用） */

void      ql_gfx_gl_set_vsync(QLGfxGL *gfx, int on);     /* 默认开 */
int       ql_gfx_gl_resize_pending(QLGfxGL *gfx);        /* 取一次即清零 */
void      ql_gfx_gl_size(const QLGfxGL *gfx, ql_coord_t *out_w, ql_coord_t *out_h);

uint32_t  ql_gfx_gl_frame_count(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_draw_calls(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_vertex_count(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_frame_us(QLGfxGL *gfx);      /* 上一帧 begin→end 的墙钟耗时 */
uint32_t  ql_gfx_gl_swap_us(QLGfxGL *gfx);       /* 其中 eglSwapBuffers 占多久 */
ql_err_t  ql_gfx_gl_read_pixels(QLGfxGL *gfx, uint8_t *rgba, uint32_t stride_bytes);
```

### 3.1 标准用法

```c
QLGfxGL *gfx = ql_gfx_gl_create("QLGL", 320, 240, QL_GFX_GL_WINDOW);
if (gfx == NULL) { /* 无 EGL/GLES3 → 落回 CPU 后端 */ }

QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
ql_gfx_gl_set_retained(gfx, 1);           /* 保留式画布（见 §3.4） */

for (;;) {
    if (!ql_gfx_gl_poll(gfx)) break;      /* 处理事件；关窗返回 0 */
    if (ql_gfx_gl_resize_pending(gfx)) {  /* 拖动缩放：重新布局 */
        ql_coord_t cw, ch;
        ql_gfx_gl_size(gfx, &cw, &ch);
        ql_ui_layout(ui, cw, ch);
        need_full = 1;
    }
    ql_input_dispatch();
    uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

    QLRect areas[16];
    int n = ql_ui_consume_dirty(ui, areas, 16);

    /* 没有脏区也没有动画 → 什么都不做，阻塞等事件（静止时 0 帧、0% CPU） */
    if (!need_full && n == 0 && moving == 0) {
        if (!ql_gfx_gl_wait(gfx, 32)) break;
        continue;
    }

    int full = ql_gfx_gl_begin_rects(gfx, BG, areas, n);
    if (full) ql_ui_render(ui, d2d);
    else for (int i = 0; i < n; i++) ql_ui_render_rect(ui, d2d, areas[i]);
    need_full = 0;

    d2d->fill_round_rect(d2d, bar, 4, QL_COLOR_RGB(0xF0,0xB0,0x30), 255);
    ql_gfx_gl_end(gfx);
}
ql_gfx_gl_destroy(gfx);
```

> **为什么要"事件驱动 + 阻塞等待"**：以前每帧都 `begin → render → swap`，
> 静止的界面也会以刷新率（如 144Hz）不停交换。拖动窗口时合成器还要重配窗口，
> 这些多余的帧会和它抢，表现就是"卡顿掉帧"。
> `ql_gfx_gl_wait()` 内部是 `poll(2)` X11 连接，没事件就睡着。
> 完整案例与实测数字见 [性能诊断](../../design/性能诊断.html) 与
> [教程 17](../../tutorial/17_perf.html)。

### 3.2 约束

| 项 | 说明 |
|---|---|
| 平台 | Linux（EGL + X11 窗口 / pbuffer）；Wayland 窗口待补 |
| GL 版本 | OpenGL **ES 3.0**（`#version 300 es`） |
| 纹理 | 一张 1024×1024 R8 字形图集（内置位图字体预烘，动态字体按需增量上传） |
| 字体 | 内置位图字体 + FreeType 动态字体（`ql_font_set_default_file`）都支持 |
| 批次 | 一帧 **1 个 draw call**；顶点上限 65536，超出丢弃 |
| 内存 | 顶点数组预分配（~3.9 MB @65536 顶点），稳态零 malloc |
| 失败处理 | 创建失败返回 `NULL` 并打印原因；调用方应回落 CPU 后端 |

> 顶点数组 65536 × 60 B ≈ 3.9 MB 是当前最占内存的一块，后续可降到 ~1 MB（顶点压缩）。

### 3.3 调试

| 环境变量 | 作用 |
|---|---|
| `QLGL_GL_DUMP_ATLAS=1` | 把字形图集导出到 `/tmp/qlgl_atlas.pgm` |

**帧耗时怎么读**：`ql_gfx_gl_frame_us()` 是 `begin → end` 的墙钟时间，
`ql_gfx_gl_swap_us()` 是其中 `eglSwapBuffers` 的部分。两者相减才是"我们花的"：

```
frame_us - swap_us  ≈ 提交 + 光栅
swap_us             ≈ 等垂直同步 + 合成器取帧
```

另外：收到 `ConfigureNotify` 后 **200ms 内后端自动把 `eglSwapInterval` 设为 0**
（拖动期间优先"跟手"，结束后恢复垂直同步）。原因是合成器忙于重配窗口时不会及时
送 frame callback，Mesa 的 Present/XCB 会一直等，实测最坏单帧卡 **1000ms**。
`ql_gfx_gl_set_vsync()` 改的是用户的意图，这个临时关闭是内建的、不需要宿主处理。

临时关闭期间**不会失去节流**：后端用跟 vsync 时的帧间隔（`pace_us`）当目标节奏，
在下一帧开头补睡到"上一帧结束 + pace_us"，所以拖动时帧率仍贴着刷新率，
不会飙到上千。这个等待发生在 `ql_gfx_gl_begin()` 里、计时开始之前，
因此 `ql_gfx_gl_frame_us()` 包含它（与跟 vsync 时口径一致）。

**例外**：收到 `ConfigureNotify` 之后的第一帧不等（内部置 `present_now`）——
窗口刚被拉大，新露出的区域等着画面，这一帧必须立刻推上去，否则就是"缩放闪白"。
所以宿主的循环里"读尺寸 → 重排 → 画 → swap"要在同一个迭代里做完，中间不要插别的等待。

**窗口管理器那边**：窗口创建时后端会把 `_NET_WM_SYNC_REQUEST_COUNTER` 写进属性、
把 `_NET_WM_SYNC_REQUEST` 加进 `WM_PROTOCOLS`，并在每帧 `swap` 之后回执。
这是 X11 交互式缩放"不跳帧"的关键（WM 等客户端画完再走下一步）；宿主不用管，
只是不要在无关的时刻长时间不画、又不调用 `ql_gfx_gl_wait()`
（那条路径会替你把"没东西可画"回执掉）。需要 libXext，CMake 找不到会自动退回不接协议。

两个环境变量（排查用，不需要改代码）：

| 变量 | 作用 |
|---|---|
| `QLGL_LAG_DEBUG=1` | 每帧打 `dt/work/swap`，每次 `ConfigureNotify` 再打一行"配置→上屏"毫秒数（正常 p50 ≈ 1ms） |
| `QLGL_KEEP_VSYNC=1` | 拖动期间也跟 vblank（某些驱动上不跟 vblank 会看到撕裂时用；代价是可能重新踩到 Mesa 等 1s 的老问题） |

日志会打印 `GL_RENDERER` / `GL_VERSION` / `GLSL` 版本，便于区分是 llvmpipe 还是真 GPU。

---
### 3.4 保留式脏区（`ql_gfx_gl_begin_rects`）

**问题**：窗口是双缓冲，`eglSwapBuffers` 之后后缓冲内容没有任何保证。
只重画脏区 ≈ 把其余部分清成底色（动画一停，没在动的元素全部消失）。

**做法**：`ql_gfx_gl_set_retained(gfx, 1)` 之后，后端把画布放在自己的 FBO 里，
每帧只清 + 只画脏区，帧末整块 `glBlitFramebuffer` 到窗口：

```c
ql_gfx_gl_set_retained(gfx, 1);                     /* 建窗口后调一次 */

for (;;) {
    ql_gfx_gl_poll(gfx);
    ql_input_dispatch();
    uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

    QLRect areas[16];
    int n = ql_ui_consume_dirty(ui, areas, 16);

    if (n == 0 && !first_frame && moving == 0) {
        /* 静止：不提交这一帧 */
    } else {
        int full = ql_gfx_gl_begin_rects(gfx, BG, areas, n);
        if (full)
            ql_ui_render(ui, d2d);                  /* 整屏（首帧/改尺寸/不可用） */
        else
            for (int i = 0; i < n; i++)
                ql_ui_render_rect(ui, d2d, areas[i]);
        ql_gfx_gl_end(gfx);
    }
}
```

| 返回值 / 参数 | 含义 |
|---|---|
| `begin_rects(...) == 1` | **这一帧必须整屏重画**：FBO 刚建、窗口改尺寸、或保留模式不可用。此时忽略 `areas` |
| `begin_rects(...) == 0` | 已经只清了 `areas` 覆盖的区域，按 `areas` 逐个 `ql_ui_render_rect` 即可 |
| `count == 0` 且返回 0 | 不会发生（无脏区时按整屏处理） |
| `set_retained(gfx, 0)` | 立刻释放 FBO，退回"每帧整屏清" |

约束与代价：

- `rects` 与 `ql_ui_consume_dirty()` 的输出同单位（CSS px），后端会乘 `scale_fx`。
- 多占一块 `w×h×4` 的纹理（360×260 ≈ 366 KB，1080p ≈ 8 MB）。MCU 上不要开。
- 局部重绘的正确性由 `test_gl_dirty` 守：连续多帧"只画脏区"与"整屏重画"逐像素相同。
- 这块 FBO 也是将来**毛玻璃/离屏合成**（M5.5 ⑨）的基础设施。


## 4. 后端一致性验收

```bash
./build/test_gl_vs_cpu
#  像素差异：2809/76800 = 3.66%（最大单通道差 239）
#  11 项检查，0 项失败
```

阈值：单通道差 ≤ 40、异常像素占比 ≤ 6%。差异集中在文字/圆角的抗锯齿边缘。

加新后端时，把这个测试扩展成"后端 vs CPU"即可复用。

---

## 5. 与其它模块的关系

| 模块 | 是否依赖 QLDraw2D |
|---|---|
| `qlui`（HTML/CSS 编译结果） | ✅ `ql_ui_render(ui, d2d)` |
| 界面层（`QLUI`） | ✅ 直接吃 `QLDraw2D`，CPU/GPU 同一份布局与绘制代码 |
| 显示后端（X11/Wayland） | ❌ 它们提供帧缓冲给 CPU 用 |

> 也就是说：**目前 GPU 路径服务于 qlui 界面**，控件路径仍是 CPU。
> 界面层的绘制已经全部走 `QLDraw2D`（见 `ql_ui_render(ui, d2d)`），
> 所以同一份 HTML/CSS 在 CPU 与 GPU 后端上像素一致（`test_gl_vs_cpu` 守着）。

---

## 附：CPU 光栅内部基础件（非公开）

`src/draw/qlcanvas.c` 内部有三个 `static` 基础件，**不对外暴露**，
但改绘制原语时必须按它们的约定写：

| 函数 | 约定 |
|---|---|
| `put_px(c, x, y, v)` | **调用方保证** `(x,y)` 已在裁剪区内；不检查、不混合 |
| `fill_row(c, y, x, n, color)` | 水平填 `n` 像素；按 `c->format` 分派一次，`v` 已按格式打包 |
| `round_row_span(...)` | 求圆角矩形第 `y` 行的两段可见区间 `[x0,xl)` / `[xr,x1)` |

设计要点：

- **裁剪只算一次**：进入行循环前就把矩形与 `clip` 求交，循环内零判断；
- **格式分派在行级**：不在像素级 `switch`；RGB888 走"4 像素 3 次 32 位写"；
- **抗锯齿不下沉到像素**：圆角/描边的覆盖率在**行跨度**层面近似，
  与 GPU 的 SDF 结果对比差异 3.66%（见 `test_gl_vs_cpu`），可接受。

新增绘制原语时：先写 `for (y)` 行循环拿跨度，再调 `fill_row`。
**禁止**写 `for (y) for (x) put_pixel()`——那正是 44 ms 的来源。
