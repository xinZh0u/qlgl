# QLUI / HTML+CSS 编译 API 手册（v0.1）

配套阅读：[QLUI 阅读指南](./QLUI阅读指南.md) · 定位：[Web 兼容设计](../../Web兼容设计.md) §7

> **先明确适用场景**
> - **MCU**：本模块就是唯一路径。想省 Flash 就裁能力包（`QL_FEAT_UI_*`），
>   或者干脆用构建期编译出来的静态 doc（见 [QLUI 阅读指南](./QLUI阅读指南.md) 的 blob 一节）。
> - **MPU / 嵌入式 Linux**：用本模块，把 HTML+CSS 编译成 C。
> - **要跑任意网页**：本模块不做，也不提供回退方案 —— 任何平台都自己绘制，不嵌 WebView。

---

## 1. 工作流

```
index.html ┐
index.css  ├─ qltext cc ─▶ ui_index.c + ui_index.h ─▶ 编译进固件
tools/qltext/qltext.c ┘
```

### 1.1 命令行

```bash
qltext cc \
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
| `--root` | 图片资源根目录（给了才把 `<img src>` 在构建期解码并嵌入） |
| `--pixel-format` | `rgb565` / `rgb888` / `rgba8888` / `a8`（构建期转码的目标格式） |
| `--platform` | 只对 `check` 有效：`pc` / `mcu`，给出**构建期降级报告**（见下） |

退出码：`0` 成功；`1` 有错误（错误信息带 `文件:行号`）；`2` 参数错误；
带 `--platform` 且发现了降级项时是 `3`。

#### 1.1.1 `qltext check --platform`：构建前就知道 MCU 上会报什么错

本库对"这个构建没启用的能力"是**构建期报错**，所以同一份 HTML/CSS 在 PC 上能编、
在 MCU 上编不过。`--platform` 把这件事提前到开发机上：

```bash
qltext check --html ui/index.html --css ui/index.css --platform mcu
#   [裁掉] 表格（display: table 全家）            3 处 → QL_FEAT_CSS_TABLE=0 时构建期报错
#   [裁掉] 滤镜 / 混合模式 / mask / 旋转（离屏图层）   1 处 → QL_FEAT_CSS_FILTER_LAYER=0 时构建期报错
# 退出码 3
```

- `pc` / `mobile` 预设 = 能力全开；`mcu` 预设 = `FILTER_LAYER` / `TABLE` / `COLUMNS` /
  `FONT_STACK` / `TEXT_SHAPE` / `BIDI` 关（与 `test/mcu/build.sh` 一致，**Grid 不裁**：
  实测它反而降低 MCU 布局时间）。
- 没有降级项时退出码 `0` 并打印 `✔ 这份界面在 <平台> 上不需要裁任何特性`。
- 代价 0：这张表只在宿主工具里，目标机根本不链接 `qltext`。

### 1.2 CMake 集成

```cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
add_custom_command(
    OUTPUT  ${UI_OUT}/ui_index.c ${UI_OUT}/ui_index.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
    COMMAND qltext cc
            --html ui/index.html --css ui/index.css --out ${UI_OUT}
    DEPENDS ui/index.html ui/index.css qltext
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
| `input` | `display:inline-block`，带内边距/边框/圆角；`type` 只支持 `text` / `password` |
| `select` / `option` | 与 `input` 同形 / **`display:none`**（只活在面板里） |

目前认 **101 个**（HTML5 共 114 个；其中 13 个是占位盒）。全量清单、每个标签的状态与能力包见
[HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html)；UA 默认样式细节见
[阅读指南 §5.4.7](QLUI阅读指南.html)。

**限制**：
- 文档必须**有且只有一个根元素**；
- 元素不能同时包含文本和子元素（混合内容）→ 报错，请把文本包进 `<span>`；
- `table` 全家 / `ruby` `rt` `rp` / 任意未实现的 `<input type>` → 报错，错误信息给出替代做法；

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
| `font-size` | `Npx` `smaller` `larger` | 任意整数；生成期汇总成 `font_sizes` 表，运行期按字号取字体。`smaller` = 父级 ×5/6、`larger` = 父级 ×6/5（相对父级，所以标题里的上标也协调） |
| `font-family` | `monospace`（含 `mono` 的候选名）、其他一律正文 | 只有**两级**：`QL_UI_FAMILY_DEFAULT` / `QL_UI_FAMILY_MONO`。写 `Arial, sans-serif` 不报错，落到正文 |
| `white-space` | `nowrap`（**默认**）`normal` `pre` `pre-wrap` `pre-line` | 见 §3.2.1 |
| `vertical-align` | `baseline`（默认）`sub` `super` | 基线偏移 = 自身字号 × ±(33%/20%)；**不改行盒高度**。`middle`/`top`/`bottom` 明确报错 |
| `text-align` | `left` `center` `right` | |
| `box-sizing` | `content-box` `border-box` | 默认 `content-box`（与浏览器一致） |
| `flex` / `flex-grow` / `flex-shrink` / `flex-basis` | `1` / `1 0 auto` / `0 0 100px` | 见 §3.8 |
| `flex-direction` | `row` `row-reverse` `column` `column-reverse` | |
| `flex-wrap` | `nowrap` `wrap` | |
| `justify-content` | `flex-start` `center` `flex-end` `space-between` `space-around` `space-evenly` | |
| `align-items` | `stretch` `flex-start` `center` `flex-end` | |
| `gap` / `row-gap` / `column-gap` | `Npx`（1~2 值） | 不支持百分比 |
| `position` | `static` `relative` `absolute` `fixed` | 见 §3.8 |
| `top` / `right` / `bottom` / `left` | `Npx` `N%` `auto` | 仅非 static 时生效 |
| `z-index` | 整数（int16） | 同级兄弟间排序 |
| `overflow` | `visible` `hidden` `auto`（`scroll` 视同 `auto`） | `hidden`/`auto` 都裁掉溢出 |

#### 3.2.1 `white-space` 与空白折叠的时机

`white-space` 既管"折不折行"，也管"空白留不留"：

| 值 | 空白 | 换行 `\n` | 自动折行 |
|---|---|---|---|
| `nowrap`（**默认**，与浏览器不同） | 折叠 | 保留 | 不折 |
| `normal` | 折叠 | 保留 | 折 |
| `pre` | 保留 | 保留 | 不折 |
| `pre-wrap` | 保留 | 保留 | 折 |
| `pre-line` | 折叠 | 保留 | 折 |

**折叠发生在建表期，不是解析期。** 原因：`white-space` 是级联之后才知道的，而折叠要
改文本字节。所以原始空白一路留到样式算好之后再折叠 —— 这样 `<pre>` 标签和
`div { white-space: pre }` 两条路都能生效。对非 `pre*` 的页面结果与旧版本逐字节相同。

`\t` 在保留模式下展开成空格（制表位 8 列，按**列数**而不是像素宽），
否则制表符落到字体查不到的字形上、宽度 0，整块缩进消失。

`<pre>` / `<textarea>` 起始标签后紧接的**一个** `\n` 会被丢掉（HTML 的
"ignore the first newline" 规则）。

**已知限制**：`pre*` 的**行首空格**只在 `<pre>` / `<textarea>` 元素上保留 ——
给普通 `div` 写 `white-space: pre` 时，行首空白已经在解析期作为"元素内容两端"
被 strip 掉了（strip 是结构性的，必须在解析期做）。

### 3.3 颜色写法

`#rgb`、`#rrggbb`、`rgb(r,g,b)`、`rgba(r,g,b,a)`、命名色（`black`/`white`/`red`/`green`/`lime`/`blue`/`gray`/`silver`/`yellow`/`orange`/`purple`/`navy`/`teal`/`transparent`）。

### 3.3.1 CSS 变量（`--x` / `var()`）

```css
#root {
  --brand: #0969DA;
  --line: #E4E8ED;
  --radius: 10px;
  --pad: 14px;
}
.card   { background-color: #FFFFFF; border: 1px solid var(--line);
          border-radius: var(--radius); padding: var(--pad); }
.title  { color: var(--brand); }
.btn    { background-color: var(--brand); color: #FFFFFF; }

/* 局部换肤：只影响这棵子树，兄弟不受影响 */
.promo  { --brand: #8250DF; }          /* 里面的 .btn / .title 自动变紫 */
```

| 项 | 支持 |
|---|---|
| `--x: <任意值>` | ✅ 定义（会**继承**，后代可覆盖） |
| `var(--x)` | ✅ 构建期替换成字面量 |
| `var(--x, <fallback>)` | ✅ fallback 里可再套 `var()` |
| 嵌套 `--a: var(--b)` | ✅ |
| 简写里用（`border: 1px solid var(--c)`、`margin: var(--g)`、`background: var(--bg)`） | ✅ 简写先展开成长属性，再替换 |
| 找不到且无 fallback / 循环引用 | ❌ **构建期报错** |
| `content: var(--x)` | ❌ 报错（伪元素在级联之前展开） |
| 运行期改变量（换肤） | ❌ 没有这条路 —— 换主题请换一份 CSS 或走热重载 |

**它是怎么做到的**：替换发生在**级联之后、属性解析之前** ——

```
UA 默认 + 级联 + 行内 + 继承  →  out
out 里先解析 `--x` 自己的值     →  再把所有 var() 换成字面量
```

所以 `QLUIStyle`、blob 格式、渲染路径**一个字节都没变**；`compile_style()` 根本不知道
有变量这回事。代价只有一条：自定义属性名按**小写**归一（CSS 是大小写敏感的）。

> 换肤的推荐写法：调色板只写一次，规则里只引用变量。配合
> [`example/20_live_reload`](../../../example/20_live_reload) 的热重载，
> 改一行 `--brand` 保存，整个界面立刻换色。

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

**与 `::before` / `::after` 的关系**：伪元素是构建期造出来的**真节点**，
它排在这个匿名文本盒的**前面**（`::before`）或**后面**（`::after`），
所以 `.x::before { content: "• " }` 的圆点一定在正文之前、不重叠。
这个匿名文本盒**不是元素**：它不参与 `:first-child` / `:nth-child` /
`+` / `~` 的匹配与计数（与"文本留在宿主节点上"时完全一致）。

### 3.4.1 文本细节（字距 / 缩进 / 大小写 / 省略号 / 断词）

```css
.title  { letter-spacing: 1px; }
.para   { text-indent: 2em; white-space: normal; }    /* 中文首行缩进 */
.badge  { text-transform: uppercase; }
.name   { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.url    { word-break: break-all; white-space: normal; }
```

| 属性 | 允许值 | 落在哪 |
|---|---|---|
| `letter-spacing` | px / em / rem（可为负） | 加在**每个字符之后**，含最后一个；宽度与绘制一起变 |
| `text-indent` | px / em / rem | 只作用在**整段第一行**：可用宽度减一截，绘制起点右移 |
| `text-transform` | `none` / `uppercase` / `lowercase` / `capitalize` | **建表期**改文本（与空白折叠同一处） |
| `text-overflow` | `clip` / `ellipsis` | 一行比盒子宽时末尾换 `…` |
| `word-break` | `normal` / `break-all` / `keep-all`（= normal） | 折行时允许在任意字符之间断 |
| `overflow-wrap` | `normal` / `break-word` / `anywhere` | 同上（`anywhere` = 允许断） |

四条要知道的行为：

1. **布局与绘制共用一份折行**（`ql_text_wrap_opt`）：`QLUIStyle` 里那几项由
   `wrap_opt_of()` 取出，两边各传一份 —— 这是"数出来 2 行、画成 3 行"这类
   bug 的唯一防线。
2. **`text-transform` 不改运行期的值**：输入框 / `<textarea>` / `<select>` 跳过它
   （那里的文字是用户输入的值，改了会和 `ql_ui_input_value()` 对不上）。
3. **`text-overflow: ellipsis` 不需要 `overflow: hidden`**：本库的语义是"少画几个字，
   末尾补省略号"，既不裁也不溢出。字体里没有 `U+2026` 时退回三个点（内置位图字体就是）。
4. **输入框不吃字距与缩进**：光标位置按 `ql_font_line_width` 算，多一个字距就会和
   字形错位。要字距就别用 `<input>`，用静态文本 + 自绘光标。

> 省略号的经典写法（真实界面里最常见的一处）：
>
> ```html
> <div class="row"><div class="name">很长的名字…</div><div class="meta">12 KB</div></div>
> ```
>
> ```css
> .row  { display: flex; gap: 6px; }
> .name { flex-grow: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
> .meta { width: 40px; text-align: right; }
> ```
>
> 左边自动让位、右边固定宽 —— 超过的部分变 `…`，不会顶翻右边的数字。

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

> **径向渐变**：`background: radial-gradient(circle, <起色>, <止色>)`。
> 圆心固定为元素中心，按到**最远角**的距离归一化（等价 CSS `farthest-corner`）。
> 只支持 `circle` / `circle at center` / `ellipse`；写 `at 偏移`、自定义半径、
> color stop 位置一律报错。UI 里的光晕、暗角就是它。

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
| `inset`、两层阴影、`spread`（第 4 个长度，B9） | ✅ |
| 三层及以上 | ❌ 构建期报错（多写的报错，不静默丢弃） |

实现是 **SDF + smoothstep 衰减**，与 GPU 着色器同一公式，因此两条后端视觉一致。
只有"距轮廓 blur 以内"的环需要逐像素算距离，内部实心区走行填充——
所以在 PC 上几乎无感，在 MCU 上也可用（见开发规划 M5.5 的能力矩阵）。

### 3.6.1 `@media` 断点

```css
.card { width: 200px; }
@media (max-width: 620px) {
  .card { width: 100%; }            /* 窄屏：撑满 */
}
@media (min-width: 900px) and (max-width: 1200px) {
  #root { font-size: 13px; }
}
```

| 项 | 支持 |
|---|---|
| `(min-width: Npx)` / `(max-width: Npx)`，可用 `and` 串联 | ✅ |
| 媒体类型 `screen` / `all` | ✅（可省略） |
| `print` | 整块跳过（永不生效） |
| `(orientation: …)` / `(prefers-color-scheme: …)` / `em` 单位 | ❌ 构建期报错 |

**它是怎么做到的**：条件在**构建期**被切成互不相交的视口宽区间，每个区间编译一套样式；
运行期 `ql_ui_layout(ui, w, h)` 只按 `w` 选区间下标（线性扫一个 `QL_CFG_UI_MAX_MEDIA`
以内的小数组），**没有任何条件求值**。所以：

- 拖窗口缩放会立刻走另一套样式（重排是必然的）；
- 每个区间都有一套 `:hover` / `:active` / `:focus` 变体，悬停不会把响应式尺寸弄丢；
- 区间数上限 `QL_CFG_UI_MAX_MEDIA`（默认 6）；断点写得太碎会报错。

> 一个实践提醒：flex 子项请写 `box-sizing: border-box`。content-box 时主轴尺寸会
> 少算 padding + border（见 [CSS3 覆盖与裁剪](CSS3覆盖与裁剪.html) §19.1）。

### 3.6.2 `calc()` / `min()` / `max()` / `clamp()`

```css
.side  { width: calc(100% - 260px); }        /* 主区：右边留 260px 给侧栏 */
.col   { width: calc((100% - 16px) / 3); }   /* 三等分（括号一起除） */
.box   { width: min(160px, 30%); }           /* 取小 */
.panel { width: clamp(240px, 40vw, 480px); } /* 有上下限的伸缩 */
.mid   { position: absolute; left: calc(50% - 90px); }   /* 绝对定位居中 */
```

| 项 | 支持 |
|---|---|
| `+` `-` `*` `/`、括号、嵌套 `calc()` | ✅ |
| `min()` / `max()`（2..8 个参数）、`clamp(lo, val, hi)` | ✅ 参数可以是长度表达式 |
| 单位 | `px` / `%` / `em` / `rem` / `vw` / `vh` / `vmin` / `vmax` |
| 用在 | `width` / `height` / `min-*` / `max-*` / `top` / `right` / `bottom` / `left` |
| `font-size` / `border-*` / `letter-spacing` / `text-indent` / `margin` / `padding` | 只接受**构建期能折成 px** 的 `calc()`（如 `calc(10px + 6px)`）；带 `%`/`em`/`vw` 报错 |
| 长度相乘 / 除以 0 / 括号不配对 / 未知单位 | ❌ 构建期报错 |

**它是怎么做到的**：公式里可能有 `%` / `vw` / `vh`，基准只有布局时才知道，所以构建期
把公式编译成一段**后缀字节码**存进文档（`doc->calc_ops[]`，每条 4 字节），样式字段只留
一个 16 位下标（`QLUILength.unit == QL_UI_UNIT_CALC`）；布局时用一个 16 深的栈求值，
**没有递归、没有分配**。

- 标量乘除在构建期折进每个操作数（`calc(2 * 10px)` → `20px`，不占池）；
- 百分比按整数存，所以 `calc(100% / 3)` 是 33%（不是 33.33%）；
- 池上限 `QL_CFG_UI_MAX_CALC_OPS`（默认 256 条，**设成 0 就整套裁掉**，写 `calc()` 报错）；
- 没用 `calc()` 的文档，这一段的长度为 0，blob 与以前一样紧凑（blob v22）。

```c
/* 宿主想知道一个 doc 用了多少字节码：直接读字段即可 */
printf("calc 指令 %u 条\n", (unsigned)doc->calc_op_count);
```

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
| 交叉轴对齐 | `stretch`（默认，子项无显式交叉尺寸时拉满）或 start/center/end；**单行时行会撑满容器**，所以 center/end 真的居中 |
| 单个子项覆盖 | `align-self: auto \| flex-start \| center \| flex-end \| stretch` |
| 多行的行间分配 | `align-content: stretch（默认）\| flex-start \| center \| flex-end \| space-between \| space-around \| space-evenly` |
| 顺序 | `order: <整数>`（升序，相等时保持 HTML 顺序） |
| 简写 | `flex-flow: <direction> \|\| <wrap>` |
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

**不支持的**：`baseline` 对齐、`wrap-reverse`、
`flex-basis: auto` 的完整语义（等同"用 width"）。

### 3.7.1 `visibility` / `pointer-events` / `cursor` / `user-select`

```css
.placeholder { visibility: hidden; }   /* 看不见，但位置还占着（display:none 才不占） */
.tooltip     { visibility: hidden; }
.card:hover .tooltip { visibility: visible; }   /* 子级能自己显示回来（继承属性） */

.overlay { pointer-events: none; }     /* 装饰层：自己不挡点击 */
.overlay .btn { pointer-events: auto; }/* 里面的按钮照常可点 */
```

| 属性 | 取值 | 行为 |
|---|---|---|
| `visibility` | `visible` / `hidden`（`collapse` 按 hidden） | 只影响"画不画自己"与命中；布局照常。**继承**：父级 hidden 时子级也 hidden，子级可写 `visible` 显示回来 |
| `pointer-events` | `auto` / `none` | `none` = 自己不作为命中目标；**子节点照常**（可写 `auto` 重新接住） |
| `cursor` | `auto` / `default` / `pointer` / `text` / `move` / `not-allowed` / `help` / `progress` | 只算**意图**，见下面 `ql_ui_cursor_at()` |
| `user-select` | `none` / `auto` / `text` / `all` / `contain` | `none` → 输入框仍可点（光标落点不变），但拖拽不再连选 |

```c
/* 鼠标移动时问一句"现在该用哪个光标"，交给窗口系统 */
uint8_t cur = ql_ui_cursor_at(ui, mx, my);   /* QLUICursor */
/* X11: XDefineCursor(display, win, cursor_for(cur)); Wayland: wl_cursor_* */
```

`ql_ui_cursor_at()` 从命中节点往上找第一个写了 `cursor` 的祖先；都没写时输入框给
`text`、其余给 `default`。**它只算意图，不设光标** —— 设光标要窗口系统配合，
后端目前没有接（写在这里而不是让它悄悄失效）。

三个属性都是**逐节点的开关**，不引入新状态：`visibility` 在绘制时跳过"自己"那几笔，
`pointer-events` 在命中测试里跳过"自己"这一步，`cursor` 只是命中后往上查一层。
所以没有性能代价，也不影响 `paint_box`（`visibility: hidden` 仍然占着脏区范围）。

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
| `sticky` | **在流里占位**；贴住最近的滚动容器的可视边缘，但不能推出自己的包含块 |

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

**`inset` 简写**

```css
.overlay { position: absolute; inset: 0; }             /* 铺满包含块 */
.badge   { position: absolute; inset: 4px 8px 4px 8px; } /* 上右下左 */
```

`inset` 与 `margin` 同一套 1/2/4 值语法，构建期展开成 `top` / `right` / `bottom` / `left`
四条长写。所以"展开后能写的东西"它都能写（`px` / `%` / `em` / `rem` / `auto`）。

**`position: sticky`**

```css
.list { height: 240px; overflow-y: auto; }
.head { position: sticky; top: 0; }     /* 列表滚动时表头钉在顶部 */
```

实现是**布局末尾的一次补偿平移**（`sticky_adjust`）：滚动已经由 `apply_scroll`
把子树整体平移过，之后 sticky 再把自己推回可视边缘。改的是 `border_box`，
所以绘制、命中测试、脏区、`paint_box` 全都自动正确 —— 没有第二套坐标。

三条约束：

| 约束 | 说明 |
|---|---|
| 只贴**给了 inset 的方向** | 不给 `top` 的 sticky 与 `relative` 等价（与 CSS 一致） |
| 参照物是**最近的滚动容器** | 没有滚动容器时按视口 |
| 不能推出**包含块** | 父内容盒的边界；这就是"跟着列表滚到头就一起走" |

**z-index**：布局结束后对所有兄弟按 `z-index` **稳定排序**子链表，
之后**绘制顺序与命中测试顺序自动一致**（链表顺序即绘制顺序）。
z-index 相同则保持文档顺序（稳定）。

**overflow**

| 值 | 行为 |
|---|---|
| `visible`（默认） | 子项可以画到父盒子外 |
| `hidden` / `auto` / `scroll` | 子树被裁到本节点边框盒；**被裁掉的部分不可命中**（命中测试同样裁剪） |
| `overflow-x` / `overflow-y` | 分轴：只裁那个方向。另一轴沿用外层裁剪，所以"只裁横向"的容器里子项仍可以纵向画出去 |
| `auto` 的轴 | 才有可滚范围、才画滚动条（另一轴固定不可滚） |

```css
.ticker { overflow-x: hidden; overflow-y: visible; }   /* 横向裁、纵向放行 */
.list   { overflow-x: hidden; overflow-y: auto; }      /* 只纵向滚 */
```

> `auto`/`scroll` 目前**不产生滚动条，也不能滚动** —— 只是裁剪。
> 滚动容器排在 M3 之后的滚动里程碑。

### 3.9 过渡（`transition`）

```css
.btn       { transition: 180ms ease-out; }                 /* 简写：全部可过渡属性 */
.btn       { background-color: #204060; }
.btn:hover { background-color: #ff8000; }                  /* 180ms 内渐变过去 */
.card      { transition: background-color 300ms ease-in-out, color 300ms ease-in-out; }
```

| 部分 | 允许值 | 默认 |
|---|---|---|
| `transition-property` | `background-color` / `opacity` / `color` / `border-color` / `border-radius` / `transform` / `clip-path` / `all`；**可以列多个**（逗号或空格分隔） | `all` |
| `transition-duration` | `Nms` 或 `Ns` | `0s`（等于不写） |
| `transition-timing-function` | `linear` `ease` `ease-in` `ease-out` `ease-in-out` `steps(n)` `cubic-bezier(a,b,c,d)` | `ease` |
| `transition-delay` | ❌ 报错 | — |

行为与浏览器一致的三条：

1. **打断从当前插值重基**，不回跳、不从头开始。
2. 过渡只改**绘制期属性**，不进布局 → 过渡期间**零重排**。
3. 只有写进 CSS 的规则才产生动画；`transition` 可以写在 base 规则上（对所有状态生效）。

**支持**：`background-color`（渐变时连同终止色）、`opacity`、`color`、`border-color`、
`border-radius`、`transform`、`clip-path`。`transition: all` 七个一起开。

`transform` 的四个分量（`tf_x` / `tf_y` / `sc_x` / `sc_y`）**永远一起插值**——
只动 translate 不动 scale 会出现"边缩边移"的错位。所以放大 + 位移写一条就够了：

```css
.card        { transition: transform 200ms ease-out; }
.card:hover  { transform: translate(4px, 2px) scale(1.5); }
.mask        { clip-path: circle(20%); transition: clip-path 300ms ease-out; }
.mask:hover  { clip-path: circle(100%); }
```

**不支持**：`transition-delay`、`box-shadow`（构建期报错，不静默降级）。

> **逗号写法只允许"共用一组参数"**：`trans_ms` / `trans_ease` 各只有一份，
> 所以 `transition: a 200ms, b 200ms` 可以，`transition: a 200ms, b 400ms`
> 会明确报错（不同时长请拆成两层元素）。这一条和浏览器不同，是数据模型决定的。

> **`transition: opacity` 与 `opacity` 不是同一件事**：`transition: opacity` 插值的是
> **背景 alpha**（`background-color` 的 a 通道，关键帧 `opacity` 也落到这里）；
> 而普通规则里的 `opacity: 0.5` 是**整体不透明度**（见下面 3.11.2），两者互不干扰。
> 早先的实现只有前者，所以 `opacity: 0` 等价于"背景全透明、子元素不受影响"—— 现在不是了。
> 写过渡时请用 `background-color: rgba(...)` 表达淡入淡出。

运行期推进（主循环里）：

```c
uint32_t dirty_count = ql_ui_anim_tick(ui, ql_anim_now_ms());
if (dirty_count > 0) {
    QLRect areas[8];
    int n = ql_ui_consume_dirty(ui, areas, 8);
    for (int i = 0; i < n; i++)
        ql_ui_render_rect(ui, &dw, areas[i]);   /* 只重绘变化区域 */
}
```

> `ql_ui_node_style()` 返回的是**过渡中的样式**（可能是中间色）。
> 需要"最终值"时请等动画结束，或读 CSS 源。

---

### 3.9.1 圆形揭示（`clip-path: circle()`）

```css
.reveal { animation: iris 400ms ease-out 1 forwards; }
@keyframes iris {
  from { clip-path: circle(0%); }
  to   { clip-path: circle(100%); }
}
```

| 写法 | 支持 |
|---|---|
| `circle(<百分比>)` | ✅ 0%~100% |
| `none` | ✅ |
| `circle(<N>px)` / `inset()` / `polygon()` / `path()` / `url()` | ❌ 构建期报错 |

语义：

- 圆心固定在**元素中心**（与 `radial-gradient` 同一个决定）。
- 百分比是**最远角距离**的千分比：`0%` 什么都不显示，`100%` 完全覆盖。
  用百分比而不是 px，是因为揭示动画要"全程都在动"——100px 的半径在 200px 的盒子上
  会在动画前半段就把元素盖满。
- 它裁的是**整棵子树**（等价 CSS 的语义）：伸出父盒子的子元素也会被切掉。
  这跟 `border-radius` 动画的区别就在这里——后者只切父元素自己的背景/边框。
- **只支持一层**。嵌套 `clip-path` 的 CSS 语义是取交集，而两个圆盘的交集不是圆盘，
  所以运行期保留外层并打一条警告。

### 3.10 变换（`transform`）

```css
.slide-in { transform: translate(-40px, 0); }   /* 静态平移 */
.icon     { transform: translateX(6px); }
```

| 写法 | 支持 |
|---|---|
| `translate(Xpx, Ypx)` / `translate(Xpx)` | ✅ |
| `translateX(Xpx)` / `translateY(Ypx)` | ✅ |
| `scale(<倍数>)` | ✅ 绕元素中心 |
| `scale(sx)` / `scale(sx, sy)` | ✅ 轴对齐的非等比（见下） |
| `scaleX(s)` / `scaleY(s)` | ✅ **假 3D 翻转**就靠这个 |
| `rotate(<deg>)` | ✅ 顺时针为正；**要离屏图层**（`QL_FEAT_CSS_FILTER_LAYER`） |
| `skew(<ax>[, <ay>])` / `skewX()` / `skewY()` | ✅ 同上 |
| `matrix(a,b,c,d,e,f)` | ✅ 同上（六参） |
| `transform-origin` | ✅ `px` / 千分比 / `left|center|right|top|bottom`，默认中心 |
| `none` | ✅ |
| `perspective()` / `rotate3d()` / `matrix3d()` | ❌ **构建期报错**（3D 不排期） |

可以组合：`transform: translate(4px, 6px) scale(2)`。

**`rotate` / `skew` / `matrix` 与上面两个的画法不同**：它们是"**先合成再重采样**"，
所以会开一层离屏图层（RAM = 元素盒大小的 RGBA 缓冲）。一整串 `transform` 只要
出现这三个之一，整串就乘成**一个**矩阵：

```css
.tilt { transform: translate(10px, 4px) rotate(12deg) scale(1.1); }
```

`@keyframes` 里**不能**写 `rotate()`/`skew()`/`matrix()`（关键帧只存
`translate`/`scale` 的整数值），写了报错。把 `QL_FEAT_CSS_FILTER_LAYER` 裁掉时，
这三个函数同样在**构建期报错**（不静默降级）。

**`scale()` 的三条行为**：

1. **不进布局**：`flex` / `border_box` 看不到它，邻居不会被挤开（与 CSS 一致）。
2. **文字一起放大**：内部按"CSS 字号 × scale × DPR"重新取字号，所以是清晰的放大，
   不是把小图拉大。
3. **命中测试跟着走**：视觉上占多大就能点多大（压扁时被压掉的部分点不中）。

**非等比缩放（`scaleX`/`scaleY`）的近似**：圆角、描边、阴影、字号都按
`min(sx, sy)` 缩放，**字形不做横向挤压**。所以 `scaleX(0.7)` 的文字高度也会变成 0.7。
`0` 是合法值（`scaleX(0)` = 零宽），此时一个字都不画、也点不中。做翻转就用它：

```css
@keyframes flip-x {
  0%   { transform: scaleX(1); }
  50%  { transform: scaleX(0); }
  100% { transform: scaleX(1); }
}
.coin { animation: flip-x 900ms ease-in-out infinite; }
```

`backface-visibility` **报错**：假翻转没有背面，没有"背面朝后就不画"这回事。
要翻面换成另一张脸，用两个元素错开 `animation-delay`。

三条语义（与 CSS 一致）：

1. **不影响布局**：`transform` 是绘制期属性，`flex` 计算里看不到它，所以它是
   "移动元素但不触发重排"的唯一手段。
2. **作用于整个子树**：父元素平移，子元素跟着走。
3. **命中测试跟着变**：视觉位置就是可点位置，动画过程中也点得中。

`transform` 也可以在 `@keyframes` 里插值（见 §3.11），这是做"滑入"最省事的办法：

```css
@keyframes slidein {
  from { transform: translate(-70px, 0); opacity: 0; }
  to   { transform: translate(0, 0);     opacity: 1; }
}
.card { animation: slidein 400ms ease-out 1 forwards; }
```

> **代价（诚实说明）**：因为不进布局，被平移的元素需要把它"可能出现的所有位置"算进
> 脏区上界——动画的位移区间越大，脏区就越大（最多大出整个位移量）。
> 这是"零重排"换来的，正常入场动效（几十像素）完全无感。

---

### 3.11 关键帧动画（`@keyframes` + `animation`）

```css
.pulse { animation: pulse 1s linear infinite alternate; }
.card  { animation-name: fade; animation-duration: 400ms;
         animation-fill-mode: forwards; }

@keyframes pulse {
  from { background-color: #204060; }
  to   { background-color: #ff8000; }
}
@keyframes fade {
  from { opacity: 0; }
  to   { opacity: 1; }
}
```

`@keyframes` 在**构建期**编译成静态帧表（`QLUIKeyframe`）+ 段表（`QLUIAnimation`），
运行期零解析。所以 `animation-name` 必须是同一份 CSS 里定义过的名字。

| 部分 | 允许值 | 默认 |
|---|---|---|
| `animation-name` | 本文件里定义过的 `@keyframes` 名 / `none` | `none` |
| `animation-duration` | `Nms` / `Ns`，**必须 > 0** | 必填 |
| `animation-timing-function` | `linear` `ease` `ease-in` `ease-out` `ease-in-out` | `ease` |
| `animation-iteration-count` | 整数 1~255 / `infinite` | `1` |
| `animation-direction` | `normal` `reverse` `alternate` `alternate-reverse` | `normal` |
| `animation-fill-mode` | `none` `forwards` `backwards` `both` | `none` |
| `animation-delay` | `Nms` / `Ns`（0~65535） | `0ms` |
| `animation-play-state: paused` | ❌ 报错 | — |

**关键帧里能写什么**（只允许能插值的绘制期属性）：

| 属性 | 含义 |
|---|---|
| `background-color` / `background` | 背景色（含 `rgba()` 的 alpha） |
| `background-image: gradient(...)` | 渐变的首末色，两个颜色一起插值 |
| `opacity` | **背景 alpha**（与 `transition` 同义，见 §3.9 的说明） |
| `transform: translate(...)` | 平移（见 §3.10），和颜色一起插值 |
| `color` | 文本颜色 |
| `border-color` | 边框颜色 |
| `border-radius` | 圆角半径（只切元素自身，做圆角/圆形揭示） |
| `clip-path: circle(<pct>%)` | 圆形遮罩，**裁整棵子树**（见 §3.9.1） |
| `animation-timing-function` | 这一帧到下一帧的缓动 |

其它属性（`box-shadow` / `font-size` / `filter` …）在构建期**报错**。

三条必须知道的行为：

1. **各帧的属性集合必须一致**。`0% {background-color} 50% {opacity}` 会被拒绝——
   否则运行期要额外记录"隐式值"，容易做出半正确的效果。
2. **只写一半的关键帧是合法的**：`@keyframes x { 50% { opacity: 0 } }` 的首末帧
   自动取**元素自己的基础样式**（与 CSS 的隐式关键帧一致），所以同一个 `@keyframes`
   可以被不同底色的元素复用。
3. **`animation-delay` 期间**：`fill-mode` 带 `backwards`/`both` 时显示 0% 帧，
   否则显示元素的基础样式（即"还没开始动"）。`animation` 简写里的第二个时长就是
   delay（`animation: pulse 1s 300ms`），写第三个时长报错。

**动画与 `transition` 同时存在时，动画赢**：动画已经占用了颜色通道，
状态过渡在这个节点上直接跳过（不会互相打架）。

**`animation` 只能写在基础规则上**：动画在 `ql_ui_build` 时按基础样式起播，
写在 `:hover` 之类里永远不会触发，所以构建期直接报错（不静默失效）。

运行期接口：

```c
/* 从头重播（点击反馈、出错抖动）；节点没有动画时返回 QL_ERR_INVAL */
ql_err_t ql_ui_anim_restart(QLUIRoot *ui, QLUINodeRT *node);

/* 正在跑的动画数（过渡 + 未播完的关键帧动画） */
uint32_t ql_ui_anim_running(const QLUIRoot *ui);
```

### `animationend` / `transitionend`

```c
typedef enum {
    QL_UI_ANIM_END_KEYFRAMES = 0,   /* @keyframes 播完 */
    QL_UI_ANIM_END_TRANSITION,      /* transition 走到终点 */
} QLUIAnimEndKind;

typedef void (*ql_ui_anim_end_cb)(QLUIRoot *ui, QLUINodeRT *node,
                                  QLUIAnimEndKind kind, void *user_data);

void ql_ui_set_anim_end_cb(QLUIRoot *ui, ql_ui_anim_end_cb cb, void *user_data);
```

典型用法——淡出后删节点：

```c
static void on_anim_end(QLUIRoot *ui, QLUINodeRT *node, QLUIAnimEndKind kind, void *user)
{
    if (kind != QL_UI_ANIM_END_TRANSITION) return;
    if (node == (QLUINodeRT *)user) {
        /* 这里已经安全：事件是 tick 末尾派发的，槽位早就释放了 */
        ql_ui_set_text(ui, node, "");
    }
}
```

| 会触发 | 不会触发 |
|---|---|
| `@keyframes` 播完（非 infinite） | `animation-iteration-count: infinite` 的每一轮之间 |
| `transition` 走到终点 | 被 `ql_ui_anim_restart()` 打断（那是新的开始） |
| | 状态变化后目标样式没有过渡而直接撤销 |
| | 过渡槽满 / 时长为 0 导致"瞬间到位"（没真的播过） |

事件队列有界（`QL_CFG_UI_ANIM_EVENT_MAX`，默认 16）。一帧内超过这个数会丢事件并
告警一次——**不做无界排队**（那会在慢机器上滚雪球）。

推进与重绘仍然只用 `ql_ui_anim_tick` / `ql_ui_consume_dirty`（见 §3.9）。
`animation-iteration-count: infinite` 的动画永远让 `ql_ui_anim_tick` 返回非 0，
所以**别用"tick 返回 0"来判断能不能退出**，那不是它的语义。

---

### 3.11.1 毛玻璃（`backdrop-filter: blur()`）

```css
.glass {
  background-color: rgba(255, 255, 255, 0.10);   /* 想看见效果就得带 alpha */
  border-radius: 14px;
  backdrop-filter: blur(12px);
}
```

| 允许值 | 说明 |
|---|---|
| `blur(Npx)`，N = 1~64 | 高斯标准差（CSS 语义）；N 越大越糊 |
| `none` | 关闭 |

它模糊的是**这层元素背后已经画好的内容**，所以：

1. **必须写在"上面那一层"**：靠 z 序/文档顺序决定谁是"背后"；
2. **自己的 `background-color` 要带 alpha**，否则糊完又被不透明底色盖住，看不出效果；
3. **只影响自己的矩形**（含 `border-radius` 的圆角），不会把外面的颜色吸进来，
   也不会把模糊漏到圆角缺口外面（见下面的"圆角裁剪"）；
4. 它跟着 `transform: translate()` 一起走——毛玻璃卡片滑动时，糊的是它**现在**
   盖住的那块内容（两条后端都在采样矩形里补了绘制期平移；早先漏了这一步，
   表现是"移动后的玻璃糊的是它原来那块地方"）；
5. **下层在动就跟着动**：模糊的是"这一帧刚画好的像素"，所以 `@keyframes` 驱动的
   背景变化会被实时反映，不需要额外机制（例程 `15_backdrop` 演示这一点）；
6. 每帧多一次 draw call；CPU 后端代价约等于"半次整屏重绘"（见 `bench.md`）。

**圆角裁剪**：模糊是方框运算，元素却常常是圆角的。不做处理时圆角缺口里会渗出
"方的模糊块"（颜色跑到边框盒之外）。上层会自动把 `border-radius` 传给
`ql_draw2d_backdrop_blur_round()`，CPU 侧只重算四个角落像素、GPU 侧复用
`sdRoundBox` 的 SDF 压掉片元 alpha，两条后端语义一致。

**GPU 侧的纹理复用**：模糊用的中间纹理是"只增不减"复用的 —— 一块大玻璃用完，
下一个更小的玻璃会复用同一张更大的纹理，有效内容只在左上角。所以回画的四边形
必须只取那一块（`uv` 按有效区域缩放），混响的抽头也要夹在区域内，否则小玻璃里
会出现"上一块大玻璃被压扁"的内容。这条有专门的单测（`test_blur` 第 5 节）。

**脏区的边界**：脏区里只要有一块与玻璃元素相交，玻璃就会重新糊一次。但如果下层
的变化只覆盖玻璃的一小块，就只有那一小块被重新模糊，模糊核够不到矩形外的邻居，
边缘可能出现细接缝 —— 需要绝对干净时，把带 `backdrop-filter` 的元素整体当脏区。

**构建期报错**：`blur()` 以外的函数（`blur` 以外的 `backdrop-filter` 值）、半径 0 或 > 64。

---

### 3.11.2 整体不透明度（`opacity`）

```css
.dim { opacity: 0.5; }          /* 0~1，默认 1 */
```

实现方式是**把整体 alpha 乘到每一个绘制入口**上（`QLDraw2D.alpha`），并且顺着子树往下乘：

| 会一起淡的 | 说明 |
|---|---|
| 背景（含渐变） | `bg_alpha × opacity` |
| 边框 | 描边走新加的 `ql_canvas_stroke_rect_alpha()` |
| 阴影 | `shadow[i].alpha × opacity`（两层各自乘） |
| 文字 | 字形覆盖率 × opacity（`ql_canvas_draw_glyph_alpha()`） |
| `<img>` | 位图搬运的 alpha × opacity |
| 子元素 | 相加相乘（父 0.5 × 子 0.5 = 0.25） |

`opacity: 0` 时整棵子树直接跳过（连遍历都不做）。

**有子节点时是真正的组不透明度**（M9.3 起）：整棵子树先合成到一张离屏图层，
再整体降透明，所以"子树内部互相覆盖"的地方**不会**更深。
**单节点**（没有子节点）仍走"逐个绘制入口乘 alpha"的老路径 —— 一个节点内部
没有互相覆盖这回事，两者逐像素等价，而省掉一层缓冲。

| 情形 | 走哪条 | 内存 |
|---|---|---|
| 无子节点 | 逐图元乘 alpha（`QLDraw2D.alpha`） | 0 |
| 有子节点 | 离屏图层（CPU 两遍哑光 / GPU FBO） | 元素尺寸 × 4 字节 |
| `QL_FEAT_CSS_FILTER_LAYER=0` | 恒走逐图元（旧语义） | 0 |

### 3.11.3 `filter` / `mix-blend-mode` / `mask`（离屏图层）

```css
.a { filter: blur(4px) brightness(120%); }
.b { filter: drop-shadow(2px 3px 4px rgba(0,0,0,.4)); }
.c { mix-blend-mode: multiply; }
.d { mask: url(alpha8); }
```

这三条和"组 `opacity`"共用同一块基建：**先把子树合成到一张临时图，再整体处理**。
支持范围与公式见《CSS3 覆盖与裁剪》§18.10，教程见 [离屏图层](../tutorial/20_filter.md)。
`transform: rotate()/skew()/matrix()` 也长在这块基建上（§3.10 / §18.11），
教程见 [旋转光栅](../tutorial/21_transform.md)。

| 属性 | 取值 |
|---|---|
| `filter` | `blur(<0~64>px)`、`brightness/contrast/saturate(<倍数>\|<百分比>)`、`grayscale/sepia/invert(<百分比>)`、`hue-rotate(<deg>)`、`opacity(<0~1>)`、`drop-shadow(<dx> <dy> [<blur>] [<color>])`；链长 ≤ 6，按书写顺序应用 |
| `mix-blend-mode` | `normal` / `multiply` / `screen` / `overlay` / `darken` / `lighten` / `difference` |
| `mask` / `mask-image` | `url(<资源名>)` / `none`；只用图片 alpha，拉伸到边框盒 |

**离屏图层是一块"按元素尺寸"的 RAM**，所以它单独一个编译开关：
`QL_FEAT_CSS_FILTER_LAYER`（MCU 默认关）。关掉时上面三个属性在**构建期报错**，
`opacity` 退回逐图元语义。

**构建期报错**：不是 0~1 的数字（`opacity: abc`）、越界（`opacity: 1.5`）。

---

### 3.11.4 表格（`display: table` 全家）

```css
table { width: 520px; border-collapse: collapse }
th, td { border: 1px solid #D0D7DE; padding: 5px 8px }
```

| 项 | 取值 |
|---|---|
| `display` | `table` / `inline-table`、`table-row`、`table-cell`、`table-caption`、`table-row-group`（`table-header-group` / `table-footer-group` 同义） |
| 标签 | `<table>` `<caption>` `<thead>` `<tbody>` `<tfoot>` `<tr>` `<td>` `<th>`；`<colgroup>`/`<col>` 存在但 `display: none` |
| `table-layout` | `auto`（默认：量各列 min/max-content 后按可用宽度插值）/ `fixed`（首行或 `<col>` 说了算，其余等分） |
| `border-collapse` | `separate`（默认）/ `collapse` |
| `border-spacing` | 1~2 个**非负 px**（横 [纵]） |
| `colspan` / `rowspan` 属性 | 1~255 的十进制整数（非法值按 1） |
| `<colgroup>` / `<col>` | `display: table-column-group` / `table-column`；`width` 定列宽，`span`（1~255）展开成多列。`fixed` 下优先于首行，`auto` 下是那一列的**下界** |
| `vertical-align` | 单元格里支持 `top` / `middle` / `bottom`；其它元素仍只认 `baseline` / `sub` / `super` |

**样式写在 `td` / `th` 上** —— `tr` / `thead` 不是盒子（与浏览器一致），
写在上面的边框、背景、内边距不会生效。实现细节与代价见《CSS3 覆盖与裁剪》§18.12，
教程见 [表格](../tutorial/22_table.md)。

**构建期报错**：`table-layout: <auto/fixed 之外>`、`border-collapse: <collapse/separate 之外>`、
`border-spacing` 带 `%`、`caption-side: bottom`、
`QL_FEAT_CSS_TABLE=0` 时的 `<table>` 与 `display: table*`。

---

### 3.11.5 多栏（`column-*`）

```css
.news { column-count: 3; column-gap: 16px; column-rule: 1px solid #D0D7DE }
```

| 项 | 取值 |
|---|---|
| `column-count` | `auto`（默认）/ 1~255（超过 `QL_CFG_UI_COLUMN_MAX`(8) 按上限截断） |
| `column-width` | 非负 `px` / 百分比（相对容器内容宽）/ `auto` |
| `columns` | `<栏数>` 与/或 `<栏宽>`（顺序随意，至少给一个） |
| `column-gap` | px（与 flex 的 `column-gap` 共用）；不写按 16px（CSS 的 `normal`） |
| `column-rule` / `-width` / `-style` / `-color` | 同 `border` 的取值：`1px solid #888`、`dashed`、`dotted`、`none` |

**语义收窄成"贪心分栏"**：按文档顺序把子节点塞进当前栏，塞不下就换下一栏，
**不把一个子节点劈成两栏**（容器高度 auto 时会先二分出一个"等高栏高"）。
不支持 `column-span: all`。实现细节与代价见《CSS3 覆盖与裁剪》§18.13，
教程见 [多栏](../tutorial/23_columns.md)。

**构建期报错**：`column-count: <非整数>`、负的 `column-width`、`columns: auto`、
`column-rule: <wavy 等非 solid/dashed/dotted/none>`、`column-span` 整个属性、
`QL_FEAT_CSS_COLUMNS=0` 时上述任一属性。

---

### 3.11.6 文本栈（`@font-face` / `font-family` / 整形 / 双向）

```css
@font-face { font-family: "My Sans"; src: url(/usr/share/fonts/.../DejaVuSans.ttf);
             font-weight: 400 }
@font-face { font-family: "My Sans"; src: url(...-Bold.ttf); font-weight: 700 }

.article {
  font-family: "My Sans", "Noto Sans CJK SC", sans-serif;   /* 逐字回退 */
  font-weight: 700;                                          /* 用真粗体字面 */
  font-variant-ligatures: normal;                            /* 连字（默认开） */
}
.arabic { direction: rtl; }                                  /* 段落基准方向 */
```

| 项 | 取值 |
|---|---|
| `@font-face` | `font-family` / `src: url(...)` / `font-weight`（100..900、`normal`、`bold`）/ `font-style`（`normal`/`italic`/`oblique`）。**必须是绝对路径或运行期登记过的资源名** |
| `font-family` | 逗号分隔的列表；`monospace` / `serif` / `sans-serif` / `system-ui` / `cursive` / `fantasy` 是通用族名，其余查注册表 |
| `font-weight` | 数字或 `normal`/`bold`；**有对应字面就用真字重**，没有则合成加粗 |
| `font-style` | `normal` / `italic`（`oblique` 同义）：同上，有真斜体就用 |
| `font-variant-ligatures` | `normal` / `none` / `common-ligatures` / `no-common-ligatures` |
| `font-feature-settings` | 只认 `"liga"` / `"clig"` / `"kern"` 的 0/1 开关 |
| `direction` | `ltr`（默认）/ `rtl`；`rtl` 时整行按视觉顺序排、并把 `text-align: start/end` 翻过来 |
| `unicode-bidi` | `normal` / `embed` / `isolate` / `bidi-override`（当前都按"单层"处理） |

**运行期接口**（不想用 CSS 里的路径时）：

```c
/* 把一个家族名绑到字体文件；同 (家族, 字重, 斜体) 重复注册会覆盖 */
ql_font_family_register("My Sans", "/opt/fonts/MySans.ttf", 400, 0);
/* 内存里的字体（打进了二进制或从资源包解出来） */
ql_font_family_register_mem("My Sans", bytes, size, 700, 0);

/* 整形 / 双向当前用的是哪条实现（"harfbuzz"/"fribidi" 或 "builtin"） */
printf("%s %s\n", ql_text_shape_backend(), ql_text_bidi_backend());
```

**生命周期**：字体注册表跟着**当前文档**走 —— `ql_ui_build()` 时按 doc 的
`@font-face` 重建，`ql_ui_free()` 时释放。所以手动 `ql_font_family_register()`
要放在 build 之后（或者干脆在 CSS 里写 `@font-face`）。

**构建期报错**：`@font-face` 的 `src` 不是 `url()`、写了 `unicode-range` /
`font-display` / `local()`、`font-feature-settings` 里出现 liga/clig/kern 之外的特性、
`QL_FEAT_CSS_FONT_STACK=0` 时的 `@font-face` 与家族名、
`QL_FEAT_CSS_BIDI=0` 时的 `direction: rtl`、
`QL_FEAT_CSS_TEXT_SHAPE=0` 时关连字/字距。

实现细节与体积见《CSS3 覆盖与裁剪》§18.14，教程见 [文本栈](../tutorial/24_text_stack.md)。

**环境变量（排查用）**：`QL_TEXT_SHAPE=builtin` 强制用内建整形；
`QL_TEXT_BIDI=builtin` 强制用内建 UAX#9 子集。

---

### 3.12 不支持的写法一律报错

```
float / align-content: baseline
椭圆 border-radius（`10px / 20px`）
perspective() / rotate3d() / matrix3d()（3D 不排期；rotate/skew/matrix 已支持）
column-span / column-fill / break-inside（多栏的其它属性）
filter: url(#svg) / spread              backdrop-filter 的 blur() 以外的函数
@font-face 的 unicode-range / local()   font-feature-settings 里 liga/clig/kern 之外
font-variant: small-caps / tabular-nums  unicode-bidi 的嵌套隔离
mask-position / -repeat / -composite    mix-blend-mode: hue / color / exclusion
background-blend-mode
list-style / @supports                   overscroll-behavior / scroll-snap-*
text-align: justify                     transition-delay
backface-visibility（假翻转没有背面）
```

报错示例：

```
qlui: 错误：index.css:27: 不支持的属性 'position: sticky-x'
qlui: 错误：index.css:12: 不支持的长度 '1.5ch'（支持 px / % / em / rem / vw / vh / calc()）
qlui: 错误：index.css:31: 不支持的 cursor 'hand'（支持 auto/default/pointer/text/move/...）
qlui: 错误：index.css:8: margin/padding 只接受构建期就能算成 px 的值
```

> 这份清单会随版本变化 —— `var()`、`@media`、`calc()`、`min/max-width`、
> `em/rem/vw/vh`、`+`/`~` 选择器、`::before`、`align-self`、`order`、`flex-flow`、
> `visibility`、`pointer-events`、四角/四边 `border-*`、`border-style: dashed/dotted`、
> `outline`、`text-shadow`、多层与 `inset` 阴影、`word-spacing`、`tab-size`、
> `text-decoration-*`（含 `overline`）、`background-image: url()` + `size/position/repeat`、
> `:disabled` / `:enabled` / `:checked` / `::placeholder`、`animation-play-state`
> **都已经支持了**（早期版本把它们列在这里）。

---

## 4. 生成的数据结构

```c
/* 节选：H1–H6、语义块（header/footer/nav/main/article/aside/...）、
   列表（ul/ol/li/dl/dt/dd）、行内（a/abbr/code/kbd/samp/var/q/cite/...）、
   表单（input/select/option）、html/body 等共 70 余个，完整见 qlui.h */
typedef enum {
    QL_UI_TAG_DIV, QL_UI_TAG_SECTION, QL_UI_TAG_P, QL_UI_TAG_SPAN,
    QL_UI_TAG_B, QL_UI_TAG_I, QL_UI_TAG_BUTTON,
    QL_UI_TAG_H1, QL_UI_TAG_H2, QL_UI_TAG_H3, /* ... H4/H5/H6 */
    /* ... */
    QL_UI_TAG_COUNT
} QLUITag;

typedef enum { QL_UI_DISPLAY_BLOCK, QL_UI_DISPLAY_INLINE,
               QL_UI_DISPLAY_INLINE_BLOCK, QL_UI_DISPLAY_NONE,
               QL_UI_DISPLAY_FLEX, QL_UI_DISPLAY_INLINE_FLEX } QLUIDisplay;

/*
 * 长度单位。EM / REM 的 value 是**百分之一 em**（1.5em → 150），
 * VW / VH 的 value 是整数百分比（10vw → 10）。
 * EM/REM 在构建期就折成 PX；只有 VW/VH 活到布局期（那时才知道视口多大）。
 */
typedef enum { QL_UI_UNIT_AUTO, QL_UI_UNIT_PX, QL_UI_UNIT_PERCENT,
               QL_UI_UNIT_EM, QL_UI_UNIT_REM, QL_UI_UNIT_VW, QL_UI_UNIT_VH,
               QL_UI_UNIT_VMIN, QL_UI_UNIT_VMAX } QLUIUnit;

typedef struct { int16_t top, right, bottom, left; } QLUIEdges;
typedef struct { int16_t value; uint8_t unit; uint8_t reserved; } QLUILength;

typedef struct {
    uint32_t bg_color, text_color, border_color;
    QLUILength width, height;
    QLUIEdges margin, padding;
    uint8_t display, bg_alpha;
    uint8_t border_w[4];       /* 上 右 下 左 */
    uint8_t border_style4[4];  /* QLUIBorderStyle，每边 */
    int16_t radius[4];         /* 左上 右上 右下 左下 */
    QLUIShadow shadow[2];      /* box-shadow 最多两层（inset 也在这里） */
    QLUIShadow text_shadow;
    uint8_t text_align, font_id, box_sizing, reserved;
    /* 省略：渐变/阴影/flex/定位/过渡/动画/transform 的字段，完整定义见 qlui.h
       （transform 用 int16_t tf_x/tf_y + int16_t sc_x/sc_y，千分比）
       line-height 是 int16_t line_h（0 = normal），
       white-space 是 uint8_t white_space（0 = nowrap 默认，1 = normal 折行），
       font_flags 是 uint8_t，见 QL_UI_FONT_*（合成加粗/斜体/下划线/删除线）；
       outline 是 uint32_t outline_color + int8_t outline_width / outline_offset
                 + uint8_t outline_style（不占布局）；
       border_color4[4] 是四边颜色（上右下左）；另有
       image_rendering / scrollbar_width / scrollbar_color / object_position[2]；
       B3：int16_t word_spacing, tab_size 是 uint8_t, deco_style/deco_thickness/
           deco_offset/deco_alpha/deco_color 是装饰线；
       B4：bg_image（文本池下标，QL_UI_NO_IMAGE=0xFFFF）、bg_size_mode、
           bg_repeat、int16_t bg_size[2]、int16_t bg_pos[2] */
} QLUIStyle;  /* sizeof == 272（blob v27） */

typedef struct {
    uint16_t tag, parent, first_child, next_sibling, style, text;
    /* 状态变体样式下标，0xFFFF = 该状态与基础样式完全一致 */
    uint16_t style_hover, style_active, style_focus;
    uint16_t style_disabled, style_checked, style_placeholder;  /* B2 */
    uint16_t placeholder;   /* placeholder 属性的文案（文本池下标） */
    uint8_t  flags, reserved;      /* QL_UI_NODE_PASSWORD / _CHECKBOX / _CHECKED / _DISABLED */
} QLUINode;  /* sizeof == 28（blob v27） */

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
void      ql_ui_render(QLUIRoot *ui, QLDraw2D *d2d);          /* CPU 或 GPU */
void      ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas); /* 便捷：包一层 CPU */
void      ql_ui_render_rect(QLUIRoot *ui, QLDraw2D *d2d, QLRect dirty);
QLRect    ql_ui_paint_bounds(const QLUIRoot *ui);
```

| 函数 | 说明 |
|---|---|
| `ql_ui_build` | 把静态表物化成节点树；一次分配，失败返回 `NULL` |
| `ql_ui_free` | 释放（不负责显示驱动） |
| `ql_ui_layout` | 按视口尺寸做完整布局 + 算 `paint_box` |
| `ql_ui_render` | 按树顺序绘制：阴影 → 背景 → 边框 → outline → 文本 → 子节点 |
| `ql_ui_render_rect` | 只重绘 `dirty`：裁剪设成 `dirty∩视口`，靠 `paint_box` 剪掉不相交的子树。**dirty 内的结果与全量重绘逐像素相同** |
| `ql_ui_paint_bounds` | 整棵树的绘制范围（= 根节点的 `paint_box`，可能超出视口，例如带阴影的贴边元素） |

### 5.0.1 模板实例化：`ql_ui_repeat()`

列表长度常常要到运行时才知道（读文件、读网络、数条目）。这时候不要写死 N 份 HTML，
也不要自己动手拼 HTML 字符串（那会把整份 CSS 重新编译一遍）：

```html
<div id="list">
  <div class="row"><span class="k">键 {i}</span><span class="v">{i}</span></div></div>
```

```c
QLUIRoot *ui = ql_ui_build(doc);
QLUINodeRT *tpl = ql_ui_root_node(ui)->first_child;
ql_ui_repeat(ui, tpl, 20);      /* 变成 20 行；{i} 依次是 1..20 */
ql_ui_layout(ui, 480, 640);
```

| 语义 | 说明 |
|---|---|
| 复制范围 | `tpl` **整棵子树**（含嵌套孩子），像素级与手写 N 份完全一样 |
| `{i}` | 替换成 **1 起的序号**；文本里没有 `{i}` 时**一个字节都不分配**（指针原样借用） |
| 样式 | 共享只读的 `QLUIStyle`（模板若在运行期改过样式，副本各得一份独立拷贝） |
| `count == 0` | **真的删掉**那支（节点数减少；不是 `display: none` —— 布局每轮会清掉 style_copy） |
| 调用时机 | **`ql_ui_build()` 之后、`ql_ui_layout()` 之前**；之后节点数不会自动回到原样 |
| ⚠️ 指针失效 | 节点数组会重新分配，**调用后所有 `QLUINodeRT*` 失效**，要用 `ql_ui_root_node()` 重新取 |
| 代价 | `(n-1) × 子树节点数` 个 `QLUINodeRT`（192 B/节点），**没有额外 CSS 成本** |

`test_repeat`（78 项）守着这些契约：节点数、兄弟顺序、嵌套父子关系、样式共享、
`{i}` 序号、`count == 0`、非法参数（根节点当模板 / 溢出 `uint16`）、以及
"没有 `{i}` 时零分配"。

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

### 输入框

```c
typedef void (*ql_ui_input_cb)(QLUIRoot *ui, QLUINodeRT *node,
                               const char *value, void *user);

void        ql_ui_set_input_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
const char *ql_ui_input_value(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t    ql_ui_input_set_value(QLUIRoot *ui, QLUINodeRT *node, const char *value);
```

| 函数 | 说明 |
|---|---|
| `set_input_cb` | 值变化 / 按回车时回调；`cb == NULL` 取消 |
| `input_value` | 当前值；不是 `<input>` 返回 `NULL`。指针在下次编辑前有效 |
| `input_set_value` | 改值（夹到**该控件的容量**：`<input>` 是 `QL_CFG_UI_INPUT_LEN−1`，`<textarea>` 是 `QL_CFG_UI_TEXTAREA_LEN−1`），光标移到末尾、滚动归零 |

编辑能力：点击定位、拖动选字、`Backspace`/`Delete`、`←`/`→`/`Home`/`End`、
`Shift+方向` 扩展选区、`QL_INPUT_TEXT` 插入、`Esc` 收焦点。
**没有剪贴板、没有 IME、光标不闪。**

### 进度条 / 度量条

```c
int      ql_ui_progress_permille(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_progress_set(QLUIRoot *ui, QLUINodeRT *node, int value, int max);
```

| 函数 | 说明 |
|---|---|
| `progress_permille` | 0..1000；`-1` = 不是 `progress`/`meter`，`-2` = 没有 `value`（不确定） |
| `progress_set` | 按 `value`/`max` 设置（夹到 `[0,max]`；`max<=0` 视为 100） |

轨道 = `background-color`，填充 = `color`。不确定进度画固定 1/3（不做动画）。

### 占位标签（`<iframe>` / `<svg>` / `<script>` …）

13 个标签**永远只做占位**，不做真渲染。认下来是为了"整页 HTML 贴进来能编译"。

| 标签 | 行为 |
|---|---|
| `script` `template` | **内容整段跳过**（解析期就丢，不进文本池）；不产生盒子 |
| `svg` `math` | **内容整段跳过**（不是 HTML 子树）；产生 `120×60` inline-block 占位盒 |
| `iframe` `object` `canvas` `video` | 内容解析但隐藏；产生 `300×150` block 占位盒 |
| `audio` | 同上，盒子 `300×32` |
| `embed` | 空元素；`300×150` 占位盒 |
| `noscript` `slot` `track` | `display:none`，零盒子 |

占位盒画 1px 实线边框 + 居中的**标签名**（`iframe`、`svg`…），盒子小于 32×12 时不画字。
这些都是普通布局盒，所以 CSS 完全能覆盖：

```css
iframe { width: 100%; height: 240px; border-width: 0; }
svg    { display: none; }              /* 连盒子都不要 */
noscript { display: block; }           /* 想显示 noscript 的内容就这样写 */
```

没有专门的 API —— 它们就是"画不出东西的节点"，不需要运行期状态。

### 图片（`<img>`）

```c
typedef struct {
    const char    *name;         /* 对应 <img src> */
    const uint8_t *data;
    size_t         len;
    uint16_t       w, h;         /* RAW 必填；其他格式填 0 */
    uint8_t        format;       /* QL_IMAGE_SRC_RAW/BMP/QOI/PNG/AUTO */
    uint8_t        pixel_format; /* RAW 时必填（QL_IMAGE_RGB565 等） */
} QLUIImageInput;

ql_err_t       ql_ui_register_image(QLUIRoot *ui, const QLUIImageInput *in);
const QLImage *ql_ui_node_image(const QLUIRoot *ui, const QLUINodeRT *node);
```

| 情况 | 返回 |
|---|---|
| 名字对不上任何 `<img src>` / 界面里没有 `<img>` / 参数非法 | `QL_ERR_INVAL` |
| 格式这个构建没编进来（PNG/BMP/QOI 或平台后端） | `QL_ERR_UNSUPPORTED` |
| 解码成功 | `QL_OK`，并且**内部已重排**（固有尺寸可能变了） |

`ql_image_validate()` / `ql_image_decode()`（`<qlgl/qlimage.h>`）是通用入口：
任何外来字节进绘制层之前都要过 `validate`（"先除后乘"挡 `w*h*bpp` 溢出）。

| 输入格式 | 现在支持 | 输出 |
|---|---|---|
| `QL_IMAGE_SRC_RAW` | ✅（不复制，`owned = 0`，零 RAM） | 直通 |
| `QL_IMAGE_SRC_BMP` | ✅ 24/32 位、8 位调色板、`BITFIELDS`、上下翻转（不解 RLE） | 按 `out_format` 转换 |
| `QL_IMAGE_SRC_QOI` | ✅ 六种 op | 同上 |
| `QL_IMAGE_SRC_PNG` 备注 | IDAT 是 **zlib 流**（校验头 + Adler-32）；chunk CRC 不校验；**不解** Adam7 隔行、APNG、iCCP/色彩管理 | — |
| `QL_IMAGE_SRC_PNG` | ✅ 位深 1/2/4/8/16 × 颜色类型 0/2/3/4/6、filter 0–4、`tRNS`；**隔行扫描（Adam7）明确报 `QL_ERR_UNSUPPORTED`**（装了平台后端时会交给后端） | 同上 |
| `QL_IMAGE_SRC_GIF` | ✅ **首帧**：全局/局部调色板、透明色、隔行、画布偏移。**不做动画** | 同上 |
| `QL_IMAGE_SRC_JPEG` | ✅ 需要平台后端（Linux 上默认带 libjpeg-turbo）。没有后端 → `QL_ERR_UNSUPPORTED` | RGBA8888（再由本库转换） |

`out_format` 默认 **RGBA8888**（它的枚举值就是 0，所以 `memset` 清零的请求即默认）。
`src_format = QL_IMAGE_SRC_AUTO` 时按魔数探测（PNG/BMP/QOI；RAW 无法探测，必须显式给）。

**构建期**用同一份解码器：`qltext cc --root DIR --pixel-format rgb565` 会把
`<img src>` 指向的文件解成目标格式嵌入 doc，运行时**零解码**。

### 平台后端（JPEG 等）

```c
typedef struct QLImageBackend {
    int (*decode)(const uint8_t *data, size_t len, uint8_t out_format, QLImage *out,
                  char *err, size_t err_cap);
    const char *name;
} QLImageBackend;

void  ql_image_set_platform_backend(const QLImageBackend *be);   /* NULL = 卸载 */
const QLImageBackend *ql_image_platform_backend(void);
const char *ql_image_last_error(void);
int         ql_image_convert(const QLImage *src, uint8_t dst_format, QLImage *out);

#if QL_FEAT_IMG_LIBJPEG
const QLImageBackend *ql_image_backend_libjpeg(void);            /* Linux/PC 参考实现 */
#endif
```

| 规则 | 说明 |
|---|---|
| 调度顺序 | **自有解码器优先**；返回 `QL_ERR_UNSUPPORTED` 才问后端 |
| 谁来装 | 由应用层（或 `qltext` 这样的宿主工具）调用 `ql_image_set_platform_backend()`；不装 = 只有自有解码器 |
| 后端职责 | 只负责"解出来"：`px` 用 `ql_mem_alloc` 分配、`owned = 1`。返回 0 = 不认识、-1 = 失败 |
| 返回值会被校验 | `stride`/`len`/`format`/像素数上限全部重查，不合规按失败处理（不信任第三方库） |
| 格式收口 | 后端给的格式与请求不同时，本库用 `ql_image_convert()` 转换 |
| 失败原因 | `ql_image_last_error()` 返回带后端名的文案（如 `libjpeg: Premature end of JPEG file`） |
| 取消失败 | 一张坏图**不会**让进程退出（libjpeg 默认的 `error_exit` 会 `exit(1)`，参考实现换成了 longjmp） |

典型用法：

```c
/* 应用启动时（PC） */
#if QL_FEAT_IMG_LIBJPEG
ql_image_set_platform_backend(ql_image_backend_libjpeg());
#endif
```

**CPU 与 GPU 都有 `blit_image`**，两者逐像素一致（`test_gl_vs_cpu` 里有一页专门比对，
差异 0.00%）。GPU 侧是"图片单独成批 + 纹理缓存"，滤波默认 `GL_NEAREST` 以对齐 CPU 的
最近邻；想要双线性就把 `QL_CFG_IMAGE_BILINEAR` 设成 1（两条后端一起变）。

### 图像热区（`<map>` / `<area>`）

```c
int              ql_ui_area_at(const QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int              ql_ui_area_count(const QLUIRoot *ui);
const QLUIArea  *ql_ui_area_get(const QLUIRoot *ui, int index);
const QLUINodeRT *ql_ui_area_image(const QLUIRoot *ui, int index);
QLRect           ql_ui_area_rect(const QLUIRoot *ui, int index);
const char      *ql_ui_area_href(const QLUIRoot *ui, int index);
const char      *ql_ui_area_alt(const QLUIRoot *ui, int index);
void             ql_ui_set_area_cb(QLUIRoot *ui, ql_ui_area_cb cb, void *user);
```

| 函数 | 说明 |
|---|---|
| `area_at` | 命中的热区下标；没命中 / 没有热区返回 `-1` |
| `area_count` | 这份界面里的热区条数（doc 里没有 `<area>` 时为 0） |
| `area_get` | 只读描述（`img` / `shape` / `ncoord` / `href` / `alt` 的池下标）；越界返回 `NULL` |
| `area_image` | 该热区所属的 `<img>` 节点 |
| `area_rect` | 当前布局下热区在屏幕上的包围盒（已与内容盒求交） |
| `area_href` / `area_alt` | 字符串；没写返回 `NULL` |
| `set_area_cb` | 点击回调 `(ui, index, user)`；注册后**接管**图片上的点击 |

```html
<div id="root">
  <img src="map.png" usemap="#board">
  <map name="board">
    <area shape="rect"   coords="0 0 63 31"   href="#cpu"  alt="CPU">
    <area shape="circle" coords="96 64 20"     href="#ram"  alt="RAM">
    <area shape="poly"   coords="10 90 50 90 30 120" href="#io">
    <area shape="default"                      href="#bg">
  </map>
</div>
```

要点：

- **坐标是图片像素**（原点 = 图片左上角），与 HTML 一致；图片被缩放到别的尺寸时
  热区跟着缩放（一次整数换算），所以 `img { width: 200px }` 不会让热区错位。
- `shape` 缺省是 `rect`；`rect` 只要 4 个坐标、`circle` 3 个、`poly` 偶数个且 ≥ 6、
  `default` 不许带坐标。**数量不对就是编译错误**，不会"猜一半"。
- `<map>` / `<area>` / `<source>` 是 `display:none`，**不占空间**；`<picture>` 是透明容器。
- 命中时派发 `area_cb`，**不再**派发 `click_cb`（与浏览器一致：点击的 target 是 `<area>`）。
- 不做导航：`href` 只是字符串，跳转/换页由回调里的应用逻辑决定。
- 上限 `QL_CFG_UI_AREA_MAX`（默认 16 条，每条 ≤ 128 个坐标）；设成 0 则整套裁掉
  （doc 里再多 `<area>` 也解析成 0 条，`area_at` 恒返回 `-1`）。

### 浮层（`<dialog>`）

```c
int      ql_ui_dialog_open(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_dialog_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);
int      ql_ui_dialog_close_top(QLUIRoot *ui);
```

打开时：居中、视口压暗、点遮罩或 `Esc` 关闭。同一时刻只认**最后打开**的那个。
`ql_ui_paint_bounds` 在浮层打开时返回整个视口（遮罩要盖满，脏区重绘才不会漏）。

### 折叠面板

```c
int      ql_ui_details_open(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_details_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);
```

`<details>` 收起时把非 `<summary>` 的子节点指向共享隐藏样式（`display:none`）。
`set_open` 会重排（高度变了）；状态没变时是空操作。点 `<summary>` 也会切换。

### 下拉框

```c
void     ql_ui_set_select_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
int      ql_ui_select_index(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_select_set_index(QLUIRoot *ui, QLUINodeRT *node, int index);
```

| 函数 | 说明 |
|---|---|
| `set_select_cb` | 用户改了选中项或程序设值时回调（`value` = 选项文字） |
| `select_index` | 当前第几项；不是 `<select>` 返回 `-1` |
| `select_set_index` | 设置选中项（**越界夹紧**，不报错） |

面板是**覆盖层**：不占布局、不受父级 `overflow` 裁剪、同一时刻只开一个。
键盘：`↑`/`↓` 改选中，`空格`/`回车` 展开收起。

### 滚动

```c
ql_err_t ql_ui_scroll_to(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x, ql_coord_t y);
ql_err_t ql_ui_scroll_by(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy);
int      ql_ui_scrollable(const QLUINodeRT *node);
```

`scroll_to` 只设状态（**需要自己再调一次 `ql_ui_layout`**），`scroll_by` 内部重排并标脏。

**平滑滚动**：容器写了 `scroll-behavior: smooth` 时，`scroll_to` / `scroll_by` 只记目标
（`scroll_goal_x/y` + `scroll_goal_ms`），偏移由 `ql_ui_anim_tick(ui, now_ms)` 逐帧推进
（160ms ease-out，内部会自己重排）。所以宿主要像驱动动画一样每帧调它：

```c
if (ql_ui_anim_tick(ui, ql_anim_now_ms()) > 0)   /* 有动画/滚动在跑 */
    mark_dirty_and_render();
```

不写 `scroll-behavior` 就是**立即跳**（老行为）。

**滚轮的单位是"格"，不是像素。** `QLInputEvent.dy` 是格数（X11 的 Button4/5 一次一格），
`ql_ui_input_event` 会乘上 `QL_CFG_UI_WHEEL_STEP_PX`（默认 **50px**，约 3 行，
与 Firefox 同量级；Chrome 是 100px）。想更快/更慢改这一个宏。

滚到头之后**继续往上滚父级容器**（最多 8 层），与浏览器一致：
内层列表到底了，整页接着滚。

### 焦点

```c
void        ql_ui_set_focus_node(QLUIRoot *ui, QLUINodeRT *node);
QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui);

/* 光标位置（CSS px，视口坐标）：给输入法定位候选窗。
 * 返回 1 = 有聚焦输入框；out_y + out_h 就是 XIM 的 spot 点。 */
int ql_ui_ime_spot(const QLUIRoot *ui, ql_coord_t *out_x, ql_coord_t *out_y,
                   ql_coord_t *out_h);
```

`Tab` / `Shift+Tab` 在 `input` / `textarea` / `select` / `button` 之间按树序循环。
焦点只点亮 `QL_UI_RT_FOCUS`（`:focus` 样式靠它），**不影响布局**。

### 剪贴板

```c
typedef struct {
    void (*set_text)(const char *utf8, void *user);
    int  (*get_text)(char *buf, size_t cap, void *user);  /* 0 = 还没到 */
} QLUIClipboard;

void     ql_ui_set_clipboard(QLUIRoot *ui, const QLUIClipboard *cb, void *user);
ql_err_t ql_ui_clipboard_copy(QLUIRoot *ui);    /* 焦点输入框的选区；无选区复制全文 */
ql_err_t ql_ui_clipboard_cut(QLUIRoot *ui);
ql_err_t ql_ui_clipboard_paste(QLUIRoot *ui);
```

`Ctrl+C` / `Ctrl+X` / `Ctrl+V` 在 `ql_input` 层就映射到这���个函数（X11 那边带 Ctrl 的
组合**不产生文本事件**，所以不会打架）。没装后端时它们返回 `QL_ERR_UNSUPPORTED` 并**什么也不做**。

**`get_text` 可以是异步的**：X11 的剪贴板是点对点协议（要等对方的 `SelectionNotify`），
所以第一次可能返回 0 —— 界面层会记住这次粘贴，在下一帧的 `ql_ui_anim_tick()` 里重试。
主循环只要照常每帧调 `ql_ui_anim_tick()` 就行。

X11 的后端已经接好：`ql_gfx_gl_attach_clipboard(gfx, ui)`（拿到 GL 窗口之后调一次）。

### 接上输入系统

```c
void ql_ui_attach_input(QLUIRoot *ui);   /* 主循环里只调 ql_input_dispatch() 即可 */
void ql_ui_detach_input(void);
```

**必须先接这一步。** 平台层只负责把窗口事件塞进队列，`ql_input_dispatch()` 派发时
找不到事件汇就会**静默丢掉** —— 表现是"窗口能开、能滚，但点不动、hover 不亮、
也打不了字"。同一时刻只支持一个 `QLUIRoot`，析构前记得 `ql_ui_detach_input()`。

**Linux 上的键盘 / 输入法**：X11 后端把字符翻成 `QL_INPUT_TEXT`（界面层靠它插入字符），
控制键翻成 `QLKey`；`Ctrl` / `Alt` 组合**不产生文本**（Ctrl+A 不该插入 `a`）。

中文/日文走 **XIM**：窗口创建时会 `XOpenIM` + `XCreateIC`，键事件先过 `XFilterEvent`
（输入法正在组字时按键归 IME），字符用 `Xutf8LookupString` 取 UTF-8。
这让 fcitx5 / ibus 的 XIM 服务能直接用。**两个前提**：

1. 进程里要调过 `setlocale(LC_ALL, "")`（XIM 靠它选 locale 对应的输入法）；
2. 系统装了 fcitx / ibus 且它们的 XIM 模块是开着的。

拿不到 XIM 时不是错误，会静默降级成"只能打 Latin-1"（ASCII）。
输入法一次提交超过 63 字节会被丢弃（不截半个汉字）—— 正常的词不会这么长。

**窗口缩放**：`ql_gfx_gl_poll()` 收到 `ConfigureNotify` 会更新画布尺寸并置位，
主循环里检查一次即可：

```c
if (ql_gfx_gl_resize_pending(gfx)) {
    ql_coord_t cw, ch;
    ql_gfx_gl_size(gfx, &cw, &ch);     /* CSS px */
    ql_ui_layout(ui, cw, ch);          /* 重排是界面层的事 */
}
```

**拖动边框时会闪黑？两个措施都做了，但你还得配合一件事**：窗口变大时，
新露出来的那块区域是 **X 服务器用窗口背景像素**立刻填的（不是我们画的），
而我们的第一帧要等一次 swap 才到。所以：

1. 后端会把窗口背景像素同步成 `ql_gfx_gl_begin()` 的 `clear_color`
   （颜色变了才发请求），新区域于是变成你给的底色而不是黑；
2. **给 `clear_color` 传页面的底色**，别写死深色 —— 找一个能代表"空页面"的颜色，
   最省事的做法是读根元素的 `background-color`：

```c
static ql_color_t page_bg(QLUIRoot *ui)
{
    const QLUIStyle *s = (ui != NULL && ui->node_count > 0)
                             ? ql_ui_node_style(&ui->nodes[0]) : NULL;
    return (s != NULL && s->bg_alpha > 0) ? s->bg_color : QL_COLOR_RGB(0xFF, 0xFF, 0xFF);
}

ql_gfx_gl_begin(gfx, page_bg(ui));
```

另外两条和"流畅"直接相关：

- **保留模式的 FBO 不会每步重建**：窗口尺寸变了只在装不下时重新分配（按 256 对齐、
  只增不减），连续拖动复用同一块显存 —— 否则每变一次尺寸都要 `glTexImage2D` 一整张
  全屏纹理，缩放会一顿一顿的；
- **默认开垂直同步**（`eglSwapInterval(1)`），帧率跟着 vblank 走。基准测试想要
  不限帧再 `ql_gfx_gl_set_vsync(gfx, 0)`。

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

## 6.6 跑到 MCU 上：只需要接两样东西

`src/` 一行都不用改，但裸机没有操作系统，有两件事必须宿主提供：

```c
/* 1) 内存：换成静态池（或者像 test/mcu 那样用 _sbrk 从静态数组切） */
static ql_alloc_fn ...
ql_mem_set_allocator(&my_alloc);

/* 2) 时间：裸机没有 clock_gettime，动画的默认时间源是 NULL */
static uint32_t my_now_ms(void) { return (uint32_t)(tick_ms()); }
ql_anim_set_time_source(my_now_ms);
```

拿界面数据也有两种：构建期 `qltext cc` 出静态 C（推荐，运行期零解析），
或者把 `.qlui` blob 放进只读区用 `ql_ui_doc_load_mem()`。

**实测数字**（QEMU 的 Cortex-M4，240×160 RGB565，21 节点）：

| 动作 | M4@200MHz |
|---|---|
| 全树重排 | 0.32 ms |
| 整屏光栅 | 6.0 ms |
| 1×1 脏区重绘 | 0.024 ms |

```bash
./test/mcu/build.sh      # 交叉编译 + 在 QEMU 里跑（口径见 test/mcu/README.md）
```

> 结论很直接：**MCU 上必须走脏区重绘**（局部更新比整屏快两个数量级），
> 而且 60fps 的 16.7ms 预算里整屏光栅已占 36%。
> 详见 [教程 17 · 性能诊断与 MCU 基准](../../tutorial/17_perf.html)。

## 7. 限制一览

| 项 | 现状 |
|---|---|
| 布局 | block / inline / inline-block；**无 flex / grid / position** |
| 高度 | 不支持 `%`，不支持 `min/max-height` |
| 文本 | 不支持混合内容、不自动换行、无字距/行距调整 |
| 伪类 | `:hover`/`:active`/`:focus` 编译成变体样式；布局仍按基础样式 |
| 运行期 | 只能改文本（含 `<input>` 的值），不能改结构/样式类（要改结构就换一份 doc） |
| 换 UI | 支持 `.qlui` blob，**不用重编译**；但不支持目标机上解析 HTML/CSS 文本（M3.5） |
| JS | 不支持（交互用 C 回调） |
| 表单 | 有 `<input>` / `<select>`，但**没有 `<form>`**、没有提交、没有校验、没有剪贴板/IME |
| 性能 | 一次 `layout` 遍历整棵树；节点数 ≤ 数百时足够 |

需要不在子集内的特性 → 构建期报错并给替代写法；**没有 WebView 回退路径**（[设计文档 §7.3](../../Web兼容设计.md)）。
