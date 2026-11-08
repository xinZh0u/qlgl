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

单趟递归。**flex 容器会在内部做"测量 → 分配 → 定位"三步**（见 §5.6），
外层仍是单趟，因为不支持 `height: %`：

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

按树顺序（父先子后）：阴影 → 背景 → 边框 → 文本 → 递归子节点。
全部通过 `QLDraw2D`（所以 CPU/GPU 同一份代码）；GPU 上整页仍是**单批次**。

字体由 `font-size` 决定：生成期把用到的字号汇总成 `font_sizes[]`，
运行期 `ql_font_default(N)` 按字号取字体（配了矢量字体就是动态字体，支持中文/任意字号）。

#### 脏区剪枝：`paint_box`

每个节点在布局末尾多算一个字段：

```c
QLRect paint_box;   /* 本节点 + 整个子树可能画到的范围（绝对坐标，取上界） */
```

绘制时只要 `paint_box` 与当前裁剪不相交，**整棵子树一次判断就跳过**——
一个矩形相交判断替代一次子树遍历。

`paint_box` 的组成：自己的边框盒 ∪ 阴影外扩（`blur+1` 并平移 `(dx,dy)`）∪
未裁剪子树的包围盒。三个容易算小的地方：

1. **伪类能改 `box-shadow`** —— 所以要把 `:hover/:active/:focus` 三份样式也并进去，
   只算基础样式会在悬停时少画一圈光晕；
2. **伪类也能改 `display` / `overflow`** —— 任一变体不是 `none` 就不能清空包围盒；
   任一变体是 `visible` 就不能按 `overflow` 裁剪子树（并集必须偏大）；
3. **`overflow:hidden` 的子树被裁到本节点边框盒** —— 子包围盒并进来不会变大，
   但仍然要递归算下去，否则子节点自己会留着上一轮的陈旧值。

#### 局部重绘：`ql_ui_render_rect`

```c
void   ql_ui_render_rect(QLUIRoot *ui, QLDraw2D *d2d, QLRect dirty);
QLRect ql_ui_paint_bounds(const QLUIRoot *ui);   /* = 根节点的 paint_box */
```

`ql_ui_render_rect` 把 d2d 裁剪设成 `dirty ∩ 视口`，然后靠 `paint_box` 剪枝。
契约是硬性的：**在 dirty 区域内的结果必须与全量 `ql_ui_render` 逐像素相同**。

#### 高 DPI：布局永远是 CSS px

`ql_ui_layout(ui, w, h)` 的 `w/h` 是 **CSS px**，与 DPR 无关。
设备尺寸由表面决定，换算由 `QLDraw2D` 后端统一完成（见 render 手册 §10.7）：

```c
/* 640×480 设备像素、2x DPR → 布局按 320×240 CSS px */
ql_gfx_gl_set_scale(gfx, 2 * QL_SCALE_ONE);
ql_ui_layout(ui, 320, 240);
ql_ui_render(ui, ql_gfx_gl_draw(gfx));   /* 坐标仍是 CSS px */
```

CPU 路径同理，用 `ql_disp_set_scale()` / `ql_canvas_css_width()`。
`test/web/test_dpr.c` 断言"两种 DPR 下 `border_box` 逐字段相同"。

`test/web/test_render.c` 就是守这条的：9 个脏区（整幅 / 只覆盖卡片 / 只有阴影光晕 /
裁剪容器 / 跨裁剪边界 / 绝对定位 / 角落小片 / 视口外 / 空白区）逐一对比，
同时检查"脏区外一个像素都没被碰"。

这份测试上线时立刻抓到一个真 bug：`fill_shadow` 的下边带 / 右边带起点
（`sy1` / `sx1`）没有与裁剪上界求交，阴影被裁掉上半部分时会往上画到裁剪外面。
**这正是"动画必须走脏区"（M5.5）之前必须先把剪枝做对的原因。**

### 5.4 状态过渡 `ql_ui_anim_tick`

CSS 里写了 `transition` 的节点，在状态切换（`:hover` 进入/离开、`:active` 按下/松开）
时不再瞬间跳到新样式，而是把变化量交给 `qlanim` 插值。三个实现点：

1. **状态标志只能通过 `set_state_chain()` 改。**
   它会先记下沿祖先链每个节点的旧样式，改完标志后再对每个节点调 `trans_retarget()`。
   早期 `pointer_move` / `pointer_up` 直接遍历清 `flags`，绕过了这一步——后果是过渡
   继续往"按下时的颜色"走，松手后颜色永久卡在中间值。**这是红线，别再直接写 `flags`。**

2. **过渡样式存在独立池** `QLUIRoot.trans[]`（`QL_CFG_UI_TRANS_MAX`=16），
   结构里带一份完整的 `QLUIStyle style`。绘制时 `style_for_state()` 若发现该节点有活跃过渡，
   返回过渡中的那份，否则返回静态表里的那份。
   *不用* `style_copy`：那是 flex 强制尺寸的临时位，两者会互相踩。

3. **重基从当前绘制值开始**。`trans_retarget` 的起点取"这一帧实际画出来的样式"，
   不是 base 样式，所以快速来回 hover 不会跳变。

只支持 `background-color` / `opacity`，因为这两个字段一旦变化**不需要重新布局**
（不需要重算 `paint_box`；阴影颜色/模糊不变，文字不受影响）。`width` 之类会触发重排，
本架构明确不支持逐帧插值布局属性。

主循环接法：

```c
ql_ui_layout(ui, w, h);
if (ql_ui_anim_tick(ui, ql_anim_now_ms()) > 0) {   /* 0 = 这一帧没动，可整帧跳过 */
    QLRect areas[8];
    int n = ql_ui_consume_dirty(ui, areas, 8);      /* 取走脏区并清标记 */
    for (int i = 0; i < n; i++)
        ql_ui_render_rect(ui, &dw, areas[i]);       /* 旧位置 ∪ 新位置 */
}
```

脏区是"旧 `paint_box` ∪ 新 `paint_box`"，所以渐隐到底色时不会留下残影。

---

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

### 5.5.1 flexbox / 定位 / 混合内容的实现

#### flex 的三步法

```
layout_flex_children(容器):
  (1) 测量   对每个子项跑一遍 layout_node（哑位置），把自然尺寸写进 border_box
  (2) 分配   sum_base / sum_grow / 加权 sum_shrink → 算 free_space
             正 → 按 flex-grow 比例加；负 → 按 shrink × basis 加权减
             交叉轴：stretch 拉满，或按 align-items 对齐
  (3) 定位   写出最终尺寸（写进 style_copy，border-box 语义）再 layout 一次
```

三个设计决定：

1. **测量复用 `layout_node` 本身**，不另写一套 measure。
   多一次遍历，但保证 flex 子项的尺寸计算与 block/inline 路径**完全一致**。
   布局不在渲染热路径上（M4 会做增量布局），这个取舍是对的。
2. **不分配任何数组**：所有信息都暂存在子节点的 `border_box` 里，
   需要汇总时再走一遍链表。`QL_CFG_UI_FLEX_MAX_ITEMS` 只用于"是否降级"的判断。
3. **最终尺寸写进 `style_copy`**，复用既有的"运行期改写样式"机制，
   不给 layout 增加新接口。代价是**每次 `ql_ui_layout` 开头必须清掉上一轮的
   `style_copy`**，否则视口变化时会把旧尺寸带到新一帧（这个 bug 踩过）。

#### 定位

- `abs_cb` 字段在布局时自上而下传递：最近的非 static 祖先的 **padding box**；
- 绝对/固定子项在主流程结束后单独布局（主循环里 `is_out_of_flow()` 直接跳过）；
- `relative` 用 `translate_subtree()` 把整棵子树平移，**不改占位**；
- `z-index` 在布局末尾对每个父节点的子链表做**稳定插入排序**。
  这样绘制与命中测试**都自动遵循 z 序**，不必各自排序：
  `render_node` 正向遍历（先画的在下），`hit_walk_ui` 正向遍历并覆盖（最后的在上）。

#### overflow

`render_node` 进入 overflow 容器时 `set_clip(intersect(saved, box))`，退出恢复；
`hit_walk_ui` 传一个 `clip` 参数做同样的事。两处必须同步，否则会出现
"看不见但能点到"。

#### 混合内容：在生成期消解

```
<p>Hello <b>bold</b>!</p>
   ↓ qlui.py 的 _flatten()
<p><span>Hello </span><b>bold</b><span>!</span></p>
```

运行期只认识元素树，所以**布局与绘制零改动**。
两个细节：文本段空白按 HTML 规则折叠；元素之间的纯空白**不产生盒子**
（否则每个换行缩进都会变成 4px 宽的匿名 span，把整页撑高——这个 bug 踩过）。

---

## 6. 与"真浏览器"的差距（诚实清单）

| 项 | 现状 | 影响 |
|---|---|---|
| flex 完整度 | 无 `align-self` / `align-content` / `order` / `flex-flow`；百分比高度不给 grow 用 | 大多数卡片/工具栏/表单布局够用 |
| 绝对定位 | 无 "static position" 与完整 shrink-to-fit | 没给 inset 时贴左上角；非文本内容撑满 |
| overflow | 只有裁剪，**没有滚动** | 长列表不能滚动（后续里程碑） |
| 文本折行 | 只有 `\n`，不自动折行 | 长中文段落会溢出 |
| flexbox / grid | ❌ 未实现 | 现代页面大量使用，是下一优先级 |
| 伪类 `:hover`/`:active`/`:focus` | ✅ 构建期编译成状态变体样式表（M1） | 只支持单个伪类；`:hover:active` 组合报错 |
| 混合内容 | ❌ 报错 | 需把文本包进 `<span>` |
| 行内元素的基线对齐 | ❌ 按矩形顶对齐 | 中英文混排可能不齐 |
| 文本换行 | 只有 `\n` | 长句不自动折行 |
| `position` / `z-index` | ❌ | 弹窗做不了（`overflow` 只裁剪不滚动） |
| `transition` | ✅ 支持 `background-color` / `opacity` / `all`（M5.5） | 只有背景色与背景 alpha 可过渡；`color`/`border-color`/`delay` 报错 |
| CSS 变量 / `@keyframes` / media query | ❌ | 明确不支持（`@keyframes` 排 M5.5 ③） |
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
