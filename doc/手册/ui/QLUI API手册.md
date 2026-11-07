# QLUI / HTML+CSS 编译 API 手册（v0.1）

配套阅读：[QLUI 阅读指南](./QLUI阅读指南.md) · 定位：[Web 兼容设计](../../Web兼容设计.md) §7

> **先明确适用场景**
> - **MCU**：不要用本模块，直接用 `QLWidget`/`QLLabel` 的 C API（见 [QLWidget API手册](../widget/QLWidget%20API手册.md)）。
> - **MPU / 嵌入式 Linux**：用本模块，把 HTML+CSS 编译成 C。
> - **PC 要高兼容**：用系统 WebView（WebView2 / WKWebView / WebKitGTK），不用本模块。

---

## 1. 工作流

```
index.html ┐
index.css  ├─ python3 tools/qlui/qlui.py ─▶ ui_index.c + ui_index.h ─▶ 编译进固件
tools/qlui/qlui.py ┘
```

### 1.1 命令行

```bash
python3 tools/qlui/qlui.py \
    --html test/web/ui_demo.html \
    --css  test/web/ui_demo.css \
    --out  build/ui
# 输出：build/ui/ui_demo.c  build/ui/ui_demo.h
```

| 参数 | 说明 |
|---|---|
| `--html` | 输入 HTML（必填） |
| `--css` | 输入 CSS（必填） |
| `--out` | 输出目录（必填，不存在会创建） |
| `--name` | 输出标识（默认取 HTML 文件名） |

退出码：`0` 成功；`1` 有错误（错误信息带 `文件:行号`）。

### 1.2 CMake 集成

```cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
add_custom_command(
    OUTPUT  ${UI_OUT}/ui_index.c ${UI_OUT}/ui_index.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/qlui/qlui.py
            --html ui/index.html --css ui/index.css --out ${UI_OUT}
    DEPENDS ui/index.html ui/index.css ${CMAKE_SOURCE_DIR}/tools/qlui/qlui.py
    COMMENT "编译 HTML+CSS 为 C")

add_executable(app main.c ${UI_OUT}/ui_index.c)
target_link_libraries(app qlgl)
target_include_directories(app PRIVATE ${UI_OUT})
```

改 HTML/CSS 后 `cmake --build` 会自动重新生成并重编译。

---

## 2. 支持的 HTML

| 标签 | UA 默认样式 |
|---|---|
| `div` / `section` | `display:block` |
| `p` | `display:block`，`margin: 8px 0` |
| `span` / `b` / `i` | `display:inline` |
| `button` | `display:inline-block`，带内边距/边框/背景 |
| `h1` / `h2` | `display:block`，`font-size:16px` |
| `h3` | `display:block`，`font-size:12px` |

**限制**：
- 文档必须**有且只有一个根元素**；
- 元素不能同时包含文本和子元素（混合内容）→ 报错，请把文本包进 `<span>`；
- 其他标签（`img`/`table`/`input`/`ul`…）→ 报错。

---

## 3. 支持的 CSS

### 3.1 选择器

| 选择器 | 支持 |
|---|---|
| `tag` / `.class` / `#id` | ✅ |
| `A B`（后代） | ✅ |
| `A > B`（子代） | ✅ |
| `A + B` / `A ~ B`（兄弟） | ❌ 报错 |
| `[attr]` / `::before` | ❌ 报错 |
| `:hover` / `:active` / `:focus` | ✅ 只允许单个，编译成状态变体 |

优先级按规范三元组 `(id, class, tag)` 计算，同级按出现顺序。

### 3.2 属性

| 属性 | 值 | 说明 |
|---|---|---|
| `display` | `block` `inline` `inline-block` `none` | |
| `width` / `height` | `auto` `Npx` `N%` | `%` 相对父**内容宽度**；`height` 的 `%` 不支持 |
| `margin` / `padding` | `Npx`（1/2/4 值） | 也支持 `margin-top` 等长写 |
| `border-width` | `Npx` | |
| `border-color` | 颜色 | |
| `border` | `1px solid #333` | 只支持 `solid` |
| `border-radius` | `Npx` | |
| `background` / `background-color` | 颜色 | |
| `color` | 颜色 | |
| `font-size` | `Npx` | `>= 14px` 用 16 号内置字体，否则 12 号 |
| `text-align` | `left` `center` `right` | |
| `box-sizing` | `content-box` `border-box` | 默认 `content-box`（与浏览器一致） |

### 3.3 颜色写法

`#rgb`、`#rrggbb`、`rgb(r,g,b)`、命名色（`black`/`white`/`red`/`green`/`lime`/`blue`/`gray`/`silver`/`yellow`/`orange`/`purple`/`navy`/`teal`/`transparent`）。

### 3.4 不支持的写法一律报错

```
position / float / flex / grid / z-index / overflow / opacity / box-shadow
transition / animation / var() / @media / em / rem / vh / vw / calc()
```

报错示例：

```
qlui: 错误：index.css:27: 不支持的属性 'position'
qlui: 错误：index.css:12: 不支持的长度 '1.5em'（只支持 px 与 %）
qlui: 错误：index.html:5: <p> 同时包含文本和子元素（混合内容）——v0 不支持，请把文本放进独立的 <span>/<p>
```

---

## 4. 生成的数据结构

```c
typedef enum {
    QL_UI_TAG_DIV, QL_UI_TAG_SECTION, QL_UI_TAG_P, QL_UI_TAG_SPAN,
    QL_UI_TAG_B, QL_UI_TAG_I, QL_UI_TAG_BUTTON,
    QL_UI_TAG_H1, QL_UI_TAG_H2, QL_UI_TAG_H3, QL_UI_TAG_COUNT
} QLUITag;

typedef enum { QL_UI_DISPLAY_BLOCK, QL_UI_DISPLAY_INLINE,
               QL_UI_DISPLAY_INLINE_BLOCK, QL_UI_DISPLAY_NONE } QLUIDisplay;

typedef enum { QL_UI_UNIT_AUTO, QL_UI_UNIT_PX, QL_UI_UNIT_PERCENT } QLUIUnit;

typedef struct { int16_t top, right, bottom, left; } QLUIEdges;
typedef struct { uint16_t value; uint8_t unit; uint8_t reserved; } QLUILength;

typedef struct {
    uint32_t bg_color, text_color, border_color;
    QLUILength width, height;
    QLUIEdges margin, padding;
    uint8_t display, bg_alpha, border_width, border_radius;
    uint8_t text_align, font_id, box_sizing, reserved;
} QLUIStyle;

typedef struct {
    uint16_t tag, parent, first_child, next_sibling, style, text;
} QLUINode;

typedef struct {
    const char *name;
    uint16_t node_count, text_count;
    const QLUINode *nodes;
    const QLUIStyle *styles;
    const char *text_pool;
    const uint16_t *text_offsets;
} QLUIDoc;
```

`0xFFFF` 表示"无"（无父/无子/无兄弟/无文本）。

---

## 5. 运行期 API

```c
QLUIRoot *ql_ui_build(const QLUIDoc *doc);
void      ql_ui_free(QLUIRoot *ui);

void      ql_ui_layout(QLUIRoot *ui, ql_coord_t width, ql_coord_t height);
void      ql_ui_render(QLUIRoot *ui, QLCanvas *canvas);
```

| 函数 | 说明 |
|---|---|
| `ql_ui_build` | 把静态表物化成节点树；一次分配，失败返回 `NULL` |
| `ql_ui_free` | 释放（不负责显示驱动） |
| `ql_ui_layout` | 按视口尺寸做完整布局，结果写入每个节点的 `border_box`/`content_box` |
| `ql_ui_render` | 按树顺序绘制：背景 → 边框 → 文本 → 子节点 |

### 查询

```c
QLUINodeRT *ql_ui_root_node(QLUIRoot *ui);
QLUINodeRT *ql_ui_find_by_tag(QLUIRoot *ui, QLUITag tag, uint16_t nth);
QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
const char *ql_ui_node_text(const QLUINodeRT *node);
const QLFont *ql_ui_node_font(const QLUINodeRT *node);
QLRect      ql_ui_node_rect(const QLUINodeRT *node);
const char *ql_ui_tag_name(QLUITag tag);
```

`ql_ui_hit_test` 从树末尾向前找第一个包含该点的节点（后面的节点视觉上更靠上）。

### 运行期改内容

```c
ql_err_t ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text);
```

只允许改"没有子节点的元素"的文本。改完需要重新 `ql_ui_layout` + `ql_ui_render`。

> **不支持**运行期改样式类、增删节点。页面结构在构建期固定。

### 交互

```c
typedef void (*ql_ui_click_cb)(QLUIRoot *ui, QLUINodeRT *node, void *user_data);

void ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data);
int  ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int  ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int  ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
```

| 函数 | 返回 | 说明 |
|---|---|---|
| `pointer_down` | 1 = 命中 | 给命中节点置 `QL_UI_RT_ACTIVE` |
| `pointer_up` | 1 = 触发点击 | 清 `ACTIVE`；若按下时命中同一节点则调 `click_cb` |
| `pointer_move` | 1 = 悬停状态有变化 | 更新 `QL_UI_RT_HOVER` |

节点状态位：`QL_UI_RT_HOVER` / `QL_UI_RT_ACTIVE` / `QL_UI_RT_DIRTY`。

> 注意：状态位**会影响绘制**（`:hover`/`:active`/`:focus` 有独立样式），但**不影响布局**（M1 限制），
> 需要高亮请在 `click` 回调或渲染后自行处理。

---

## 6. 完整示例

```c
#include <qlgl/qlgl.h>
#include "ui_index.h"          /* extern const QLUIDoc ui_index_doc; */

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    printf("clicked: %s\n", ql_ui_node_text(node));
}

int main(void)
{
    qlgl_init();
    QLDispDriver *d = ql_disp_x11_create("QLGL", 320, 240, QL_COLOR_FORMAT_AUTO);

    QLUIRoot *ui = ql_ui_build(&ui_index_doc);
    ql_ui_layout(ui, 320, 240);
    ql_ui_set_click_cb(ui, on_click, NULL);

    QLCanvas canvas;
    ql_canvas_from_disp(&canvas, d);

    int running = 1;
    while (running) {
        ql_ui_pointer_move(ui, x, y);      /* 你的输入层把坐标传进来 */
        ql_ui_render(ui, &canvas);
        disp_flush_all();                  /* 或接渲染器的脏矩形 */
        running = ui_poll_events();
    }

    ql_ui_free(ui);
    ql_disp_x11_destroy(d);
    qlgl_shutdown();
}
```

---

## 7. 限制一览

| 项 | 现状 |
|---|---|
| 布局 | block / inline / inline-block；**无 flex / grid / position** |
| 高度 | 不支持 `%`，不支持 `min/max-height` |
| 文本 | 不支持混合内容、不自动换行、无字距/行距调整 |
| 伪类 | `:hover`/`:active`/`:focus` 编译成变体样式；布局仍按基础样式 |
| 运行期 | 只能改文本，不能改结构/样式类 |
| JS | 不支持（交互用 C 回调） |
| 性能 | 一次 `layout` 遍历整棵树；节点数 ≤ 数百时足够 |

需要更高兼容度 → 用 PC + WebView（[设计文档 §7.3](../../Web兼容设计.md)）。
