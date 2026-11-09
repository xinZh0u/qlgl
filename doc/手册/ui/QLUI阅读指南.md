# QLUI / HTML+CSS 编译 阅读指南

> 本部分的定位见 [Web 兼容设计](../../Web兼容设计.md) §7：
> **MCU 不追求 Web 兼容（纯 C API）**；HTML/CSS 编译成 C 是给 **MPU/嵌入式 Linux** 用的中间档；
> **要跑任意网页**：本库不做，也不提供 WebView 回退 —— 任何平台都自己绘制。
>
> 本部分实现的是"构建期把 HTML+CSS 翻译成静态 C 数据"，运行时零解析。
> 目标读者：要改 `tools/qlui/` 或布局算法的人。只想用，请看 [QLUI API手册](./QLUI%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 阅读顺序 |
|---|---|---|
| `src/ui/qlui_parse.c` | HTML 解析 + CSS 解析 + 选择器匹配 + 级联（构建期与运行期共用） | ① |
| `tools/qltext/qltext.c` | 把上一行算出的 doc 落盘成 C 表 / `.qlui` | ① |
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
tools/qltext/qltext.c  ┘                                     │
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

## 4. 生成器（`src/ui/qlui_parse.c` + `tools/qltext/qltext.c`）

### 4.1 处理流程

```
HTML ──⑤ HTML 词法──▶ 节点树 ─┐
                              ├─▶ ⑥ 级联（UA 默认 → 规则 → 内联 → 继承）
CSS  ──④ CSS 词法────▶ 规则表 ─┘        │
                                        ▼
                              ⑦ 编译成 QLUIStyle ─▶ 去重 ─▶ ⑧ 建表进 arena
                                                                  │
                                        ┌─────────────────────────┴──────────┐
                                        ▼                                    ▼
                            qltext 把 doc 写成 C 表            运行期直接布局/绘制
```

**注意这张图的上半部分是唯一的一份**：`src/ui/qlui_parse.c` 算出来的 `QLUIDoc`，
构建期（`qltext`）拿它落盘成 C 源，运行期（`ql_ui_load_text`）拿它直接画。
两条路不存在两份代码，所以不可能漂移——这是 M3.5 第 4 步的全部意义。

### 4.2 CSS 解析

- 先去掉 `/* */` 注释，再按 `选择器 { 声明; }` 切块；
- **`@media` / `@supports` 等 at-rule 直接报错**（只认 `@keyframes`）；
- 属性白名单是一张 `(名字, 处理器)` 表，不在表里就报错并给出 `文件:行号`；
- 简写（`margin` / `padding` / `flex` / `gap` / `background` / `border` /
  `transition` / `animation`）**在单条规则内**就展开成长写，不能等级联合并后再展开。

### 4.3 选择器与优先级

支持：`tag`、`.class`、`#id`、后代（空格）、子代（`>`）。
**不支持 `+` / `~` 兄弟选择器**（会报错）。

优先级用规范的三元组 `(id 数, class 数, tag 数)`，排序后再按出现顺序覆盖 ——
这是"同一份 CSS 在两个引擎上渲染一致"的前提，不是可选项。

```c
/* 先按 (id 数, class 数, tag 数) 排，再按出现顺序 —— 稳定排序 */
qsort(matched, n, sizeof(*matched), cmp_specificity);
```

### 4.4 级联与继承

```
UA 默认样式（UA_DEFAULTS）
  ↓ 被匹配到的规则覆盖（按优先级）
  ↓ 被行内 style 覆盖
  ↓ 继承（只有 color / font-size / text-align / line-height）
  ↓ font-size 在生成期汇总成 font_sizes 表，运行期按字号取字体
```

继承的实现很直接：父节点先算完样式，子节点从父的样式里挑 `INHERITED` 三个属性作为初始值。

### 4.5 明确报错的场景（不静默降级）

红线是"明确失败优于半个正确"：本实现做不到的写法一律在构建期/加载时爆掉，
而不是运行时什么都不发生。当前的拒绝集合（`test_text_reject` 每条都在钉它）：

| 类别 | 例子 |
|---|---|
| 不认识的标签 | 自定义标签、obsolete 元素（`marquee` / `center` / `font` 等）；`<iframe>` 之类则是**占位盒**，不是错误 |
| HTML 结构错 | 结束标签不匹配、标签未闭合、根元素不是恰好 1 个 |
| 属性名不认识 | `inset` / 任意拼错的属性 |
| 属性值不认识 | `position: sticky`、`display: grid`、`border: 2px dashed` |
| 单位 | `ch` / `ex` / `lh`（支持 `px` / `%` / `em` / `rem` / `vw` / `vh` / `vmin` / `vmax` / `calc()`） |
| 颜色 | `#12345`、拼错的命名色 |
| at-rule | `@supports` / `@font-face`（认 `@keyframes`，`@media` 只认宽度断点） |
| 选择器 | `:has()`、属性选择器的复杂匹配（`+` / `~` / `::before` 已支持） |
| 简写值个数 | `margin: 1px 2px 3px`（只接受 1/2/4 个） |
| transform | `rotate()` / `skew()` / `matrix()`；`scale` 超出 `0~32.767` |
| 关键帧 | 百分比重复/越界、各帧属性不一致、帧里写了不支持的属性 |
| animation | 没写 `duration`、`duration: 0`、`steps()`/`cubic-bezier()`、`paused`、三处时长 |
| transition | 延迟（`transition-delay`）仍不支持；`transition` 简写里各段的时长/缓动必须一致 |
| 状态规则 | `:hover` 里写 `animation`（动画只按基础样式起播，写了会静默失效） |
| backdrop-filter / clip-path | `blur` 半径不在 `1~64px`、`clip-path` 不是 `circle()` |

> **已经不再是错误的东西**（M3/M5.5 之后）：`position` / `flex-*` / `box-shadow` /
> `linear-gradient` / `transform` / `clip-path` / `<img>` / `<input>` / `@media`（宽度断点）/
> `calc()` / CSS 变量 `/ `+` 与 `~` 兄弟选择器 / `::before` 全都支持了。
> 本文档早期版本把它们列在"报错"里，如果你看到旧说明请以这张表为准。

**唯一一个"结构上"的限制**：元素级 doc 不能混用（跨 doc 的 `uint16` 下标无法引用），
混用单位锁死在**面板**（一个 `QLUIRoot` ↔ 一份 doc）。

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

支持的通道：`background-color`（含渐变终止色）/ `opacity` / `color` / `border-color` /
`border-radius` / `transform` / `clip-path`。
判据只有一条：**插值它不需要重新布局**。`width` 之类会触发重排，
本架构明确不支持逐帧插值布局属性。

`transform` 插值必须**四个分量一起动**（`tf_x`/`tf_y`/`sc_x`/`sc_y`）：
只动 translate 不动 scale，元素会"边缩边移"错位。

**顺带修掉的一个脏区 bug**：`scale_extent()` 原来只看**基础样式**的 `sc`，
所以 `@keyframes` 里放大、`:hover` 里放大的元素 `paint_box` 都算小了——
脏区漏掉放大的那一圈，缩回去时留下残影。现在 `scale_max()` 和 `move_extent()`
一样把三个来源（基础样式 / 状态变体 / `@keyframes` 帧）取最大值。
**`paint_box` 的每一条通道都要按"可能取到的极值"算，只按当前值算一定会漏。**

插值的是**整份样式**：过渡槽存 `from` / `to` / `style` 三份 `QLUIStyle`，
`QLAnimKey.v[0]` 只当 0..1 的进度用。这样通道数不受 `QLAnimValue` 的 4 个 float 限制，
加新通道只是多一个 `trans_props` 位 + 一次 `lerp_color()`。

**shorthand 必须在单条规则内展开**（`qlui_parse.c` 的简写展开段）：
级联合并之后再展开，会把后出现的长写覆盖回 shorthand 的值 ——
`.card{animation: x 1s} .a{animation-name: y}` 会让 `.a` 也跑 `x`
（踩过：10 张卡片退化成同一段关键帧）。`transition` / `flex` / `gap` /
`background` / `border` 同理。

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

### 5.4.1 关键帧动画 `@keyframes`

`transition` 是"状态变了就渐变过去"，`@keyframes` 是"自己一直播"。
两者在运行期**共用同一条路径**（求值 → 覆盖样式 → 标脏 → 局部重绘），区别只有求值函数：

```
transition:  一次线性插值（qlanim 管，重基从当前值开始）
@keyframes:  按时间定位到帧段 + 段内插值（qlui 自己算，无状态可打断）
```

**构建期**（`qlui_parse.c`）：`@keyframes` 编译成两张静态表

```
QLUIKeyframe[]  每帧：offset(千分比) + 缓动 + 通道位图 + bg_color/bg_color2/alpha  （16 B）
QLUIAnimation[] 每段：first_frame + frame_count + dur_ms + iterations + direction + fill
QLUIStyle.anim  该节点播第几段（0xFFFF = 没有）
```

`@keyframes` 会被**去重**：`.pulse` 和 `.rev` 引用同一个 `pulse` 段时共享同一段帧表，
只是 `QLUIAnimation` 里的 `direction` 不同（多一条 10 字节的记录）。

**运行期**（`qlui.c`）：

1. `ql_ui_build` 时给每个 `anim != 0xFFFF` 的节点分配一个 `QLUIAnimRun`，
   并**立即落到 0% 帧**——第一次绘制就正确，不需要等第一帧 tick。
2. 起始时刻在**第一次 `ql_ui_anim_tick`** 时记下（`started` 标志）。
   这样测试可以用假时钟完全控制，主循环也不需要额外传时间。
3. 每帧 `elapsed = now - start_ms` → `iter = elapsed / dur`、`rem = elapsed % dur`
   → 按 `direction` 决定是否 `1-u` → 定位帧段 → 段内缓动 → 写 `run->style`。
   **不累加 `dt`**，所以掉帧/补帧/窗口最小化都不会漂移。
4. `style_for_state()` 之上再插一层：有动画的节点，`ql_ui_node_style()` 直接返回
   `run->style`（基础样式 + 颜色覆盖）。

**为什么用 `QL_UI_KF_BASE` 而不是给每个元素生成帧表**：
`@keyframes x { 50% { opacity: 0 } }` 的首末帧在 CSS 里是"元素自己的值"。
把这两帧标成"取基础样式"，同一个 `@keyframes` 就能被任意底色的元素复用——
否则每个引用它的元素都得复制一份帧表。

**`animation-delay`**：`QLUIAnimation.delay_ms`，`elapsed < delay` 时不进动画。
`fill-mode` 带 `backwards`/`both` 时延迟期间亮出 0% 帧，否则**回到基础样式**。
`QLUIAnimRun.phase` 记 0/1/2（未起播 / 延迟中 / 播放中），保证"延迟里每帧都改样式"
不会把元素标脏一辈子。踩过的 bug：判延迟的条件写成 `phase == 0 && elapsed < delay`，
第二天 tick 起 `phase` 已是 1，直接掉进 `elapsed -= delay` 的**无符号下溢**，
延迟里的元素被算成一个天文数字的 elapsed（实测整块消失）。

`animation` 简写的第二个时长是 delay（`animation: pulse 1s 300ms`），第三个报错。

**三个容易踩的点**：

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 关键帧里写 `color`/`box-shadow` | 构建期报错 | 只能用 `background*` / `opacity` / `transform: translate` / `animation-timing-function` |
| 各帧属性集合不一致 | 构建期报错 | 要么写全，要么只写中间帧（首末自动补） |
| 用 `ql_ui_anim_tick() == 0` 判断"动画播完了" | infinite 动画永远非 0 | 那是"这一帧有没有东西要重绘"的判断 |

---

### 5.4.2 `transform: translate`：一处改动要动四个地方

`transform` 不进布局，所以它**改变的是"节点画在哪"而不是"节点占多大"**。
这条性质让实现很便宜（没有重排），但也意味着凡是"假设坐标 = 布局结果"的地方都要改：

| 位置 | 改法 |
|---|---|
| 绘制 `render_node` | 把 `transform` 加到 `d2d->offset_*` 上，**逐层累加**，画完自己再画子树、最后恢复。子树跟着平移，与 CSS "transform 建立包含块"一致 |
| 命中 `hit_walk_ui` | 反向来一遍：把祖先 + 自己的平移加起来，把框挪到"实际画的位置"再判断。用**生效样式**（动画中的值），移动中的元素也点得中 |
| `paint_box` | 必须覆盖"旧位置 ∪ 新位置"。布局末尾 `resolve_transform()` 自上而下算出 `tf_abs_*`（静态绝对平移）与 `tf_dev_*`（动画位移区间，含祖先），`paint_box` 取 Minkowski 包围盒 |
| 排序 / 布局 | **不用改**——这正是"绘制期属性"的收益 |

**`resolve_transform` 里最容易写错的一行**：本节点的 `tf_abs` 必须**包含自己**的平移
（`ax + sdx`），因为渲染时就是"先设自己的偏移、再画自己"。写成 `ax` 的话，
自己平移了但 `paint_box` 不动 → 脏区漏掉新位置，画面撕裂。动画位移区间同理
（`lo_x + nx`）——这个 bug 在实现时真的出现过，被"滑入第一帧 paint_box 覆盖旧位置"那条断言抓住。

`translate` 用**整数像素**（`int16_t tf_x/tf_y`）。分数像素的需求由 M5.3 的
`ql_draw2d_set_offset`（26.6 定点）承担，两者共用同一个绘制偏移字段，所以
"动画平移 + 亚像素偏移"不会打架。

---

### 5.4.3 假 3D 翻转：`scaleX()` / `scaleY()`

真 3D 需要透视矩阵、深度排序、背面剔除——那是一条独立的技术路线。
**"卡片翻面"这个需求其实不需要 3D**：

```
scaleX: 1 → 0 → 1     元素在水平方向压到零宽再展开 = 翻转
```

`scale(1.2, 2)` 这种非等比写在 CSS 里是很自然的，所以这次放开了它：
`QLUIStyle.sc_x` / `sc_y`（千分比，两个 `int16_t`），
`QLDraw2D.elem_x_fx` / `elem_y_fx`（26.6）。`scale(s)` 给两轴同一个值，
`scaleX(s)` 只改 x、`scaleY(s)` 只改 y。

#### 为什么这次能做，而 `rotate()` 还是不行

轴对齐矩形在**轴对齐**缩放下仍然是轴对齐矩形。所以 CPU 光栅化一行都不用改：

```c
sx()  →  按边独立缩放      /* 只要把 elem_fx 换成 elem_x_fx / elem_y_fx */
sl()  →  长度按 min(sx, sy)
```

`rotate()`/`skew()` 会把矩形变成平行四边形，行跨度扫描得整个重写——那才是大工程。

#### 非等比带来的三个歧义，以及选择

| 东西 | 非等比下"正确"是什么 | 本实现 |
|---|---|---|
| `border-radius` | 圆角变成椭圆角（左右半径 ×sx、上下 ×sy） | 按 `min(sx, sy)` 缩放：`sx=0` 时圆角归零，但那时宽度也是 0，看不见 |
| `box-shadow` / `border-width` | 同上，各向异性 | 同样按 `min(sx, sy)` |
| 文字 | 字形横向压扁（真翻转的样子） | 字号按 `min(sx, sy)` 重取，位置按轴缩放。压到 0 时**一个字都不画** |

文字那条是唯一"看起来不对"的地方：`scaleX(0.7)` 时文字高度也变成 0.7 倍。
翻转动画 300~900ms，中途宽度趋 0，视觉上察觉不到；若真要逐像素压扁字形，
得给字形位图加一次最近邻重采样（成本高于收益，且 MCU 上不可能）。

#### 压到 0 的那些坑

`sc == 0` 是**完全合法的值**（翻转的中间帧），所以：

* `ql_draw2d_set_elem_scale_xy()` 的钳位必须是 `fx >= 0`（写 `> 0` 会把 0 悄悄变成 1，
  翻转动画在中点闪一下满宽）；
* 命中测试里 `sfx == 0 || sfy == 0` 直接 `return NULL`，否则是除零；
* 文本绘制里 `elem_len_fx == 0` 提前返回，否则几十个字形全叠在中心一条线上。

#### 命中测试：从"盒子膨胀"改成"逆缩放"

以前（只支持放大时）的做法是"`sc > 1000` 就把命中盒按比例撑大"，这是**近似**：
缩小时盒子不缩，被压掉的部分还能点中。有了非等比缩放后这个近似太糙，改成精确的：
把点绕元素中心**逆缩放**回布局空间再做矩形判断。

```c
hx = cx + (x - cx) * 1000 / sc_x;
hy = cy + (y - cy) * 1000 / sc_y;
if (hx >= box.x && hx < right(box) && hy >= box.y && hy < bottom(box)) → 命中
```

`overflow` 的裁剪区仍然按**放大后**的可见范围算（绘制侧的 clip 也是缩放后的矩形），
所以 `vis` 那一段留着。

#### `backface-visibility` 为什么直接报错

CSS 的 `backface-visibility: hidden` 靠"这个面朝后（变换行列式为负）"来决定不画。
**假翻转根本没有背面**——没有 3D 变换，也就没有行列式符号可看。
所以本实现里它是"不支持的属性"，构建期报错。
真要翻面换成另一张脸，用两个元素错开 `animation-delay` 更简单：

```css
.front { animation: flip-x 900ms ease-in-out infinite; }
.back  { animation: flip-x 900ms ease-in-out infinite; animation-delay: 450ms; }
```

`test/web/flex/flipdemo.*` + `example_flip_window` 就是这么演示的（水平翻、垂直翻、
圆形"硬币"各一张，错开相位）。

---

### 5.4.4 毛玻璃：唯一会"回头读自己画过的东西"的效果

`backdrop-filter: blur(Npx)` 要模糊**元素背后已经画好的内容**。

布局与数据侧几乎没有成本：`QLUIStyle` 只加了一个 `uint8_t backdrop_blur`
（正好塞进原来的 padding，**116 字节没变**，blob version 9 → 10）。
`render_node` 里就是在"画自己的背景之前"插一句：

```c
if (s->backdrop_blur > 0 && d2d->backdrop_blur != NULL)
    ql_draw2d_backdrop_blur(d2d, box, s->backdrop_blur);
```

真正的戏在绘制后端，两边做法完全不同：

| | 做法 | 代价 |
|---|---|---|
| CPU | 画布就是"背后"，**就地**取区域 → 两趟滑动窗口盒式卷积 → 写回 | 108×56 卡片 **0.159 ms**；300×200 **1.68 ms** |
| GPU | **中途 flush 一次**批次 → blit 降到半分辨率 → H/V 两趟 9 抽头 → mode 4 画回 | 额外 1 个 draw call、6 个顶点、两块半分辨率纹理 |

GPU 那条要特别小心三件事（`QLRender阅读指南` §15）：`u_bg` 采样器必须始终绑着东西、
FBO/视口必须还原、纹理上下颠倒。这三条都是"错了就整块变黑/变倒影"级别。

**为什么 CPU 也能做**：本架构没有保留式场景图，但画布本身就是"前面画完的东西"，
所以"模糊背后" = "模糊这块内存"。代价是 O(面积)，所以只适合小卡片；MCU 明确不支持。

**两条后端不一致是故意的**：CPU 全分辨率盒式、GPU 半分辨率高斯近似，
所以 `test_blur` 不比逐像素，而是断言性质（玻璃内出现长串中间色、玻璃外一个都没有）。

---

### 5.4.5 文本的竖直位置：`line-height` 是 `text-align` 的 counterpart

`text-align` 管水平，`line-height` 管竖直。少了后者，"按钮里的字对不齐"
就**没法表达**——只能靠 `padding-top` 硬凑，`box-sizing` 一改就散。

实现只有一条公式（`ql_draw2d_text_layout`，CPU/GPU 共用）：

```
行盒高 L  = line_h > 0 ? line_h : 字体自身 line_height
半行距  h = (L - (ascent + descent)) / 2      ← L == a + d 时为 0
第一行基线 = box.y + h + ascent
后续每行   += L
```

因为 `QLFont.line_height == ascent + descent`，不给 `line-height` 时半行距恒为 0，
**与旧版本逐像素相同**（这也是为什么加这个属性只需重生成 blob、基准图一张都没动）。
给了 `line-height: 26px` 而盒高也是 26px 时，字形区上下各得 13 - a/2 的空间，
墨迹中心正好落在盒中心——这就是"完美居中"的由来。

`line_h` 同时参与两处：绘制时的基线计算，以及**文本节点 auto 高度**的内容高
（`text_line_h_of()`）。后者保证 `line-height: 30px` 的裸文本行真的占 30px 高，
而不是字体行高。


### 5.4.6 自动折行：`white-space: normal`

**默认 `nowrap`（与浏览器不同）**，这是刻意的：v0 的文本模型是"一段文本一行"，
整套逐像素基准都建立在这个前提上。折行要显式打开。

断行只有一个实现：`ql_text_wrap()`（`src/draw/qldraw2d.c`）。它同时被
**布局**（数行数 → 算高度）和**绘制**（逐行发射字形）调用 —— 这是唯一能保证
"布局与绘制对行数有分歧"这类 bug 不出现的方式。

```
ql_text_wrap(font, text, max_w, out, max_lines)
    max_w = 0        → 只按 '\n' 断（nowrap）
    out   = NULL     → 只数行数（布局用，不分配内存）

ql_text_wrap_ex(font, text, max_w, out, max_lines, keep_trailing)
    keep_trailing = 1 → 每行**最后一段**的尾随空格保留宽度
```

`keep_trailing` 只给输入框用：普通文本的行尾空格不该占宽（否则 `text-align: right`
会凭空缩进），但输入框末尾的空格是真实内容 —— 裁掉它就会出现"在末尾敲空格，
值插进去了、屏幕上什么都不动"。绘制侧对应 `QLTextStyle.keep_trailing`。

规则：空格后断（丢弃行首行尾空白）、CJK 之间随处断、超长单词硬断、`'\n'` 强制断。

绘制侧在栈上开 `QL_CFG_TEXT_MAX_LINES`（默认 64）条行记录，超出的行不画；
**行数统计不受这个上限影响**（否则长文本的高度会算少）。

**踩到的两个坑**

1. **宽度里必须算上空格**。第一版把空格当成"零宽"（想省掉行尾空白），结果
   含空格的英文行实际宽 120px 却被判成放得下 —— 直接把折行逻辑废掉了。
   正确处理是：宽度照算，断行**之后**再 `trim_line_end()` 去掉尾随空白。
2. **`max_content_main()` 没分轴**。它给 flex 项算 max-content 主轴尺寸时，
   对文本节点一律返回"文本宽度"。可主轴是竖直（`flex-direction: column`）时，
   max-content 应该是**高度**（行数 × 行高）。后果很夸张：一个 46 个汉字的
   简介在 column 容器里声称自己 322px 高，整页被撑爆、折行高度也算不出来。
   现在按 `row ? 文本宽度 : 行数 × 行高` 分轴，折行时行数用"父容器给的可用宽度"估。

---


### 5.4.7 标签与 UA 默认样式

标签在本架构里**只影响三件事**：UA 默认样式、`tag` 选择器、调试名。
布局与绘制只看编译出来的 `QLUIStyle`，不看标签——所以加标签几乎零成本（只加枚举）。

共 101 个（HTML5 共 114 个；其中 13 个是占位；全量状态见 [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html)）。
`QLUITag` / `k_tags[]` / `s_tag_names[]` **三处顺序必须一致**，
且只能在末尾追加：`node.tag` 是存进 blob 的数字）：

| 类 | 标签 | UA 默认样式 |
|---|---|---|
| 容器 | `div` `section` `header` `footer` `nav` `main` `article` `aside` `figure` `figcaption` `blockquote` | `display: block` |
| 标题 | `h1`…`h6` | block + 字号 24/20/17/15/13/12 + 递减 margin |
| 文本块 | `p` | block + `margin: 8px 0` |
| 代码块 | `pre` | block + **`white-space: pre`**（空白与换行全保留、不折行）+ 等宽 + `margin: 8px 0` |
| 上下标 | `sub` / `sup` | inline + `vertical-align: sub/super` + `font-size: smaller`（父级 ×5/6） |
| 行内 | `span` | `display: inline` |
| 语义行内 | `b` `strong`（加粗）`i` `em`（斜体）`u`（下划线）`s`（删除线） | inline + 对应的字体变体 |
| 高亮 | `mark` | inline + 黄底黑字 |
| 列表 | `ul` `ol` / `li` | block + `margin: 8px 0` / block；`padding-left: 18px` |
| 空元素 | `hr` / `br` | 1px 细线 + margin / 高度 0 的块 |
| 控件 | `button` | inline-block + **`box-sizing: border-box`** + 内边距 + 边框 + 圆角 |
| 输入 | `input` | inline-block + **`box-sizing: border-box`** + 内边距 + 边框 + 圆角（`height: 26px` = 内容 24 + 上下边框；`type=password` 额外置位） |
| 多行输入 | `textarea` | inline-block + **`box-sizing: border-box`** + 200×60 + **`white-space: pre-wrap`** + 等宽 + 边框 |
| 下拉 | `select` / `option` / `optgroup` | 与 `input` 同形（同样 `border-box`）/ **`display: none`** |
| 位图 | `img` | 固有尺寸来自 doc 的资源条目（不看解码是否成功）；`text` 存**资源名** |
| 浮层 | `dialog` | 居中 + 遮罩；整棵树画完再画一遍（子树每帧多画一次） |
| 折叠 | `details` / `summary` | block；收起时非 `summary` 子节点指向共享隐藏样式 |
| 控件 | `progress` / `meter` / `output` / `datalist` | 进度条（池里只存完成度 0..1000）/ 度量条 / 行内文本 / **`display:none`** |
| 占位（EMBED） | `iframe` `embed` `object` `audio` `video` `canvas` `svg` `math` | 占位盒：1px 实线边框 + 居中的**标签名**（`300×150`；`audio` 矮条；`svg`/`math` 是 `120×60` inline-block）。小盒子不画字 |
| 占位（无盒子） | `script` `noscript` `slot` `template` `track` | **`display: none`**；`script`/`template` 的内容在解析期整段跳过，`svg`/`math` 的内容同样跳过（非 HTML 子树） |
| 元数据 | `base` `head` `link` `meta` `style` `title` | **解析期整段跳过**：不进树、不产生盒子；`<style>`/`<title>` 按 raw text 处理（内容里不认标签） |
| 纯语义 | `html` `body` `a` `abbr` `code` `kbd` `samp` `var` `q` `dfn` `cite` `data` `time` `small` `bdi` `bdo` `del` `ins` `dl` `dt` `dd` `menu` `address` `hgroup` `search` `form` `label` `fieldset` `legend` `wbr` | 只给 `display`，另有几处"真的看得出"：`dd` 缩进 18px、`cite`/`var`/`dfn`/`address` 斜体、`del` 删除线、`ins` 下划线、`fieldset` 边框 |

**几处必须知道的取舍**

1. **字体变体是合成的**：加粗 = 画两遍偏移 1px，斜体 = 按行剪切，装饰线 = 1px 横线。
   不加载第二个字体文件（MCU 上也不可能）。详见 §5.4.8。
2. **表单控件默认 `box-sizing: border-box`**（与浏览器一致）：`width: 100%` 加上
   `padding`/`border` 不会溢出父容器。写在选择器里的 `box-sizing` 照常覆盖它。
   不给普通元素这个默认值 —— 那一侧 `content-box` 才是 CSS 语义。
3. **列表不画项目符号**：本库没有生成内容（`::before` / `::marker`），标记请自己写字符。
4. **`<br>` 是一个"高度为 0 的块"**：没有生成内容，也没法在一个文本节点里插换行，
   所以靠"块级会结束当前行内行"来达到换行的效果。代价是它占一个节点。
5. **`<table>` / `<textarea>` 明确报错**，错误信息里给出替代做法
   （表格用嵌套 flex、多行文本用 `white-space: pre`）。`<input type="date">`
   这类没实现的 `type` 也报错，并列出支持的那几个。
6. **空元素的结束标签被忽略**（`<br></br>` / `<embed></embed>`）：浏览器这么做，
   这里照做 —— 否则看着正常的写法会让整页编译不过。非空标签不匹配仍然报错。

**行内元素之间的空白保留成一个空格**（`<b>a</b> <i>b</i>` 不能变成 `ab`）。
这一步在解析期做，靠的是标签的**默认** display（级联还没跑），所以如果你把
`span` 改成 `block`，这个判断不会跟着变。块级兄弟之间的空白照旧丢掉，
否则每个缩进都会变成一行。flex 容器里这种"只有空白的 span"会在 `ql_ui_build`
时被共享的隐藏样式关掉（CSS：只含空白的匿名 flex 项不渲染），零额外内存。

### 5.4.8 字体变体：合成加粗 / 合成斜体 / 装饰线

`QLUIStyle.font_flags` 一个字节装四种位（`QL_UI_FONT_*`），`QLUIStyle` **仍是 120 字节**
（塞进最后一个 padding 字节）。全部**合成**，不换字体文件：

| 效果 | CPU（`qlcanvas.c`） | GPU（`qlgfx_gl.c`） |
|---|---|---|
| 加粗 | 同一个字形 `glyph_blit()` 跑两遍，第二遍 `strike = 1px` | 同一个 quad 发两遍，第二遍 `x + 1` |
| 斜体 | 行循环里逐行右移：`row = round(shear × (hh−gy) / (hh × 16))` | 顶边两个顶点右移 `shear`，**uv 仍按剪切前角点取** |
| 装饰线 | `fill_rect` 一条 1px | 同一条 `fill_rect`（就是普通 quad） |

**斜体必须用 1/16 像素定点**：9px 高的小字按整数像素剪切只有 1px，分摊到 9 行里
每行要么 0 要么 1（视觉上就是没斜）。改成 1/16 后每行都能分到不同位移。

装饰线画在 `ql_draw2d_text_layout()` 里，因为只有它知道每行的起止与基线 ——
放这里两个后端就自动都有，不用各写一遍。空行（`llen == 0`）不画，否则折行产生的
空行会拖一条线。

**三个坑**：

1. 加粗的第二遍必须带**同样的剪切**，否则加粗斜体的右下角会缺一块。
2. GPU 侧 `emit_quad()` 加了 `shear` 参数后，`v.u` 必须用**剪切前**的角点算，
   否则纹理采样跟着四边形一起歪（字形会变成斜着错位的一团）。
3. 行高是 CSS px，到了后端要按 `font_fx` 放大；装饰线画在设备像素里，
   HiDPI 下不能还是 1 个 CSS px 粗。

---

### 5.6 滚动：偏移烘进布局

`overflow: auto` 与 `hidden` 都裁，只有 `auto` 能滚。实现方式和绝大多数框架相反：

```
ql_ui_scroll_by() → node->scroll_y → ql_ui_layout() → 布局末尾 translate_subtree(子节点, -scroll)
```

**为什么不搞"绘制偏移 + 命中逆变换"**：绘制、命中测试、`paint_box`、脏区这四件事
读的都是 `node->border_box`。坐标一旦是对的，四件事自动一致；否则要维护
"绘制加了偏移、命中忘了加"这类四份不同步的正确性。代价是滚动要重排，
实测 211 节点 0.037 ms，只在滚动时发生。

**`scroll_to` 不重排，`scroll_by` 重排**：前者是"设置状态"（连续设多次只该排一次），
后者是"做一次操作"（用户滚一下就该看到）。

| 项 | 位置 | 说明 |
|---|---|---|
| 可滚范围 | `apply_scroll()` 末尾 | `ext - content`，`ext` 由子节点 `border_box` 外沿 + margin 求出 |
| 夹紧 | 同上 | 范围变化（窗口缩放、内容变长）时自动夹回，不会卡在越界位置 |
| 滚动条 | `draw_scrollbar()` | 覆盖式 4px 滑块，不占布局；`hidden` 不画 |
| 滚轮 | `ql_ui_input_event` 的 `QL_INPUT_WHEEL` | 命中 → 往上找最近可滚祖先；**一格 = `QL_CFG_UI_WHEEL_STEP_PX`（默认 50px ≈ 3 行）**，滚到底继续交给父级 |
| 拖滚动条 | `ql_ui_pointer_down/move/up` | 右侧 10px 热区；拖动**不算点击**（`scroll_drag` 非空直接吞掉） |

**两个坑**

1. 拖动滚动条时如果先走 `set_state_chain(node, ACTIVE, 1)`，整个列表会闪一下 `:active`
   样式。所以 `pointer_down` 里必须先判滚动条热区，命中就直接接管。
2. `translate_subtree` 会一并平移 `abs_cb`，所以容器里的绝对定位子元素也跟着滚
   —— 这是对的（CSS 里绝对定位相对的是 padding box，滚动会让它移动）。

---

### 5.7 输入框：唯一的运行期可变文本

```
doc.nodes[i].text ──▶ 池里的可变缓冲（build 时把初始值拷进去）
```

整套数据模型都是编译期算完的静态表，`<input>` 是**唯一例外**：它有自己的一条池
（`struct QLUIInput`），`build_internal()` 按需分配 —— 界面里没有 `<input>` 就一个字节都不花。

| 项 | 字段 | 说明 |
|---|---|---|
| 值 | `buf` / `len` | UTF-8，上限 `QL_CFG_UI_INPUT_LEN − 1` 字节 |
| 光标 | `caret` | **字节**偏移（`prev_boundary` / `next_boundary` 负责跨过 UTF-8 续字节） |
| 选区 | `sel` | 锚点；`sel == caret` 表示没有选区 |
| 横向滚 | `scroll_x` | 文本比盒子宽时，**只在绘制期**用（`DISP_OFF` 宏），不进布局 |

**为什么值要放进池、而不是另开一个"值字段"**：让 `node->text` 指过去，
绘制、`text-align`、自动折行、横向滚动**一行都不用改** —— 它们读的就是最新值。

**节点标志位**：`QLUINode.flags` 一个字节省着用（`QL_UI_NODE_PASSWORD`），
旁边留了 `reserved`。这就是 blob **v11 → v12** 的原因，`sizeof(QLUINode)` 18 → 20。

| 功能 | 实现位置 | 坑 |
|---|---|---|
| 密码圆点 | `input_display_text()` | 宽度必须按**圆点**量，否则光标飘出框外 |
| 光标/选区绘制 | `draw_input_decor()` | 1px 竖线 + 半透明矩形，画在文字之后 |
| 键盘 | `input_key()` | `Enter` 只发回调（没有 `<form>`）；`Esc` 收焦点 |
| 超长插入 | `input_insert()` | **静默截断**（用户打字不该因为满了而失败） |
| 回调 | `input_emit()` | 只在值真的变了时发 |

**没有剪贴板、没有 IME、光标不闪**：前两个要平台层提供事件，第三个要求主循环
一直醒着（省电优先）。都写进了 §6 的诚实清单。

### 5.8 下拉框：覆盖式面板

```
<select>  →  struct QLUISelectState { node, sel, open }   池：QL_CFG_UI_SELECT_MAX
<option>  →  display: none 的普通节点（只被面板读文字）
```

| 项 | 位置 | 说明 |
|---|---|---|
| 面板矩形 | `select_panel_rect()` | 贴在 select 下方，高度夹到 `QL_CFG_UI_SELECT_PANEL_H` |
| 绘制 | `draw_select_panel()` | **`ql_ui_render` / `render_rect` 末尾单独画**：不占布局、不受父级 `overflow` 裁剪 |
| 命中 | `select_pointer_down()` | 先判面板（它在最上层），再判"点面板外 → 收起" |
| 同步显示文本 | `select_sync_text()` | 选中项的文字就是 select 的文字（`node->text` 直接指过去） |
| 键盘 | `select_key()` | `↑`/`↓` 改选中，`空格`/`回车` 展开收起 |

**一个容易写错的分支**：点面板外要"收起 + 继续走普通点击"，但如果点的正是这个
select 自己，那就是"再点一次关掉"，必须 **return**，否则会立刻又展开（`open` 先清 0
再走到展开分支）。这个 bug 是被 `test_select` 第 4 项（白色像素数回到原值）抓到的。

**同一时刻只开一个**：`ui->open_select` 只有一个指针，够用且省事。

### 5.9 焦点：Tab 在控件之间循环

可聚焦 = `input` / `select` / `button` 且 `display != none`（`node_is_focusable()`）。
`Tab` / `Shift+Tab` 按**树序**步进，到头上绕回（`focus_step()`）；
聚焦到输入框时光标放末尾，不选中全文（"一打字就覆盖整串"的语义留给多行编辑一起定）。

焦点本身只做两件事：点亮 `QL_UI_RT_FOCUS`（`:focus` 样式靠它，`state_chain` 一路传到父级），
以及决定键盘事件归谁。**绘制不读焦点**，所以焦点变化只标脏、不重排。

---

### 5.5 `.qlui` blob：换界面不用重编译

```
                    ┌──▶ ui_demo.c ──▶ 编译器 ──▶ .rodata ──▶ ql_ui_build()
index.html + index.css ──qltext cc──┤
                    └──▶ ui_demo.qlui ──▶ ql_ui_load_file()（换界面不重编译）
```

#### 为什么加载这么便宜

`QLUIDoc` 里**所有内部引用都是 `uint16` 下标**，唯一的指针是 doc 自己的
那几个基址字段（nodes / styles / 文本池 / 动画 / 图片 / 热区 / name）：

```c
QLUIDoc {
    nodes / styles / text_pool / text_offsets / font_sizes
    anims / keyframes / images / image_data / areas / area_coords / name
    node_count / style_count / text_count / ... / area_count
    arena        ← NULL = 静态数据，不归我管
}
```

所以"解码"= 校验 + 设这些指针，**没有任何递归重定位**。这是当初"用下标不用指针"
的一个意外红利。

#### 格式（**v14**）

```
[ QLUIDocBlobHeader 96 字节 ]
[ nodes      ] node_count       * 20 字节
[ styles     ] style_count      * 120 字节
[ text_pool  ] 以 '\0' 分隔的 UTF-8
[ text_ofs   ] text_count       * 2 字节
[ font_sizes ] font_size_count  * 2 字节
[ anims      ] anim_count       * 12 字节
[ keyframes  ] keyframe_count   * 36 字节
[ images     ] image_count      * 20 字节
[ image_data ] image_data_size 字节（编码后的资源字节）
[ areas      ] area_count       * 10 字节
[ area_coords] area_coord_count * 2 字节（int16，全部热区共用一段）
[ name       ] name_len 字节
```

各段按 8 字节对齐。头部含：magic(`QLUI`)、version、header_size、
各段计数、`node_struct_size` / `style_struct_size` / `anim_struct_size` /
`keyframe_struct_size`（**ABI 校验**）、total_size、FNV-1a 32 校验和、各段偏移。

**没有图片/热区的界面，那四段是 0 字节** —— 这也正是"MCU 上砍掉图片功能
不会多付一个字节"的原因。

版本历史：v12 = `<input>`（`QLUINode` 18 → 20）；v13 = 图片资源段；
**v14 = `<area>` 热区段**（`area_count` / `area_coord_count` 与两个段偏移，
头 84 → 96 字节）。

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
   ↓ qlui_parse.c 的扁平化
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
| `transition` | ✅ 背景色/alpha/文本色/边框色/圆角/`transform`/`clip-path`（M5.5） | `box-shadow`/`delay` 报错 |
| `backdrop-filter` | ✅ `blur(1~64px)`（M5.5 ⑨，PC 优先） | 其它 filter 函数报错 |
| `@keyframes` / `animation` | ✅ 构建期编译成静态帧表（M5.5） | 关键帧里只能改背景色/渐变/alpha/文本色/边框色/圆角/平移；无 `steps()` |
| `transform` | ✅ `translate()` + `scale()`/`scaleX()`/`scaleY()` | `rotate`/`skew`/`matrix`/3d 报错 |
| `radial-gradient` | ✅ 圆心固定在元素中心（M5.5 ⑥） | `at 偏移` / 自定义半径 / stop 位置报错 |
| `clip-path` | ✅ 只有 `circle(<pct>%)` | 圆心固定元素中心；只支持一层；其它形状报错 |
| CSS 变量 / media query | ❌ | 明确不支持 |
| JS | ❌（编译期无 JS） | 交互靠 C 回调 |

> 这些差距是**刻意的**：MPU 档的目标是"设计师给的静态页面能渲染"，不是"跑任意网页"。
> 子集外的东西构建期报错并给替代写法；没有换个引擎这条路（设计文档 §7.3）。

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
qltext cc --html test/web/ui_demo.html \
                          --css  test/web/ui_demo.css --out build/ui
./build/qlblob build/ui/ui_demo.qlui
# 窗口自动更新，应用二进制没有重新编译
```

要点：

- 应用**不需要重编译**（`example_ui_blob` 的二进制没变）；
- 想做"改文件就自动更新"的应用：`example/20_live_reload/main.c` 是最小实现（`stat()` 看 mtime →
  `ql_ui_load_text` → 换树，**实测整份重编译 1 ms**，编译失败保留旧界面）；
- 重载是整份重建（几十微秒~1 ms），因为 doc 之间不能合并；
- 如果新的 `.qlui` 坏了，示例**保留旧界面**并打印原因——这条规则来自
  "明确失败优于半个正确"，`test_ui_blob` 里有对应断言。

---

## 9. 建议的阅读 / 验证顺序

1. `ctest --test-dir build -R test_web_ui --output-on-failure` 全绿；
2. 打开 `build/ui/ui_demo.c`，对照 HTML 看节点表与样式表 —— 这是最快的理解方式；
3. 读 `test/web/test_web_ui.c`：每个断言对应一条布局规则；
4. 读 `src/ui/qlui_parse.c` 的 ④ CSS 词法 → ⑥ 级联 → ⑦ 编译 `QLUIStyle` 三段；
5. 读 `src/ui/qlui.c` 的 `layout_node`。
