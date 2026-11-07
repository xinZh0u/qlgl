# QLUI / HTML+CSS 编译 阅读指南

> 本部分的定位见 [Web 兼容设计](../../Web兼容设计.md) §7：
> **MCU 不追求 Web 兼容（纯 C API）**；HTML/CSS 编译成 C 是给 **MPU/嵌入式 Linux** 用的中间档；
> **PC 要高兼容请直接用系统 WebView**。
>
> 本部分实现的是"构建期把 HTML+CSS 翻译成静态 C 数据"，运行时零解析。
> 目标读者：要改 `tools/qlui/` 或布局算法的人。只想用，请看 [QLUI API手册](./QLUI%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 阅读顺序 |
|---|---|---|
| `tools/qlui/qlui.py` | HTML 解析 + CSS 解析 + 选择器匹配 + 级联 + 生成 C | ① |
| `include/qlgl/qlui.h` | 生成数据格式（`QLUIDoc`/`QLUINode`/`QLUIStyle`）与运行期 API | ② |
| `src/ui/qlui.c` | 物化、布局、绘制、命中测试 | ③ |
| `src/ui/qlui_blob.c` | `.qlui` 二进制编解码（**独立编译单元，可摘**） | ④ |
| `tools/qlblob/qlblob.c` | 宿主工具：把生成的 doc 导出成 `.qlui` | ⑤ |
| `test/web/ui_demo.html` / `.css` | 示例页面（构建期被编译） | ⑥ |
| `test/web/test_web_ui.c` | 布局/绘制/交互断言 | ⑦ |
| `test/web/test_ui_blob.c` | blob 往返 / 损坏检测 / 与构建产物逐字节一致 | ⑧ |
| `test/web/example_ui_blob.c` | 从 `.qlui` 加载 + 热重载示例 | ⑨ |

生成链路：

```
test/web/ui_demo.html ┐
test/web/ui_demo.css  ├─ cmake --build ─▶ build/ui/ui_demo.c + ui_demo.h ─▶ 编译进测试程序
tools/qlui/qlui.py    ┘                                     │
                                                            └─ qlblob ─▶ ui_demo.qlui
```

`CMakeLists.txt` 里用 `add_custom_command` 声明依赖，改 HTML/CSS 会自动重新生成（与 Qt 的 `.ui` 体验一致）。

---

## 2. 为什么"编译成 C"而不是"运行时解析"

| | 运行时解析 | 编译成 C |
|---|---|---|
| 代码量 | 需要完整 CSS 引擎（1 万行+） | 运行时只有布局与绘制（约 500 行） |
| 启动开销 | 解析 + 选择器匹配 + 级联 | 0（数据已在 `.rodata`） |
| RAM | 需要保存解析中间态 | 只有计算后的布局框 |
| 错误发现时机 | 运行时（可能上板才炸） | **编译期 / 构建期** |
| 代价 | — | 运行期不能改 CSS |

这正是 Qt `uic` 的思路，也是 `qlgen`（类型 id）、`qlfontgen`（字体）已经在用的思路。

**但"运行期不能改 CSS"并不意味着"换界面必须重编译"。** 见 §5.5 的 `.qlui` blob。

---

## 2.5 三种形态，一份 IR

产出的都是同一个 `QLUIDoc`，区别只在"字节在哪、什么时候解析"：

| | ① 静态 C 数据 | ② `.qlui` blob | ③ 运行时文本（M3.5） |
|---|---|---|---|
| HTML/CSS 何时转 | 构建机 | 构建机 | **目标机运行时** |
| 入口 | `ql_ui_build(&ui_x_doc)` | `ql_ui_load_file("x.qlui")` | `ql_ui_load_text()`（未实现） |
| 加载成本 | **0** | ~10µs（校验 + 设 6 个基址） | 50~200µs |
| 渲染成本 | **相同** | **相同** | **相同** |
| 换 UI 要重编译 | 要 | **不要** | 不要 |
| 目标机可改 HTML | 不能 | 不能 | **能** |
| MCU | ✅ | 可选（摘掉 `qlui_blob.c` 即可） | ❌ |

三者渲染开销完全一致，因为都走同一套 `QLUINodeRT → 布局 → 绘制`。
blob 省掉的只是"解析文本"，**不提升帧率**——它在乎的是启动时间和"改 UI 不重编译"。

混用的单位是**面板**（一个 `QLUIRoot` ↔ 一份 doc）：`parent/first_child/next_sibling`
是同一数组内的 `uint16` 下标，**跨 doc 引用物理上不存在**，所以不做元素级混合。
需要"某块热改"就重建那个 `QLUIRoot`（几十微秒）。

---

## 3. 生成的数据长什么样

```c
typedef struct {
    uint16_t tag;          /* QL_UI_TAG_DIV / _P / _SPAN / _BUTTON ... */
    uint16_t parent;       /* 0xFFFF = 根 */
    uint16_t first_child;  /* 0xFFFF = 无 */
    uint16_t next_sibling; /* 0xFFFF = 无 */
    uint16_t style;        /* styles[] 下标 */
    uint16_t text;         /* 文本池下标，0xFFFF = 无 */
} QLUINode;

typedef struct {
    uint32_t   bg_color, text_color, border_color;
    QLUILength width, height;      /* { 值, auto/px/% } */
    QLUIEdges  margin, padding;    /* 上右下左 */
    uint8_t    display, bg_alpha, border_width, border_radius;
    uint8_t    text_align, font_id, box_sizing;
} QLUIStyle;
```

关键不变式（生成器保证）：

1. **`nodes[]` 是深度优先顺序**，`parent` 下标一定小于子节点下标 → 物化时一趟 for 循环即可，不需要递归；
2. **样式表已去重**（`add_style` 用内容做 key），相同样式只存一份；
3. **文本池按 `'\0'` 分隔**，`text_offsets[]` 是每段的首字节偏移；
4. **颜色/长度在生成期全部换算成整数**，运行期不做字符串处理。

---

## 4. 生成器（`tools/qlui/qlui.py`）

### 4.1 处理流程

```
HTML ──HTMLParser──▶ Node 树 ─┐
                              ├─▶ 逐节点计算样式 ─▶ 去重 ─▶ 生成 C
CSS  ──parse_css──▶ Rule 列表 ─┘
```

### 4.2 CSS 解析

- 先 `strip_comments` 去掉 `/* */`；
- 逐条 `选择器 { 声明; }`；
- **`@media` / `@supports` 等 at-rule 直接报错**；
- 属性白名单 `SUPPORTED_PROPS`，不在表里就报错并给出 `文件:行号`。

### 4.3 选择器与优先级

支持：`tag`、`.class`、`#id`、后代（空格）、子代（`>`）。
**不支持 `+` / `~` 兄弟选择器**（会报错）。

优先级用规范的三元组 `(id 数, class 数, tag 数)`，排序后再按出现顺序覆盖 ——
这是"同一份 CSS 在两个引擎上渲染一致"的前提，不是可选项。

```python
matched.sort(key=lambda r: (r.specificity, r.order))
```

### 4.4 级联与继承

```
UA 默认样式（UA_DEFAULTS）
  ↓ 被匹配到的规则覆盖（按优先级）
  ↓ 被行内 style 覆盖
  ↓ 继承（只有 color / font-size / text-align）
  ↓ font-size 在生成期汇总成 font_sizes 表，运行期按字号取字体
```

继承的实现很直接：父节点先算完样式，子节点从父的样式里挑 `INHERITED` 三个属性作为初始值。

### 4.5 明确报错的场景（不静默降级）

| 场景 | 报错信息 |
|---|---|
| 不支持的标签（`<table>`/`<img>`） | `不支持标签 <table>` |
| 不支持的属性（`position`/`flex`/`box-shadow`） | `不支持的属性 'position'` |
| 不支持的长度单位（`em`/`rem`/`vh`） | `不支持的长度 '1.5em'（只支持 px 与 %）` |
| 不支持的 at-rule | `不支持 at-rule '@media'` |
| 兄弟选择器 | `不支持相邻/兄弟选择器 '+'` |
| **混合内容**（`<p>文字<b>粗</b></p>`） | `同时包含文本和子元素（混合内容）——请把文本放进独立的 <span>/<p>` |
| 文档有 0 或 2+ 个根元素 | `文档必须有且只有一个根元素` |

最后一条很容易被忽略：v0 的文本是"元素的一段文本"，不支持一个元素里
既有文字又有子元素。所以演示页面里写的是：

```html
<p class="line"><span>inline</span><span>bold</span></p>   <!-- ✅ -->
<p class="line">inline <b>bold</b></p>                      <!-- ❌ 报错 -->
```

---

## 5. 运行期三件事

### 5.1 物化 `ql_ui_build`

```c
for (i = 0..node_count) {
    rt->src   = &doc->nodes[i];
    rt->style = &doc->styles[src->style];
    rt->text  = doc->text_pool + doc->text_offsets[src->text];
    /* 挂到父节点末尾（生成顺序 = 深度优先，父一定先于子） */
}
```

一次 `ql_mem_calloc` 分配整个节点数组，之后**零 malloc**。

### 5.2 布局 `ql_ui_layout`

单趟递归，不需要两趟（因为不支持 `height: %`，也没有 flex）：

```
layout_node(node, x, y, avail_w):
    display:none → 空矩形，返回 0
    border_w = 宽度解析（px / % / inline 按文本量 / 其余撑满）
    box 位置 = (x + margin_left, y + margin_top)
    content 位置 = box + border + padding
    if 有子节点:
        block 子节点纵向堆叠；
        inline/inline-block 子节点横向排布，超出宽度换行
        content_h = 游标推进量
    else if 有文本:
        content_h = 行高
    if 显式 height: content_h = max(content_h, 显式值)
    记录 border_box / content_box
    return border_h + margin
```

要点：

- **宽度先算**：`%` 相对父**内容宽**，`auto` 时 block 撑满、inline 按文本量；
- **`box-sizing: border-box`** 时 `width` 指边框盒（浏览器默认是 `content-box`，
  所以 `width:100%` + `padding` 会溢出 —— 这是对的，别以为是 bug）；
- **高度**：没有百分比高度，所以一趟就够；
- **溢出**：内容高于显式高度时不裁剪（等于 CSS 的 `overflow: visible`）。

### 5.3 绘制 `ql_ui_render`

按树顺序（父先子后）：背景 → 边框 → 文本 → 递归子节点。
全部复用已有的 `QLCanvas` 原语（`fill_round_rect_alpha` / `stroke_rect` / `draw_text`）。

字体由 `font-size` 决定：生成期把用到的字号汇总成 `font_sizes[]`，
运行期 `ql_font_default(N)` 按字号取字体（配了矢量字体就是动态字体，支持中文/任意字号）。

### 5.5 `.qlui` blob：换界面不用重编译

```
qlui.py ──▶ ui_demo.c ──┐
                        ├─ 编译进程序（零加载成本）
        └──▶ qlblob ────┴─▶ ui_demo.qlui ──▶ ql_ui_load_file()（运行时加载）
```

#### 为什么加载这么便宜

`QLUIDoc` 里**所有内部引用都是 `uint16` 下标**，唯一的指针是 doc 自己的 6 个基址字段：

```c
QLUIDoc {
    nodes / styles / text_pool / text_offsets / font_sizes / name   ← 只有这 6 个指针
    node_count / style_count / text_count / font_size_count
    arena        ← NULL = 静态数据，不归我管
}
```

所以"解码"= 校验 + 设 6 个指针，**没有任何递归重定位**。这是当初"用下标不用指针"
的一个意外红利。

#### 格式

```
[ QLUIDocBlobHeader 64 字节 ]
[ nodes      ] node_count      * 18 字节
[ styles     ] style_count     * 44 字节
[ text_pool  ] 以 '\0' 分隔的 UTF-8
[ text_ofs   ] text_count      * 2 字节
[ font_sizes ] font_size_count * 2 字节
[ name       ] name_len 字节
```

各段按 8 字节对齐。头部含：magic(`QLUI`)、version、header_size、
4 个计数、`node_struct_size` / `style_struct_size`（**ABI 校验**）、
total_size、FNV-1a 32 校验和、6 个段偏移。

#### 加载时的校验（缺一不可）

| 检查 | 失败信息 |
|---|---|
| magic | `bad magic (not a .qlui blob)` |
| version | `version mismatch` |
| header / struct size | `QLUINode layout mismatch (blob built on another ABI)` |
| total_size 与文件长度 | `size mismatch (file truncated)` |
| FNV-1a 校验和 | `checksum mismatch (file corrupted)` |
| 段偏移与计数推出的布局一致 | `section offsets inconsistent with counts` |
| 容量上限（`QL_CFG_UI_MAX_*`） | `too many nodes` / `text pool too large` |

策略是**明确失败**，绝不"跳过坏数据继续画"——半个正确的界面比报错难查十倍。
失败原因用 `ql_ui_doc_last_error()` 取。

#### 为什么不跨架构

blob 里是**结构体内存镜像**，`QLUIStyle` 的布局随编译器和 ABI 变。
所以格式刻意不追求可移植：头里存 `sizeof` 并在加载时逐项校验，
不匹配就明确拒绝。要跨架构分发就分发 HTML/CSS 源（等 M3.5 的 C 解析器）。

#### 所有权：一条判据

```c
arena == NULL   → 静态 doc（.rodata 或调用方自己管），ql_ui_doc_free 是空操作
arena != NULL   → ql_ui_doc_load* 分配，ql_ui_doc_free 释放
```

`ql_ui_load_file()` 返回的 root **持有** doc，`ql_ui_free(root)` 会一并释放，
所以最常见的一行用法不需要额外管理：

```c
QLUIRoot *ui = ql_ui_load_file("ui/index.qlui");
if (ui == NULL) { /* 原因见 ql_ui_doc_last_error() */ }
...
ql_ui_free(ui);      /* doc 一起释放 */
```

需要拿到 doc 自己管时用 `ql_ui_doc_load()` + `ql_ui_build()` + `ql_ui_doc_free()`。

#### 为什么序列化只在 C 侧实现

`ui_demo.qlui` 是 `tools/qlblob` 这个**宿主工具**调用 `ql_ui_doc_save()` 生成的，
不是 Python 写的。这样"HTML/CSS → doc"只有一份语义实现（Python 生成 C 表），
"doc → 二进制"也只有一份（C）。Python 只额外生成一个 6 行的
`ui_<name>_blob.c` 把 doc 交给工具。

`test_ui_blob.c` 会校验：**工具产出的文件 ≡ 测试现场编码的结果（逐字节）**，
这就是防止两种形态漂移的护栏。

#### 摘掉它（MCU）

`src/ui/qlui_blob.c` 是独立编译单元，`qlui.c` / `qlcore` 里**没有任何静态引用**指向它。
不把 `qlui_blob.c` 加进构建，blob 相关代码一个字节都不进镜像；
`qlui.h` 里的声明不产生引用，链接器不会拖入。

---

## 6. 与"真浏览器"的差距（诚实清单）

| 项 | 现状 | 影响 |
|---|---|---|
| flexbox / grid | ❌ 未实现 | 现代页面大量使用，是下一优先级 |
| 伪类 `:hover`/`:active`/`:focus` | ✅ 构建期编译成状态变体样式表（M1） | 只支持单个伪类；`:hover:active` 组合报错 |
| 混合内容 | ❌ 报错 | 需把文本包进 `<span>` |
| 行内元素的基线对齐 | ❌ 按矩形顶对齐 | 中英文混排可能不齐 |
| 文本换行 | 只有 `\n` | 长句不自动折行 |
| `position` / `overflow` / `z-index` | ❌ | 弹窗、滚动容器做不了 |
| CSS 变量 / transition / media query | ❌ | 明确不支持 |
| JS | ❌（编译期无 JS） | 交互靠 C 回调 |

> 这些差距是**刻意的**：MPU 档的目标是"设计师给的静态页面能渲染"，不是"跑任意网页"。
> 需要高兼容请走 PC + WebView（见设计文档 §7.3）。

---

## 7. 常见坑

| 坑 | 现象 | 说明 |
|---|---|---|
| `width:100%` + `padding` 溢出 | 比父元素宽 | `content-box` 语义；加 `box-sizing: border-box` |
| `<p>a<b>b</b></p>` 构建失败 | 混合内容报错 | 文本放进独立 `<span>` |
| 改了 HTML 但界没变 | 忘记重新构建 | 生成物是构建期产物，需 `cmake --build` |
| 改了 CSS 但没重生成 | 同上 | `add_custom_command` 已声明依赖，正常会自动触发 |
| 文本不换行 | 超出部分被裁 | 只支持 `\n` |
| 元素重叠 | 布局理解错 | 本引擎只有 block/inline 流，没有 `position` |
| 样式没生效 | 优先级/拼写 | 用生成物里的注释核对每个节点的最终样式 |

---

## 8. 热重载工作流（blob 的实际用法）

两个终端：

```bash
# 终端 1：窗口 + 监视文件大小变化
./build/example_ui_blob --watch

# 终端 2：改 test/web/ui_demo.css 后
python3 tools/qlui/qlui.py --html test/web/ui_demo.html \
                          --css  test/web/ui_demo.css --out build/ui
./build/qlblob build/ui/ui_demo.qlui
# 窗口自动更新，应用二进制没有重新编译
```

要点：

- 应用**不需要重编译**（`example_ui_blob` 的二进制没变）；
- 重载是整份重建（几十微秒），因为 doc 之间不能合并；
- 如果新的 `.qlui` 坏了，示例**保留旧界面**并打印原因——这条规则来自
  "明确失败优于半个正确"，`test_ui_blob` 里有对应断言。

---

## 9. 建议的阅读 / 验证顺序

1. `ctest --test-dir build -R test_web_ui --output-on-failure` 全绿；
2. 打开 `build/ui/ui_demo.c`，对照 HTML 看节点表与样式表 —— 这是最快的理解方式；
3. 读 `test/web/test_web_ui.c`：每个断言对应一条布局规则；
4. 读 `tools/qlui/qlui.py` 的 `parse_css` → `build_style` → `emit` 三个函数；
5. 读 `src/ui/qlui.c` 的 `layout_node`。
