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
| `display` | `block` `inline` `inline-block` `none` `flex` `inline-flex` | `flex` 见 §3.8 |
| `width` / `height` | `auto` `Npx` `N%` | `%` 相对父**内容宽度**；`height` 的 `%` 不支持 |
| `margin` / `padding` | `Npx`（1/2/4 值） | 也支持 `margin-top` 等长写 |
| `border-width` | `Npx` | |
| `border-color` | 颜色 | |
| `border` | `1px solid #333` | 只支持 `solid` |
| `border-radius` | `Npx` | |
| `background` / `background-color` | 颜色，或 `linear-gradient(...)`（见 §3.5） | |
| `box-shadow` | `<dx> <dy> [blur] <color>` | 软阴影，SDF 实现，单趟。`none` 关闭 |
| `color` | 颜色 | |
| `font-size` | `Npx` | 任意整数；生成期汇总成 `font_sizes` 表，运行期按字号取 `ql_font_default(N)`（配置了矢量字体就是该字号的动态字体，支持中文；否则回落到内置 12/16 位图字体） |
| `text-align` | `left` `center` `right` | |
| `box-sizing` | `content-box` `border-box` | 默认 `content-box`（与浏览器一致） |
| `flex` / `flex-grow` / `flex-shrink` / `flex-basis` | `1` / `1 0 auto` / `0 0 100px` | 见 §3.8 |
| `flex-direction` | `row` `row-reverse` `column` `column-reverse` | |
| `flex-wrap` | `nowrap` `wrap` | |
| `justify-content` | `flex-start` `center` `flex-end` `space-between` `space-around` `space-evenly` | |
| `align-items` | `stretch` `flex-start` `center` `flex-end` | |
| `gap` / `row-gap` / `column-gap` | `Npx`（1~2 值） | 不支持百分比 |
| `position` | `static` `relative` `absolute` `fixed` | 见 §3.9 |
| `top` / `right` / `bottom` / `left` | `Npx` `N%` `auto` | 仅非 static 时生效 |
| `z-index` | 整数（int16） | 同级兄弟间排序 |
| `overflow` | `visible` `hidden` `auto`（`scroll` 视同 `auto`） | `hidden`/`auto` 都裁掉溢出 |

### 3.3 颜色写法

`#rgb`、`#rrggbb`、`rgb(r,g,b)`、`rgba(r,g,b,a)`、命名色（`black`/`white`/`red`/`green`/`lime`/`blue`/`gray`/`silver`/`yellow`/`orange`/`purple`/`navy`/`teal`/`transparent`）。

### 3.4 混合内容

支持 `<p>`/`<div>` 里文本与行内元素混排：

```html
<p class="m">Hello <b>bold</b> and <i>it</i>!</p>
```

**在生成期消解**：每个文本段被包成一个匿名 `<span>` 子节点，
运行期只认识"元素树"，因此布局与绘制代码完全不需要额外分支。

- 文本段的空白按 HTML 规则折叠（连续空白变单个空格）；
- 元素之间的**纯空白**不产生盒子（否则每个换行缩进都会变成一个 4px 宽的 span）；
- 匿名 span 继承父元素的 `color` / `font-size` / `text-align`；
- 相邻文本段不会自动合并，写 CSS 时按"每个文本段是一个 inline 盒子"来理解。

### 3.5 渐变

```css
background: linear-gradient(to right, #3399ff, #1a4f8a);
background: linear-gradient(to bottom, #4ad08a, #2a9a5c);
```

| 项 | 支持 |
|---|---|
| 方向 | `to bottom` / `to top` / `180deg`、`to right` / `to left` / `90deg` |
| 颜色停靠点 | 只支持 **2 个**（起始 + 终止） |
| 多停靠点 / 角度其他值 / `radial-gradient` | ❌ 构建期报错 |

实现：垂直渐变走**行填充快路径**（整行同色，成本≈纯色填充）；水平渐变逐像素插值，慢一个量级。
GPU 侧用**顶点颜色插值**实现，不增加顶点、不增加 draw call。

> ⚠️ **状态样式与渐变**：`:hover` / `:active` 等变体样式是"整套重算"的，
> 如果变体规则只写了 `background-color`（没有 `background`），渐变会**退回纯色**。
> 想让渐变按钮在 hover/active 下保持渐变，要显式写：
>
> ```css
> .btn:hover { background: linear-gradient(to bottom, #58e09a, #35a869); }
> ```

### 3.6 阴影

```css
box-shadow: 0 4px 12px rgba(0, 0, 0, 0.55);
box-shadow: 0 1px 0 #000;          /* 硬阴影（无模糊） */
box-shadow: none;
```

| 项 | 支持 |
|---|---|
| `dx dy` | 必须（`px`） |
| `blur` | 可选，`0` = 硬边 |
| 颜色 | 必须（不支持浏览器默认色） |
| `spread`（第 4 个长度）、`inset`、多重阴影 | ❌ 构建期报错 |

实现是 **SDF + smoothstep 衰减**，与 GPU 着色器同一公式，因此两条后端视觉一致。
只有"距轮廓 blur 以内"的环需要逐像素算距离，内部实心区走行填充——
所以在 PC 上几乎无感，在 MCU 上也可用（见开发规划 M5.5 的能力矩阵）。

### 3.7 Flexbox

```css
.row { display: flex; flex-direction: row; gap: 8px; align-items: center; }
.card { flex: 1 1 0px; }             /* 等分 */
.btn  { flex: 1 1 auto; }            /* 按内容比例分 */
```

**支持的语义**

| 项 | 说明 |
|---|---|
| 主轴方向 | row / row-reverse / column / column-reverse |
| 换行 | nowrap / wrap（按主轴尺寸断行） |
| 剩余空间 | 正：按 `flex-grow` 比例分配；负：按 `flex-shrink × basis` 加权收缩 |
| 主轴对齐 | justify-content 六种（`grow` 生效时 justify 不再参与分配） |
| 交叉轴对齐 | `stretch`（默认，子项无显式交叉尺寸时拉满）或 start/center/end |
| gap | `gap` / `row-gap` / `column-gap`，px |
| 子项上限 | 单个 flex 容器 ≤ `QL_CFG_UI_FLEX_MAX_ITEMS`（默认 256），超了降级为 block 流并告警 |

**实现要点（影响你写 CSS 时的预期）**

1. **`flex-direction: column` + `height: auto` 时没有剩余空间可分**，
   `flex-grow` 不生效 —— 与浏览器一致（grow 需要确定的容器尺寸）；
2. 布局是"**先测量、再分配、最后定位**"：容器先让每个子项按自然尺寸排一遍，
   再根据 grow/shrink 改写尺寸重排。所以**子项的自然尺寸必须能算出来**
   （`width: auto` 的块级子项会按容器宽度算，这是 CSS 的行为）；
3. `flex-basis` 支持 px 与 `%`（相对容器主轴尺寸）；
4. 百分比 `width` 在 flex 子项上是相对**容器内容宽**，不是 flex-basis。

**不支持的**：`align-self`、`align-content`、`order`、`flex-flow` 简写、
`flex-basis: auto` 的完整语义（等同"用 width"）、多行 `align-content` 对齐。

### 3.8 定位与 z-index

```css
.card  { position: relative; }                    /* 成为绝对子项的包含块 */
.badge { position: absolute; top: -6px; right: 8px; }
.toast { position: fixed; bottom: 5px; left: 5px; }
```

| `position` | 行为 |
|---|---|
| `static` | 常规流 |
| `relative` | 常规流占位不变，**绘制位置**按 `top/left`（或 `right/bottom` 取负）偏移，不影响兄弟 |
| `absolute` | 脱离文档流；相对**最近的非 static 祖先的 padding box** 定位；没有则相对根 |
| `fixed` | 脱离文档流；相对**视口**定位 |

**绝对定位的尺寸规则**

| 给了什么 | 宽度 |
|---|---|
| 显式 `width` | 用它 |
| `left` + `right`（无 width） | 拉伸填满 |
| 只有 `right`（无 width） | 从右边界往左放 |
| 都没有 | **shrink-to-fit**：只支持"内容是本节点自己的文本"（badge/标签场景）；有块级子项时撑满包含块 |

高度同理（`top` + `bottom` 拉伸，`bottom` 反推 y）。

> ⚠ 已知简化：`position: absolute` 在 CSS 里还有 "static position"（没给 inset 时
> 停在原来该在的位置）与 shrink-to-fit 的完整算法。这里都没实现，
> 没给 inset 时贴包含块左上角。涉及这些的布局请显式写 inset。

**z-index**：布局结束后对所有兄弟按 `z-index` **稳定排序**子链表，
之后**绘制顺序与命中测试顺序自动一致**（链表顺序即绘制顺序）。
z-index 相同则保持文档顺序（稳定）。

**overflow**

| 值 | 行为 |
|---|---|
| `visible`（默认） | 子项可以画到父盒子外 |
| `hidden` / `auto` / `scroll` | 子树被裁到本节点边框盒；**被裁掉的部分不可命中**（命中测试同样裁剪） |

> `auto`/`scroll` 目前**不产生滚动条，也不能滚动** —— 只是裁剪。
> 滚动容器排在 M3 之后的滚动里程碑。

### 3.9 不支持的写法一律报错

```
float / grid / opacity / align-self / align-content / order / flex-flow
transform / transition / animation / filter / clip-path / var()
min-width / max-width / min-height / max-height
@media / em / rem / vh / vw / calc()
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
    uint16_t node_count, style_count, text_count, font_size_count;
    const QLUINode   *nodes;
    const QLUIStyle  *styles;
    const char       *text_pool;      /* 以 '\0' 分隔的 UTF-8 池 */
    const uint16_t   *text_offsets;
    const uint16_t   *font_sizes;     /* font_id 索引它，值是像素字号 */
    void             *arena;          /* NULL = 静态数据，不归 doc 管 */
} QLUIDoc;
```

`0xFFFF` 表示"无"（无父/无子/无兄弟/无文本）。

`arena` 是**唯一的所有权判据**：

- `arena == NULL` → 静态 doc（`.rodata` 或调用方自己管内存），`ql_ui_doc_free` 是空操作；
- `arena != NULL` → `ql_ui_doc_load*` 分配，`ql_ui_doc_free` 释放。

因为这条判据，静态 doc 误进 free 路径不可能发生，`ql_ui_doc_free` 可以无脑调用。

> 所有内部引用都是 `uint16` **下标**而不是指针 —— 这既让 blob 的加载变成
> "设 6 个基址指针"（零重定位），也让 doc 之间可以各自独立（不能跨 doc 引用）。

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

## 6.5 `.qlui` blob API（换界面不用重编译）

头文件 `<qlgl/qlui.h>`。想省字节量的目标机可以不编译 `src/ui/qlui_blob.c`，
下面的函数就不存在（声明不会产生引用，链接器不会拖入）。

### 6.5.1 最常用：一行加载

```c
QLUIRoot *ui = ql_ui_load_file("ui/index.qlui");
if (ui == NULL) {
    fprintf(stderr, "load failed: %s\n", ql_ui_doc_last_error());
    return 1;
}
ql_ui_layout(ui, 320, 240);
/* ... 渲染 / 交互 ... */
ql_ui_free(ui);          /* doc 由 root 持有，会一起释放 */
```

`ql_ui_load_file` = `ql_ui_doc_load()` + `ql_ui_build()`，并让 root **持有** doc。

### 6.5.2 想自己管 doc

```c
QLUIDoc *doc = ql_ui_doc_load("ui/index.qlui");
QLUIRoot *ui = ql_ui_build(doc);
...
ql_ui_free(ui);          /* 不释放 doc（root 不持有它） */
ql_ui_doc_free(doc);
```

### 6.5.3 API 一览

```c
/* 构建期（宿主工具用） */
size_t   ql_ui_doc_encode(const QLUIDoc *doc, void *buf, size_t cap);
ql_err_t ql_ui_doc_save(const QLUIDoc *doc, const char *path);

/* 运行期 */
QLUIDoc *ql_ui_doc_load_mem(const void *data, size_t size);
QLUIDoc *ql_ui_doc_load(const char *path);
void     ql_ui_doc_free(QLUIDoc *doc);
QLUIRoot *ql_ui_load_file(const char *path);

/* 辅助 */
int         ql_ui_doc_is_owned(const QLUIDoc *doc);   /* arena != NULL */
size_t      ql_ui_doc_text_pool_size(const QLUIDoc *doc);
const char *ql_ui_doc_last_error(void);
```

| 函数 | 说明 |
|---|---|
| `ql_ui_doc_encode(doc, NULL, 0)` | 只算需要多少字节（返回 0 = doc 非法） |
| `ql_ui_doc_encode(doc, buf, cap)` | 写入；`cap` 不足时不写、只返回需求 |
| `ql_ui_doc_load_mem` | **拷贝**一份数据进来，调用方可以立刻释放自己的缓冲 |
| `ql_ui_doc_free` | 对**静态 doc（`arena == NULL`）是空操作**，可以无脑调用 |
| `ql_ui_load_file` | 失败返回 NULL 并打错误日志，原因也可用 `ql_ui_doc_last_error()` |

### 6.5.4 失败原因

加载失败**一律返回 NULL**，绝不"跳过坏数据继续画"。原因用
`ql_ui_doc_last_error()` 取（静态缓冲，非线程安全）：

| 信息 | 原因 |
|---|---|
| `bad magic (not a .qlui blob)` | 不是 blob 文件 |
| `version mismatch: blob built by a different qlgl` | 库版本变了，重新生成 |
| `QLUINode layout mismatch (blob built on another ABI)` | 结构体布局变了（换编译器/架构/改了结构体） |
| `size mismatch (file truncated or corrupted)` | 文件被截断 |
| `checksum mismatch (file corrupted)` | 内容被改过 |
| `section offsets inconsistent with counts` | 头部被手工篡改 |
| `too many nodes` / `text pool too large` | 超过 `QL_CFG_UI_MAX_*` 上限 |

### 6.5.5 容量上限

```c
#define QL_CFG_UI_MAX_NODES         4096
#define QL_CFG_UI_MAX_STYLES        4096
#define QL_CFG_UI_MAX_TEXT_BYTES    (64u * 1024u)
#define QL_CFG_UI_MAX_FONT_SIZES    256
```

只作用于 blob（外来数据）。静态编译的 doc 是编译器保证的常量，不受限制。
MCU 上按实际页面调小，例如 256 / 256 / 8K。

> ⚠ `text_offsets` 是 `uint16`，所以**单个 doc 的文本池天然上限 64KB**
> （中文约 2 万字）。超了会加载失败，不会静默截断。

### 6.5.6 与静态 C 数据混用

```c
/* 主壳：构建期固化，零加载成本 */
QLUIRoot *shell = ql_ui_build(&ui_shell_doc);

/* 侧栏/插件：运行时加载，可换皮 */
QLUIRoot *panel = ql_ui_load_file("themes/dark/sidebar.qlui");

ql_ui_layout(shell, 320, 480);
ql_ui_layout(panel, 120, 480);
/* 各自 render 到自己的区域 */
```

混用单位是**面板**。`parent/first_child/next_sibling` 是同一数组内的 `uint16` 下标，
**doc 之间不能互相引用合成一棵树**；跨面板的 CSS 继承也不存在（每个面板独立闭合）。
需要"某块热改"就重建那个 `QLUIRoot`（几十微秒），不要尝试做元素级混合。

---

## 7. 限制一览

| 项 | 现状 |
|---|---|
| 布局 | block / inline / inline-block；**无 flex / grid / position** |
| 高度 | 不支持 `%`，不支持 `min/max-height` |
| 文本 | 不支持混合内容、不自动换行、无字距/行距调整 |
| 伪类 | `:hover`/`:active`/`:focus` 编译成变体样式；布局仍按基础样式 |
| 运行期 | 只能改文本，不能改结构/样式类（要改结构就换一份 doc） |
| 换 UI | 支持 `.qlui` blob，**不用重编译**；但不支持目标机上解析 HTML/CSS 文本（M3.5） |
| JS | 不支持（交互用 C 回调） |
| 性能 | 一次 `layout` 遍历整棵树；节点数 ≤ 数百时足够 |

需要更高兼容度 → 用 PC + WebView（[设计文档 §7.3](../../Web兼容设计.md)）。
