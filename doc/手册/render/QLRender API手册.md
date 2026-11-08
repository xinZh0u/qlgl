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

    void (*fill_round_rect_gradient)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                     ql_color_t from, ql_color_t to, uint8_t dir,
                                     uint8_t alpha);
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
| `fill_round_rect_gradient` | `dir` 取 `QLGradientDir`；只有 2 个停靠点 |
| `fill_shadow` | 以 `inner` 为轮廓向外扩散 `blur` 并偏移 `(dx,dy)`；**必须先画阴影再画本体** |
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

int       ql_gfx_gl_poll(QLGfxGL *gfx);          /* 窗口：1 继续 / 0 关闭 */

uint32_t  ql_gfx_gl_frame_count(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_draw_calls(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_vertex_count(QLGfxGL *gfx);
uint32_t  ql_gfx_gl_frame_us(QLGfxGL *gfx);      /* 上一帧 begin→end 的 CPU 侧耗时 */
ql_err_t  ql_gfx_gl_read_pixels(QLGfxGL *gfx, uint8_t *rgba, uint32_t stride_bytes);
```

### 3.1 标准用法

```c
QLGfxGL *gfx = ql_gfx_gl_create("QLGL", 320, 240, QL_GFX_GL_WINDOW);
if (gfx == NULL) { /* 无 EGL/GLES3 → 落回 CPU 后端 */ }

QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
while (ql_gfx_gl_poll(gfx)) {
    ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x08, 0x0A, 0x10));
    ql_ui_render(ui, d2d);                /* 界面 */
    d2d->fill_round_rect(d2d, bar, 4, QL_COLOR_RGB(0xF0,0xB0,0x30), 255);
    ql_gfx_gl_end(gfx);                   /* 上传 + 1 次 draw + swap */
}
ql_gfx_gl_destroy(gfx);
```

### 3.2 约束

| 项 | 说明 |
|---|---|
| 平台 | Linux（EGL + X11 窗口 / pbuffer）；Wayland 窗口待补 |
| GL 版本 | OpenGL **ES 3.0**（`#version 300 es`） |
| 纹理 | 只建一张 512×512 R8 字形图集 |
| 字体 | 只支持内置 `ql_font_mono_12/16`（其他字体不画文字） |
| 批次 | 一帧 **1 个 draw call**；顶点上限 65536，超出丢弃 |
| 内存 | 顶点数组预分配（~3.9 MB @65536 顶点），稳态零 malloc |
| 失败处理 | 创建失败返回 `NULL` 并打印原因；调用方应回落 CPU 后端 |

> 顶点数组 65536 × 60 B ≈ 3.9 MB 是当前最占内存的一块，后续可降到 ~1 MB（顶点压缩）。

### 3.3 调试

| 环境变量 | 作用 |
|---|---|
| `QLGL_GL_DUMP_ATLAS=1` | 把字形图集导出到 `/tmp/qlgl_atlas.pgm` |

日志会打印 `GL_RENDERER` / `GL_VERSION` / `GLSL` 版本，便于区分是 llvmpipe 还是真 GPU。

---

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
| 控件（`QLWidget`/`QLLabel`） | ❌ 仍走 `QLCanvas`（CPU），后续迁移 |
| 显示后端（X11/Wayland） | ❌ 它们提供帧缓冲给 CPU 用 |

> 也就是说：**目前 GPU 路径服务于 qlui 界面**，控件路径仍是 CPU。
> 下一步可以把 `ql_widget_draw_func` 也改成 `QLDraw2D`，让控件同样能用 GPU。

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
