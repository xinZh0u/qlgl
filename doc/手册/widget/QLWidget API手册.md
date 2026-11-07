# QLWidget / 绘制 API 手册（v0.1）

配套阅读：[QLWidget 阅读指南](./QLWidget阅读指南.md) · 对象系统：[QLObject API手册](../object/QLObject%20API手册.md)

- 头文件：`#include <qlgl/qlgl.h>`（已包含 `qlgeom.h` / `qldisp.h` / `qlcanvas.h` / `qlwidget.h` / `qlrender.h`）
- 链接：`target_link_libraries(xxx PRIVATE qlgl)`
- C11，无 POSIX 依赖

---

## 1. 最小可运行示例

```c
#include <qlgl/qlgl.h>

int main(void)
{
    qlgl_init();
    QLDispDriver *disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB565);
    ql_disp_set_bg_color(QL_COLOR_BLACK);

    QLWidget *root = ql_widget_new_ex(0, 0, 320, 240);
    ql_widget_set_bg((QLObject *)root, QL_COLOR_RGB(0x20, 0x20, 0x20), 255);
    ql_render_set_root((QLObject *)root);

    QLWidget *box = ql_widget_new_child((QLObject *)root, 20, 20, 120, 60);
    ql_widget_set_bg((QLObject *)box, QL_COLOR_RED, 255);
    ql_widget_set_border((QLObject *)box, 2, QL_COLOR_WHITE, 10);

    ql_render();                                  /* 首帧 */
    ql_disp_mem_save_ppm(disp, "out.ppm");        /* 看图 */

    ql_object_unref((QLObject *)root);
    ql_disp_mem_destroy(disp);
    qlgl_shutdown();
    return 0;
}
```

**默认背景是透明的**（`BG_ALPHA=0`），只设 `BG_COLOR` 不会显示，必须同时设 alpha。

---

## 2. 几何与颜色（`qlgeom.h`）

```c
typedef int32_t ql_coord_t;      /* 坐标；可整体改小以省 MCU 内存 */
typedef uint32_t ql_color_t;     /* 0xRRGGBB，与像素格式无关 */

#define QL_COLOR_RGB(r, g, b)
#define QL_COLOR_BLACK / WHITE / RED / GREEN / BLUE / GRAY / SKY
#define QL_COLOR_GET_R(c) / GET_G / GET_B

typedef struct { ql_coord_t x, y, w, h; } QLRect;

QLRect ql_rect_make(x, y, w, h);
int    ql_rect_is_empty(r);            /* w<=0 或 h<=0 */
ql_coord_t ql_rect_right(r);           /* x + w */
ql_coord_t ql_rect_bottom(r);          /* y + h */
QLRect ql_rect_intersect(a, b);        /* 空交集返回 (0,0,0,0) */
QLRect ql_rect_union(a, b);
int    ql_rect_overlaps(a, b);
int    ql_rect_contains_round(r, px, py, radius);
```

约定：`x/y` 是左上角，`w/h` 是尺寸，**右/下边界是不包含的**（`x + w` 已经是外部）。

---

## 3. 显示驱动（`qldisp.h`）

### 3.1 格式

| 枚举 | 每像素 | 说明 |
|---|---|---|
| `QL_COLOR_RGB565` | 2 B | MCU 常用 |
| `QL_COLOR_RGB888` | 3 B | 无量化损失 |
| `QL_COLOR_ARGB8888` | 4 B | 渲染器写入 alpha=0xFF |
| `QL_COLOR_L8` | 1 B | 灰度/单色屏 |

```c
uint32_t    ql_disp_format_bytes(uint8_t format);
const char *ql_disp_format_name(uint8_t format);
uint32_t    ql_disp_buffer_size(QLColorFormat, ql_coord_t w, ql_coord_t h);
```

### 3.2 驱动结构

```c
struct QLDispDriver {
    ql_coord_t width, height;
    uint8_t    format, rotation;
    void      *framebuffer;
    uint32_t   stride;                              /* 字节/行 */
    int      (*flush)(QLDispDriver *self, QLRect area);
    void      *user_data;
};
```

### 3.3 注册与配置

```c
ql_err_t      ql_disp_set(QLDispDriver *driver);   /* 设为活动显示 */
QLDispDriver *ql_disp_get(void);
void          ql_disp_set_bg_color(ql_color_t color);  /* 每帧清屏用 */
ql_color_t    ql_disp_bg_color(void);
ql_err_t      ql_disp_init_fb(QLDispDriver *d, void *fb, uint32_t stride,
                              ql_coord_t w, ql_coord_t h, QLColorFormat fmt,
                              int (*flush)(QLDispDriver *, QLRect));
```

`ql_disp_set` 会校验：`framebuffer != NULL`、宽高 > 0、格式合法。
`ql_disp_init_fb` 的 `stride == 0` 表示"按最紧凑宽度自动计算"。

### 3.4 对接真实屏的写法

```c
static uint16_t s_fb[320 * 240];

static int panel_flush(QLDispDriver *d, QLRect area)
{
    (void)d;
    lcd_set_window(area.x, area.y, area.x + area.w - 1, area.y + area.h - 1);
    lcd_write_pixels(&s_fb[area.y * 320 + area.x], area.w * area.h);
    return 0;                       /* 返回非 0 只打警告，不影响正确性 */
}

QLDispDriver drv;
ql_disp_init_fb(&drv, s_fb, 320 * sizeof(uint16_t), 320, 240, QL_COLOR_RGB565,
                panel_flush);
ql_disp_set(&drv);
```

### 3.5 内存驱动（PC / 测试 / 示例）

```c
QLDispDriver *ql_disp_mem_create(ql_coord_t w, ql_coord_t h, QLColorFormat fmt);
void          ql_disp_mem_destroy(QLDispDriver *driver);
void          ql_disp_mem_clear(QLDispDriver *driver, ql_color_t color);
ql_color_t    ql_disp_mem_pixel(QLDispDriver *driver, ql_coord_t x, ql_coord_t y);
ql_err_t      ql_disp_mem_save_ppm(QLDispDriver *driver, const char *path);
uint32_t      ql_disp_mem_flush_count(QLDispDriver *driver);
```

- `create` 会自动 `ql_disp_set`；
- `destroy` 会释放显存，若它是活动显示则一并注销；
- `save_ppm` 输出 P6 二进制 PPM，可直接用图片查看器打开；
- `pixel` 返回 `0xRRGGBB`（RGB565 已经还原成 8 位）。

---

## 4. 画布（`qlcanvas.h`）

```c
typedef struct {
    uint8_t *buffer; uint32_t stride;
    ql_coord_t width, height;
    QLRect clip; uint8_t format;
} QLCanvas;

ql_err_t ql_canvas_from_disp(QLCanvas *c, QLDispDriver *d);
ql_err_t ql_canvas_from_disp_clip(QLCanvas *c, QLDispDriver *d, QLRect clip);
void     ql_canvas_set_clip(QLCanvas *c, QLRect clip);   /* 自动与屏幕求交 */
QLRect   ql_canvas_clip(QLCanvas *c);
```

**坐标是屏幕绝对坐标**，`clip` 决定可见范围。

### 4.1 像素

```c
ql_color_t ql_canvas_read_pixel(QLCanvas *c, ql_coord_t x, ql_coord_t y);   /* 越界返回 0 */
void       ql_canvas_write_pixel(QLCanvas *c, ql_coord_t x, ql_coord_t y,
                                 ql_color_t color, uint8_t alpha);
```

`alpha`：0 完全透明（跳过）、255 完全不透明、其余与已有像素线性混合。

### 4.2 填充

```c
void ql_canvas_clear(QLCanvas *c, ql_color_t color);                       /* 填满 clip */
void ql_canvas_fill_rect(QLCanvas *c, QLRect r, ql_color_t color);
void ql_canvas_fill_rect_alpha(QLCanvas *c, QLRect r, ql_color_t color, uint8_t alpha);
void ql_canvas_fill_round_rect(QLCanvas *c, QLRect r, ql_coord_t radius, ql_color_t color);
void ql_canvas_fill_round_rect_alpha(QLCanvas *c, QLRect r, ql_coord_t radius,
                                     ql_color_t color, uint8_t alpha);
```

半径会被裁到 `min(w,h)/2`，超过自动退化，不会越界。

### 4.3 描边与线条

```c
void ql_canvas_stroke_rect(QLCanvas *c, QLRect r, ql_coord_t radius,
                           ql_coord_t thickness, ql_color_t color);   /* 向内描边 */
void ql_canvas_draw_hline(QLCanvas *c, ql_coord_t x1, ql_coord_t x2, ql_coord_t y,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_vline(QLCanvas *c, ql_coord_t x, ql_coord_t y1, ql_coord_t y2,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_line(QLCanvas *c, ql_coord_t x1, ql_coord_t y1,
                         ql_coord_t x2, ql_coord_t y2, ql_coord_t thickness,
                         ql_color_t color);
```

`stroke_rect` 的描边**向内**：控件尺寸不变，内容区被挤小。

### 4.4 量化

```c
ql_color_t ql_canvas_quantize(QLCanvas *c, ql_color_t color);
```

返回该格式下**实际会写进去**的颜色（RGB565 会丢低位、L8 转灰度）。
测试用它做精确断言。

---

## 5. 控件（`qlwidget.h`）

### 5.1 类型

```c
QL_DECLARE(QLWidget, ql_widget)
#define QL_WIDGET_TYPE  ql_widget_get_type()
```

### 5.2 属性

| 属性 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `QL_WIDGET_PROPERTY_X` | INT | 0 | 相对父控件 |
| `QL_WIDGET_PROPERTY_Y` | INT | 0 | 相对父控件 |
| `QL_WIDGET_PROPERTY_WIDTH` | INT | 0 | 负数被拒绝 |
| `QL_WIDGET_PROPERTY_HEIGHT` | INT | 0 | 负数被拒绝 |
| `QL_WIDGET_PROPERTY_BG_COLOR` | COLOR/UINT | 白 | |
| `QL_WIDGET_PROPERTY_BG_ALPHA` | INT | **0** | 0 = 透明；255 = 不透明 |
| `QL_WIDGET_PROPERTY_BORDER_WIDTH` | INT | 0 | 0 = 不画边框 |
| `QL_WIDGET_PROPERTY_BORDER_COLOR` | COLOR/UINT | 黑 | |
| `QL_WIDGET_PROPERTY_BORDER_RADIUS` | INT | 0 | 0 = 直角 |
| `QL_WIDGET_PROPERTY_CLIP_CHILDREN` | BOOL/INT | 0 | 1 = 子控件被裁到父矩形内 |
| `QL_WIDGET_PROPERTY_VISIBLE` | BOOL/INT | 1 | 0 = 整棵子树不画 |

属性变化会自动标记脏区，不需要手动调用任何刷新函数。

### 5.3 创建

```c
QLWidget *ql_widget_new(void);
QLWidget *ql_widget_new_ex(ql_coord_t x, ql_coord_t y, ql_coord_t w, ql_coord_t h);
QLWidget *ql_widget_new_child(QLObject *parent, ql_coord_t x, ql_coord_t y,
                              ql_coord_t w, ql_coord_t h);
```

`ql_widget_new_child` 等价于 `new_ex` + `ql_object_add_child`，失败返回 `NULL`。
用 `new_ex` 创建的控件调用方持有引用，需要自己 `ql_object_unref`；
用 `new_child` 创建的由父对象接管，**不要**再 `unref`。

### 5.4 便捷访问

```c
void       ql_widget_set_pos(QLObject *obj, ql_coord_t x, ql_coord_t y);
void       ql_widget_set_bg(QLObject *obj, ql_color_t color, uint8_t alpha);
void       ql_widget_set_border(QLObject *obj, ql_coord_t width, ql_color_t color,
                                ql_coord_t radius);
void       ql_widget_set_visible(QLObject *obj, int visible);

ql_coord_t ql_widget_x(QLObject *obj);
ql_coord_t ql_widget_y(QLObject *obj);
ql_coord_t ql_widget_width(QLObject *obj);
ql_coord_t ql_widget_height(QLObject *obj);
int        ql_widget_is_visible(QLObject *obj);
int        ql_widget_clips_children(QLObject *obj);
QLRect     ql_widget_local_rect(QLObject *obj);   /* 相对父控件 */
QLRect     ql_widget_abs_rect(QLObject *obj);     /* 屏幕绝对坐标 */
int        ql_widget_contains(QLObject *obj, ql_coord_t abs_x, ql_coord_t abs_y);
```

全部对 `NULL` 与非控件对象安全：返回 0 或空矩形，不崩。

### 5.5 自定义控件（覆盖绘制）

```c
/* ① id 枚举 */
QL_PROPERTY_ENUM_EMPTY(QL_BOX, QLBox, QL_WIDGET, 10)
QL_EVENT_ENUM_EMPTY(QL_BOX, QLBox, QL_WIDGET, 10)

/* ② 声明 */
QL_DECLARE(QLBox, ql_box)
#define QL_BOX_TYPE  ql_box_get_type()

struct QLBox        { QLWidget parent; };
struct QLBoxClass   { QLWidgetClass parent; };
struct QLBoxPrivate { ql_color_t color; };

/* ③ 定义 */
QL_DEFINE_WITH_PRIVATE(QL_BOX, QLBox, ql_box, ql_widget)

/* ④ 绘制：先画背景边框，再画自己的内容 */
static void ql_box_draw(QLWidget *self, QLCanvas *canvas)
{
    ql_widget_draw_default(self, canvas);

    struct QLBoxPrivate *priv = ql_box_get_private((QLBox *)(void *)self);
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    ql_canvas_draw_line(canvas, r.x, r.y, ql_rect_right(r) - 1,
                        ql_rect_bottom(r) - 1, 1, priv->color);
}

/* ⑤ 钩子 */
static void ql_box_class_init(QLBoxClass *klass)
{
    ql_widget_class_set_draw(((QLObjectClass *)(void *)klass)->type, ql_box_draw);
}
static void ql_box_init(QLBox *self)
{
    struct QLBoxPrivate *priv = ql_box_get_private(self);
    priv->color = QL_COLOR_RED;
}
static void ql_box_deinit(QLBox *self) { QL_UNUSED(self); }

/* ⑥ 使用 */
QLType type = QL_REGISTER(ql_box, QL_WIDGET_TYPE);
QLBox *box = (QLBox *)ql_object_new(type);
```

要点：

- 绘制函数签名 `void (*)(QLWidget *self, QLCanvas *canvas)`；
- 画布是**屏幕坐标**，用 `ql_widget_abs_rect(self)` 取自己的位置；
- 只画内容，背景/边框调用 `ql_widget_draw_default`；
- 绘制函数通过 `ql_widget_class_set_draw` 登记到**本类型的 id** 上，子类再覆盖即可。

### 5.6 绘制相关接口

```c
typedef void (*ql_widget_draw_func)(QLWidget *self, QLCanvas *canvas);

void                ql_widget_class_set_draw(QLType type, ql_widget_draw_func draw);
ql_widget_draw_func ql_widget_class_get_draw(QLType type);
ql_widget_draw_func ql_widget_get_draw(QLObject *obj);   /* 沿类链找最近的 */
void                ql_widget_draw_default(QLWidget *self, QLCanvas *canvas);
void                ql_widget_reset(void);               /* qlgl_init/shutdown 内部用 */
```

### 5.7 Linux 窗口后端（`qldisp_x11.h`）

编译期由 CMake 打开：找到 X11 时自动定义 `QLGL_ENABLE_X11=1`；未启用时本节接口不参与编译。

```c
QLDispDriver *ql_disp_x11_create(const char *title, ql_coord_t width, ql_coord_t height,
                                 QLColorFormat format);
int           ql_disp_x11_poll(QLDispDriver *driver);   /* 1 = 继续，0 = 窗口已关闭 */
void          ql_disp_x11_destroy(QLDispDriver *driver);
const char   *ql_disp_x11_title(QLDispDriver *driver);
uint32_t      ql_disp_x11_expose_count(QLDispDriver *driver);
```

| 项 | 说明 |
|---|---|
| `format` | 传 `QL_COLOR_FORMAT_AUTO` 按 X 服务器视觉自动选（通常是 `ARGB8888`） |
| 返回值 | 成功时驱动已被 `ql_disp_set` 设为活动显示；无 `DISPLAY` 时返回 `NULL` 并打错误日志 |
| 像素存储 | **XImage 的像素内存就是渲染器的帧缓冲**，绘制与上屏之间无额外拷贝 |
| `flush` | 只把脏矩形用 `XPutImage` 推给窗口 |
| 退出条件 | 点窗口关闭按钮、按 `Esc` / `q`、收到 `DestroyNotify` |
| `Expose` 事件 | 自动 `ql_render_invalidate_all()`，窗口被遮挡后恢复会整屏重画 |

支撑的视觉格式：32bpp/24bpp（TrueColor，掩码 RGB），16bpp RGB565。其他视觉会拒绝并打错误日志。

### 5.8 Linux Wayland 后端（`qldisp_wayland.h`）

编译期由 CMake 的 `QLGL_BUILD_WAYLAND` 控制：找到 `wayland-client` 与 `wayland-scanner`
时自动定义 `QLGL_ENABLE_WAYLAND=1`。`xdg-shell.xml` 已随仓库提供
（`src/port/protocols/`），构建时用 `wayland-scanner` 生成协议代码，不依赖系统安装 wayland-protocols。

```c
QLDispDriver *ql_disp_wayland_create(const char *title, ql_coord_t w, ql_coord_t h,
                                     QLColorFormat format);
int           ql_disp_wayland_poll(QLDispDriver *driver);   /* 1 = 继续，0 = 已关闭 */
void          ql_disp_wayland_destroy(QLDispDriver *driver);
const char   *ql_disp_wayland_title(QLDispDriver *driver);
uint32_t      ql_disp_wayland_commit_count(QLDispDriver *driver);
uint32_t      ql_disp_wayland_skip_count(QLDispDriver *driver);
```

| 项 | 说明 |
|---|---|
| 支持的格式 | 只有 `QL_COLOR_ARGB8888`（对应 `WL_SHM_FORMAT_XRGB8888`）；传 AUTO 也可以 |
| 帧缓冲 | 渲染器写一块**持久**缓冲；每次 flush 把脏矩形拷进 shm 缓冲区 |
| 双缓冲 | 两个 `wl_buffer` 轮换；两个都在用时**跳过本帧**（`skip_count++`），不撕裂 |
| 提交合并 | 一帧内的所有脏区在 `flush_done` 里合并成一次 `attach + damage + commit` |
| 退出条件 | 点窗口关闭按钮、按 `Esc` / `q` |
| 环境变量 | `WAYLAND_DISPLAY`、`XDG_RUNTIME_DIR`（桌面会话里通常已设置） |
| 键盘 | 直接读 evdev 键码（`1`=Esc，`16`=q），不依赖 xkbcommon，因此只有这两个键可用 |

**和 X11 后端的差别**：X11 的 XImage 数据直接就是渲染器的帧缓冲（零拷贝）；
Wayland 因为要轮流 attach 两个缓冲区，必须多一次脏矩形拷贝。拷贝量只跟脏区面积有关。

### 5.9 后端选择建议

```c
QLDispDriver *d = NULL;
#if defined(QLGL_ENABLE_WAYLAND)
    d = ql_disp_wayland_create("QLGL Demo", 320, 240, QL_COLOR_FORMAT_AUTO);
#endif
#if defined(QLGL_ENABLE_X11)
    if (d == NULL)
        d = ql_disp_x11_create("QLGL Demo", 320, 240, QL_COLOR_FORMAT_AUTO);
#endif
if (d == NULL)
    d = ql_disp_mem_create(320, 240, QL_COLOR_RGB888);   /* 无桌面环境 */
```

`test/widget/example_window.c` 就是按这个顺序自动降级的，也可以用
`--backend=wayland|x11` 强制指定。

标准用法：

```c
QLDispDriver *disp = ql_disp_x11_create("QLGL Demo", 320, 240, QL_COLOR_FORMAT_AUTO);
if (disp == NULL) {
    disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB888);   /* 降级 */
}

ql_render_set_root((QLObject *)root);

while (ql_disp_x11_poll(disp)) {
    /* 改属性 -> 自动打脏 */
    ql_render();
}
ql_disp_x11_destroy(disp);
```

> 窗口示例会进入事件循环，因此 **不注册为 ctest 用例**（CMake 里按名字排除）。
> 用 `--frames N` 可以跑固定帧数后自动退出，便于脚本化验证。

---

## 6. 渲染器（`qlrender.h`）

```c
ql_err_t  ql_render_set_root(QLObject *root);    /* 根可以不是控件 */
QLObject *ql_render_root(void);

void      ql_render_invalidate_all(void);        /* 整屏重绘 */
void      ql_render_invalidate_rect(QLRect area);
void      ql_render_invalidate(QLObject *obj);   /* 按控件绝对矩形 */

uint32_t  ql_render_dirty_count(void);
void      ql_render_dirty_rect(uint32_t index, QLRect *out);

int       ql_render(void);                       /* 返回绘制的脏区数量 */
```

| 情况 | `ql_render()` 返回 |
|---|---|
| 正常绘制 n 个脏区 | `n` |
| 无脏区 | `0`（不做任何工作） |
| 未设显示或未设根 | `QL_ERR_NOT_INIT`（负数） |

### 典型主循环

```c
for (;;) {
    handle_input();
    ql_render();          /* 有脏区才真正画 */
    idle();
}
```

### 脏区行为

- 属性变化、挂接/摘除控件都会自动打脏；
- 相交的脏区会合并，最多跟踪 `QL_CFG_DIRTY_RECT_MAX`（默认 16）块，满了并成一块；
- 超出屏幕的部分被裁掉；
- 未挂进渲染树的对象不会打脏。

---

## 7. 配置项（`qlconfig.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `QL_CFG_DIRTY_RECT_MAX` | 16 | 同时跟踪的脏矩形数量 |
| `QL_CFG_MAX_CLASS` | 64 | 类表容量，也是绘制表的长度 |

CMake 选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `QLGL_BUILD_X11` | ON | 在 Linux 上构建 X11 窗口后端（需要 libX11 开发包） |
| `QLGL_BUILD_WAYLAND` | ON | 在 Linux 上构建 Wayland 窗口后端（需要 wayland-client 与 wayland-scanner） |
| `QLGL_BUILD_TESTS` | ON | 构建 `test/` 下的示例与单元测试 |

---

## 8. 限制与后续

| 项 | v0.1 状态 |
|---|---|
| 布局 | 只支持绝对定位（`x/y/w/h`），无盒模型 / flex |
| 文字 | 未实现（`QLCanvas` 尚无字形绘制） |
| 图片 | 未实现 |
| 抗锯齿 | 未实现，圆角是硬边 |
| 图层合成 | 只有"源 alpha 与已有像素混合"，无独立图层 |
| 缓冲策略 | 需要至少一屏显存（直绘）；部分缓冲后端待补 |
| 线程 | 非线程安全 |

---

## 9. 示例与测试

| 文件 | 内容 |
|---|---|
| `test/widget/example_widget.c` | 卡片 + 标题条 + 主/次按钮 + 自定义徽标，输出 PPM |
| `test/widget/example_window.c` | **真实窗口**（Wayland / X11）：动画 + 文字 + 增量重绘 |
| `test/widget/test_canvas.c` | 每个绘制原语的像素级断言（含 RGB565/L8/ARGB8888） |
| `test/widget/test_widget_basic.c` | 属性、几何换算、命中测试、可见性 |
| `test/widget/test_widget_render.c` | 层叠、圆角、alpha、裁剪、脏矩形合并、子类 draw |

```bash
cmake --build build
./build/example_widget /tmp/out.ppm
./build/example_window                      # 自动选择后端（优先 Wayland）
./build/example_window --backend=x11        # 强制 X11
./build/example_window --frames 120         # 跑 120 帧后自动退出
ctest --test-dir build --output-on-failure
```
