# QLText / 字体 API 手册（v0.1）

配套阅读：[QLText 阅读指南](./QLText阅读指南.md) · 相关：[QLUI API手册](../ui/QLUI%20API%20手册.md)（界面里的文本样式）

- 头文件：`#include <qlgl/qlgl.h>`（已包含 `qlfont.h` / `qlstr.h`）
- **文本最终由谁画**：界面层的 `QLUI` 引擎（HTML/CSS 的 `font-size` / `font-weight` … 编译成样式），
  或者你在画布上直接调 `ql_canvas_draw_text()`。这一章讲的是**字体与测量**那一层，
  没有"文本控件"这种对象 —— 显示文本是 `<span>` / `<p>` 的事。
- C11，无 POSIX 依赖

---

## 1. 最小示例

两条路都只依赖字体层，选哪条看你有没有界面：

**① 画布上直接画（最省，没有任何页面）**

```c
#include <qlgl/qlgl.h>

int main(void)
{
    qlgl_init();
    QLDispDriver *d = ql_disp_mem_create(240, 60, QL_COLOR_RGB888);
    QLCanvas cv;
    ql_canvas_from_disp(&cv, d);
    ql_canvas_clear(&cv, QL_COLOR_RGB(0x10, 0x14, 0x1C));
    ql_canvas_draw_text(&cv, &ql_font_mono_16, "Hello, QLGL!", -1,
                        ql_rect_make(8, 8, 224, 24), QL_COLOR_WHITE, QL_TEXT_ALIGN_LEFT);
    ql_disp_mem_save_ppm(d, "/tmp/hello.ppm");
    ql_disp_mem_destroy(d);
    qlgl_shutdown();
    return 0;
}
```

**② 交给界面层（字号/字重/颜色写进 CSS）**

```html
<div id="r">Hello, QLGL!</div>
```
```css
#r { font-size: 16px; color: #FFFFFF; }
```

```c
QLUIDoc *doc = ql_ui_load_text(html, css);
QLUIRoot *ui = ql_ui_build(doc);
ql_ui_layout(ui, 240, 60);
ql_ui_render_canvas(ui, &cv);
```

界面层内部就是用下面这些字体 API 测量与绘制的，所以两边字形完全一致。

---

## 2. 内置字体

```c
extern const QLFont ql_font_mono_12;   /* DejaVu Sans Mono, 12px, 4bpp */
extern const QLFont ql_font_mono_16;   /* DejaVu Sans Mono, 16px, 4bpp */
```

| 字段 | 说明 |
|---|---|
| `name` | `"DejaVuSansMono-16"` |
| `bpp` | `4`（抗锯齿灰度） |
| `line_height` / `ascent` / `descent` | 行高与基线位置，`ascent + descent == line_height` |
| `fallback` | `'?'`，缺字时用它 |
| `glyph_count` | 95（ASCII 32..126） |
| `glyphs` / `bitmap` | 字形表与位图数据 |

覆盖范围：**仅 ASCII 32..126**。中文/日文/emoji 会显示为 `?`。
字形数据由 `tools/qlfontgen/gen_bitmap_font.py` 从 TTF 生成。

> 需要中文/任意字号就用下面的 FreeType 动态字体（§2.2），内置位图字体是
> MCU 的保底方案，两者接口完全一样。

```bash
python3 tools/qlfontgen/gen_bitmap_font.py           # 重新生成
python3 tools/qlfontgen/gen_bitmap_font.py --check   # CI 校验
```

---

## 2.2 FreeType 动态字体（M2）

头文件 `<qlgl/qlfont_ft.h>`，需要 `QLGL_ENABLE_FREETYPE`（CMake 选项
`QLGL_BUILD_FREETYPE=ON`，自动探测 `freetype2`）。

```c
QLFont *ql_font_ft_load(const char *path, int face_index, int pixel_size);
```

| 参数 | 说明 |
|---|---|
| `path` | `.ttf` / `.otf` / `.ttc` |
| `face_index` | TTC 里的字面下标；**`-1` 自动挑**（优先 family 名带 `SC` 的简体中文字面） |
| `pixel_size` | 字号（像素，不是 pt） |

返回的 `QLFont*` 由调用方 **`ql_font_destroy()`** 释放。失败返回 `NULL`（打错误日志）。

```c
ql_font_ft_available()                 /* 后端是否可用（编译进来就是 1） */
ql_font_ft_set_cache_limit(uint32_t)   /* 每字体字形缓存上限，默认 2048，全局生效 */
```

### 默认字体：按字号取

```c
int         ql_font_set_default_file(const char *path, int face_index);  /* 1 = 成功 */
const char *ql_font_default_file(void);
const QLFont *ql_font_default(int pixel_size);   /* 带缓存，返回值不要释放 */
```

- 设了默认文件 → `ql_font_default(N)` 返回该字号的动态字体（同字号复用）；
- 没设或加载失败 → **回落到最接近的内置位图字体**（`>=14` 用 mono_16，否则 mono_12）。

> 生命周期：默认字体表由 `qlgl_shutdown()` 释放（`ql_font_reset_defaults()`），
> 所以界面/画布必须在 shutdown 之前用完最后一帧，否则会留下悬垂字体指针。
> 表容量固定 8 个字号，超出后不再缓存（每次新建，调用方别依赖指针相等）。
> 单线程使用；与其它 GL/绘制状态一样不做加锁。

只有界面层直接用它 —— CSS 里写 `font-size: 18px;` 就会拿到 18px 的动态字体：

```c
const QLFont *f = ql_font_default(18);   /* 同字号复用；失败回落到内置位图字体 */
```

手写画布代码同样可以这么取字体，再交给 `ql_canvas_draw_text()`。

### 动态字体的行为约定

| 项 | 说明 |
|---|---|
| 光栅化 | 缺字才做（`FT_LOAD_DEFAULT` + `FT_RENDER_MODE_NORMAL`），8bpp 灰度 |
| 缓存 | 有序数组 + 每字形独立位图；插入时 `memmove` 保持按 codepoint 升序 |
| 上限 | 达到 `cache_limit` 后**拒绝新字形**并告警一次（不做 LRU 淘汰） |
| 缺字 | `FT_Get_Char_Index == 0` → 返回 `NULL` → 走 `font->fallback` |
| 空字形 | 空格这类 `box_w == 0` 的字形也会缓存，避免每次重新走一遍 FreeType |
| 指针有效期 | `ql_font_find_glyph` 的返回值在**下一次查找前**有效 |

### 平台无关的读取接口

```c
const uint8_t *ql_font_glyph_row(const QLFont *font, const QLGlyph *glyph, int32_t y);
uint8_t        ql_font_glyph_alpha(const QLFont *font, const QLGlyph *glyph, int32_t x, int32_t y);
int            ql_font_is_dynamic(const QLFont *font);
void           ql_font_destroy(QLFont *font);       /* 静态字体上是空操作 */
```

写自己的绘制后端时**只用这两个访问器**取像素，就能同时支持两种字体。

---

## 3. 字体结构（自定义字体用）

```c
typedef struct {
    uint32_t codepoint;
    int16_t  adv_w;        /* 前进宽度 */
    uint8_t  box_w, box_h; /* 墨迹尺寸 */
    int8_t   ofs_x, ofs_y; /* 墨迹左上角相对笔位/基线（ofs_y 通常为负） */
    uint16_t data_offset;  /* 相对 QLFont.bitmap 的字节偏移 */
} QLGlyph;

typedef struct QLFont {
    const char     *name;
    uint8_t         bpp;          /* 1 / 4 / 8 */
    int16_t         line_height;
    int16_t         ascent;
    int16_t         descent;
    uint32_t        fallback;     /* 缺字 codepoint，0 = 不画只前进 */
    uint16_t        glyph_count;
    const QLGlyph  *glyphs;       /* 按 codepoint 升序 */
    const uint8_t  *bitmap;
} QLFont;
```

位图行字节数：

| bpp | 每行字节 |
|---|---|
| 1 | `(box_w + 7) / 8`，高位在左 |
| 4 | `(box_w + 1) / 2`，高半字节在左 |
| 8 | `box_w` |

**自定义字体的硬性要求**：`glyphs` 必须按 `codepoint` 升序（查找用二分）。

---

## 4. 字形与度量

```c
const QLGlyph *ql_font_find_glyph(const QLFont *font, uint32_t codepoint);
uint32_t       ql_font_glyph_stride(const QLFont *font, const QLGlyph *glyph);
uint8_t        ql_font_glyph_alpha(const QLFont *font, const QLGlyph *glyph,
                                   int32_t x, int32_t y);
```

| 函数 | 说明 |
|---|---|
| `find_glyph` | 二分查找；找不到则返回 `fallback` 字形，再找不到返回 `NULL` |
| `glyph_stride` | 该字形的位图行字节数；空字形（如空格）返回 0 |
| `glyph_alpha` | 覆盖率 0..255；越界或 `NULL` 返回 0；4bpp 下一定是 17 的倍数 |

```c
void ql_font_measure_text(const QLFont *font, const char *text, int32_t len,
                          ql_coord_t *out_w, ql_coord_t *out_h);
ql_coord_t ql_font_line_width(const QLFont *font, const char *text, int32_t len);
uint32_t   ql_font_line_count(const char *text, int32_t len);
```

| 规则 | 说明 |
|---|---|
| `len < 0` | 以 `'\0'` 结尾 |
| 宽度 | 最宽的一行 |
| 高度 | 行数 × `line_height` |
| 换行 | 只认 `'\n'` |
| `out_w` / `out_h` | 可为 `NULL` |

---

## 5. UTF-8

```c
int32_t ql_utf8_decode(const char *text, uint32_t *codepoint);   /* 返回消费字节数 */
int32_t ql_utf8_encode(uint32_t codepoint, char *buf);           /* 返回写入字节数 */
```

- 合法序列返回 1..4；
- 非法/截断序列返回 **1** 且 `*codepoint = 0xFFFD`（保证调用方一定前进，不会死循环）；
- `encode` 的 `buf` 至少 4 字节。

```c
const char *s = "A你😀";
int32_t i = 0;
uint32_t cp;
while (s[i] != '\0') {
    i += ql_utf8_decode(s + i, &cp);
    printf("U+%04X\n", cp);
}
```

---

## 6. 字符串 QLStr

```c
typedef enum { QL_STR_COMPACT = 0, QL_STR_AMORTIZED = 1 } QLStrPolicy;

typedef struct {
    char *data; uint32_t len, capacity;
    uint8_t own, policy; uint16_t reserved;
} QLStr;

#define QL_STR_INIT { NULL, 0, 0, 0, QL_STR_COMPACT, 0 }
```

| 函数 | 说明 |
|---|---|
| `ql_str_init(&s)` | 初始化（可用 `QL_STR_INIT` 静态初始化） |
| `ql_str_free(&s)` | 释放并复位 |
| `ql_str_set(&s, text)` | 复制内容；`text == NULL` 等价于清空 |
| `ql_str_set_n(&s, text, len)` | 复制前 len 字节，允许内容不含 `'\0'` 结尾 |
| `ql_str_set_static(&s, text)` | **不复制**，引用外部只读串 |
| `ql_str_append(&s, text)` | 追加 |
| `ql_str_clear(&s)` | 清空（amortized 策略保留容量） |
| `ql_str_data(&s)` | 内容指针，空串返回 `""` 而不是 `NULL` |
| `ql_str_len(&s)` | 字节长度 |
| `ql_str_equals(&s, text)` | 内容比较 |
| `ql_str_set_policy` / `ql_str_policy` | 切换/查询策略 |

两种策略的实际行为：

| 操作 | COMPACT | AMORTIZED |
|---|---|---|
| 设为 5 字符 | 容量 6 | 容量 16 |
| 缩短为 2 字符 | 容量 3 | 容量不变 |
| 追加到 1000 字符 | 每次重新分配到精确大小 | 按 2 倍增长，重分配次数 ≈ log₂ |
| `clear` | 释放内存 | 保留容量，只置空 |

只读串（`set_static`）在需要写入时会自动**转成自有内存副本**（`own` 变 1），
不会去修改原来的只读内存。

---

## 7. 画布文本原语

```c
typedef enum {
    QL_TEXT_ALIGN_LEFT = 0,
    QL_TEXT_ALIGN_CENTER,
    QL_TEXT_ALIGN_RIGHT,
} QLTextAlign;

void ql_canvas_draw_text(QLCanvas *canvas, const QLFont *font, const char *text,
                         int32_t len, QLRect box, ql_color_t color, QLTextAlign align);

void ql_canvas_draw_glyph(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color);
```

`draw_text` 的定位规则：

| 项 | 规则 |
|---|---|
| 水平 | 在 `box` 宽度内按 `align` 对齐 |
| 垂直 | 首行基线 = `box.y + font->ascent`，向下每行 `+line_height` |
| 裁剪 | 自动裁剪到 `box ∩ canvas->clip`，不会溢出控件 |
| 换行 | 只处理 `'\n'`，不自动折行 |
| 抗锯齿 | 像素级 alpha 混合（覆盖率即 alpha） |

`draw_glyph` 是单字形版本，坐标是**基线笔位**，一般不用直接调用。

---

## 8. 界面里的文本（没有"文本控件"）

QLGL 没有 `QLLabel` 这样的控件类 —— 文本是 **HTML 元素**，样式是 CSS：

```html
<p class="title">QLGL</p>
<span>普通文本</span>
```
```css
.title { font-size: 20px; font-weight: bold; color: #7CE0FF; }
```

| 想要的效果 | 写法 |
|---|---|
| 字号 | `font-size: 18px` |
| 颜色 | `color: #RRGGBB` |
| 对齐 | `text-align: left\|center\|right` |
| 加粗 / 斜体 / 下划线 / 删除线 | `font-weight: bold` / `font-style: italic` / `text-decoration: underline\|line-through` |
| 行高 | `line-height: 24px`（数值或百分比） |
| 换行 | 放 `<br>`，或写 `white-space: pre` 并在文本里留 `\n` |
| 不折行 | `white-space: nowrap` |

**运行期改文本**（唯一允许的结构改动，见 [QLUI API手册](../ui/QLUI%20API%20手册.md)）：

```c
ql_ui_set_text(ui, node, "新内容");
```

布局与绘制都是界面层的事，用例见 `example/02_text/main.c` / `example/08_tags/main.c`。

---

## 9. CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `QLGL_BUILD_X11` | ON | Linux X11 窗口后端 |
| `QLGL_BUILD_WAYLAND` | ON | Linux Wayland 窗口后端（需要 wayland-client + wayland-scanner） |
| `QLGL_BUILD_TESTS` | ON | 构建 `test/` 下示例与测试 |
| `QLGL_BUILD_FREETYPE` | ON | FreeType 动态字体后端（中文 + 任意字号） |

---

## 10. 限制

| 项 | v0.1 状态 |
|---|---|
| 字符集 | 内置位图字体只有 ASCII；**启用 FreeType 后支持任意 Unicode** |
| 字号 | 内置只有 12/16px；动态字体任意字号 |
| 自动折行 | 不支持，只有手动 `'\n'` |
| 垂直对齐 | 只支持顶端对齐 |
| 字体样式 | 粗体/斜体/下划线/删除线都是**合成**的（见 `QL_UI_FONT_*`）；无描边/阴影 |
| 排版 | 无字距调整（kerning）、无连字 |
| 矢量字体 | ✅ M2 已支持（FreeType，按需光栅化 + 缓存） |
| 字重/斜体 | 需要不同字体文件（`FT_Face`），无合成加粗 |
| 字形淘汰 | 无 LRU，达到上限后拒绝新字形 |

---

## 11. 示例与测试

| 文件 | 内容 |
|---|---|
| `test/text/example_text.c` | 文本样张：两种字号、三种对齐、多行、缺字兜底，输出 PPM |
| `test/text/test_font.c` | 字形查找/缺字/灰度/UTF-8/度量（191 项断言） |
| `test/text/test_str.c` | 两种容量策略与所有权（150 项断言） |
| `test/text/test_label.c` | 所有权、自动尺寸、对齐的像素级断言 |

```bash
./build/example_text /tmp/text.ppm
./build/example_window            # 窗口里能看到标题/按钮/状态文字
ctest --test-dir build --output-on-failure
```
