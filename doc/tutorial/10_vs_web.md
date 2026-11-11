# 与 Web 的差别

这一章把"从浏览器稿子搬到 QLGL"会踩到的坑一次讲完。

**逐条状态去看这三份**：[CSS 覆盖清单](../manual/ui/CSS3覆盖与裁剪.html)（每条属性支持到哪一步）、
[CSS 进度规划](../manual/ui/CSS3进度规划.html)（还剩哪些在排队）、[HTML5 标签清单](../manual/ui/HTML5标签覆盖与裁剪.html)（114 个元素）。
本章只讲"从浏览器稿子搬过来**最容易踩**的那几个坑"。

**先说清楚定位**：QLGL 不是浏览器，也不是 WebView。它借 HTML/CSS 的**写法**，
实现却是一套"构建期编译成静态表 + 每帧直接画"的 GUI 库。所以有大量东西
**故意不支持** —— 而且都是**明确报错**，不会静默降级。

先给你一个心理预期：**一份能在 Chrome 里跑的页面，直接贴进来通常跑不起来** ——
但不会是"渲染错一点"这种软失败，而是一条条带行号的报错，告诉你哪一行用了
本库做不到的东西。

---

## 1. 最容易被忽略的一条：默认不折行

```css
/* 浏览器：文字自动按宽度折行 */
/* QLGL：默认一段文本就是一行，超出就溢出（被父级 overflow 裁掉） */
```

| 项 | Web | QLGL |
|---|---|---|
| `white-space` 默认值 | `normal` | **`nowrap`** |

想要折行必须显式写：

```css
.desc { white-space: normal; line-height: 18px; }
```

为什么反过来：v0 的文本模型是"一段文本一行"，整套逐像素基准都建立在这个前提上；
默认改成 `normal` 会让已有界面在你没改任何东西时重新排版。详见
[文本](02_text.html)。

---

## 2. HTML：认 114 个标签，画 101 个

| 在浏览器里能写 | 在 QLGL 里 |
|---|---|
| `<html>` `<body>` | ✅ 当普通块级容器（**整个文档仍然只有一个根元素**） |
| `<head>` `<title>` `<meta>` `<link>` `<style>` `<base>` | ✅ **整段跳过**（不进树、不占位；`<style>` 内容不会变成文字） |
| `<!DOCTYPE html>` `<!-- 注释 -->` | ✅ 直接跳过 |
| `<a>` | ⚠️ 只是带颜色的文字：`href` 被忽略，没有光标、没有导航 |
| `<img>` | ⚠️ 能画，但 `src` 是**资源名**：字节要靠 `ql_ui_register_image()` 或构建期转码给（没有文件系统/网络） |
| `<input type="text\|password">` | ✅ 单行输入（见下） |
| `<select>` `<option>` | ✅ 覆盖式下拉面板（见下） |
| `<textarea>` | ✅ 多行输入（独立 arena，按视觉行定位光标） |
| `<table>` `<tr>` `<td>` `<th>` `<caption>` `<colgroup>` | ✅ 已支持（B7）：`display: table` 全家 + `table-layout: auto/fixed` + `colspan/rowspan` + `border-collapse`，见[表格](22_table.html) |
| `<style>` | ✅ **整段跳过**（不进树）。样式仍然只来自构建期的 `.css` 文件 |
| `<script>` | ✅ 占位盒，**内容不变成文字**；没有脚本引擎 |
| `<canvas>` `<video>` `<audio>` `<iframe>` `<svg>` `<math>` 等 13 个 | ✅ 占位盒（能排版、不渲染内容，EMBED 包） |
| 其余 101 个标签 | ✅ 见[标签与语义](08_tags.html) |

> 完整的 114 个元素逐条状态（含 MCU 能力包裁剪）在
> [HTML5 标签覆盖与裁剪](../manual/ui/HTML5标签覆盖与裁剪.html)。

**影响渲染的属性只有这几个**：`class` / `id` / `style` / `value` / `type` /
`placeholder` / `disabled` / `checked`（+ `<option>` 的 `selected`）。其余（`data-*`、
`aria-*`、`href`、`title`、`maxlength`…）会被**静默忽略** —— 这是本库唯一"不报错的宽容"，
因为属性不影响渲染。`type` 的合法值：`text` / `password` / `search` / `email` /
`number` / `checkbox` / `radio`，别的值（如 `date`）**报错**。

几个结构性差异：

| 写法 | 差异 |
|---|---|
| `<br>` | 是一个"高度 0 的块"，靠块级结束行内行来换行；占一个节点 |
| `<hr>` | 是一个 1px 高的块，**必须自己有背景色**（不然是黑的） |
| `<ul><li>` | 结构与缩进都有，但**不画项目符号**（没有 `::marker`） |
| `<em>` `<strong>` `<u>` `<s>` | 合成加粗/斜体/装饰线（不加载第二个字体文件） |
| `<button>` | 只是一个样式化盒子；点击靠事件回调，没有表单语义 |

---

### 2.1 输入控件：没有表单

`<input>` 与 `<select>` 能用，但不要指望表单语义：

| 在浏览器里 | 在 QLGL 里 |
|---|---|
| `<form>` `submit` | ❌ 没有 `<form>`，也没有提交；用 `ql_ui_set_input_cb()` 接"值变了/按了回车" |
| `placeholder` | ✅ 空值时画占位文字，样式走 `::placeholder` |
| `disabled` | ✅ 不接焦点、不接输入，样式走 `:disabled` |
| `readonly` | ❌ 静默忽略（要只读就自己别接 `input_cb`） |
| `required` / `pattern` / `maxlength` | ❌ 没有校验；长度上限是编译期常量 `QL_CFG_UI_INPUT_LEN` |
| `<progress>` / `<meter>` | ✅ 画成一根条；`low`/`high`/`optimum` 分色没有 |
| `<details>` / `<summary>` | ✅ 能折叠；**没有三角标记**（没有 `::marker`），自己写字符 |
| `<dialog>` | ✅ 真浮层（居中 + 遮罩 + `Esc`/点外面关）；**没有焦点圈定**，也不支持嵌套模态 |
| `type="date"` `type="color"` 等 | ❌ 报错（支持 `text`/`password`/`search`/`email`/`number`/`checkbox`/`radio`） |
| `type="checkbox"` `type="radio"` | ✅ 16×16 方框，勾态画 `√`；`:checked` 生效 |
| `Tab` 切换焦点 | ✅ 在 `input` / `select` / `button` 之间按树序循环，到头绕回 |
| 剪贴板（Ctrl+C/V/X） | ✅ 平台后端接口 `QLUIClipboard`；GL/X11 用 `ql_gfx_gl_attach_clipboard()` 一次接好 |
| 中文输入法（IME） | ⚠️ 要平台层把上屏文本翻译成 `QL_INPUT_TEXT` |
| 光标闪烁 | ❌ 不闪（闪烁要求主循环一直醒着，省电优先） |
| `<select>` 展开的面板 | ✅ 是**覆盖层**：不占布局、不被父级 `overflow` 裁剪、同一时刻只开一个 |
| `<option>` | ✅ 自动 `display: none`（只活在面板里），不要再给它写尺寸 |

**为什么 `<input>` 的值是"运行期唯一可变文本"**：整套数据模型是编译期算完的静态表，
输入框是唯一例外 —— 它有一条自己的池（`QL_CFG_UI_INPUT_MAX` 条 ×
`QL_CFG_UI_INPUT_LEN` 字节），界面里没有 `<input>` 时是零字节。

---

## 3. CSS：选择器是构建期的一小撮

支持（**全部在构建期求值**，运行期只剩"切一个样式下标"）：

```css
div { }                    /* 标签 */
.card { }                  /* 类（.a.b 可叠加） */
#id { }                    /* id */
* { }                      /* 通配 */
.nav .item { }             /* 后代 */
.list > .row { }           /* 子代 */
.a + .b { }   .a ~ .b { }  /* 相邻 / 兄弟 */
input[type="text"] { }     /* [attr] / [attr=v] / [attr~=v] / [attr|=v] */
a[href^="http"] { }        /* [attr^=v] / [attr$=v] / [attr*=v] */
li:first-child { }         /* 结构伪类：first/last/only/nth-child(an+b)、*-of-type */
.a:not(.b) { }             /* :not(<简单选择器>) */
.btn:hover { }             /* :hover / :active / :focus */
.btn:disabled { }          /* :disabled / :enabled / :checked */
.inp::placeholder { }      /* ::placeholder */
.card::before { content: "→"; }   /* ::before / ::after + 纯文本 content */
.card::before { left: 4px; }       /* 同一伪元素可拆成多条规则，合成一个节点 */
```

不支持（都会报错）：

| 写法 | 为什么 |
|---|---|
| `:has()` / `:is()` / `:where()` / `:focus-within` / `:focus-visible` | L4 选择器；`has()` 会引入"反向依赖"，与构建期定型冲突 |
| `::first-line` / `::first-letter` | 要能"只改一行的字形"，与逐行排版冲突 |
| `::selection` / `::marker` | 选区是运行期状态；列表符号请自己写字符 |
| `content: attr(x)` / `counter()` / `url()` | 等于引入一个小运行期求值器 |
| `:nth-child(2n+1 of .a)` | `of` 子句未实现 |

@ 规则与全局特性：

| 特性 | 情况 |
|---|---|
| `@media` | ✅ 但**只有宽度断点**（`min-width` / `max-width`，可用 `and` 串）；`print` 整块跳过 |
| `@keyframes` | ✅ |
| `@font-face` | ✅ B8：`font-family` / `src: url()` / `font-weight` / `font-style`；`unicode-range`、`local()` 报错 |
| `@supports` / `@import` / `@layer` / `@container` | ❌ 报错：构建期就知道支持什么，没有"探测"的必要；合并样式表在构建期做 |
| CSS 变量 `--x` / `var()` | ✅ 构建期替换（`var()` 可带 fallback） |
| `!important` | ✅ 级联两趟：普通先应用，important 后应用 |
| `calc()` / `min()` / `max()` / `clamp()` | ✅ 编译成后缀字节码，布局期求值 |

---

## 4. 单位与数值：px / % / 视口 / 计算值

| 写法 | 结果 |
|---|---|
| `px` / `%` | ✅ |
| `line-height: 1.5` | ✅ 无单位倍数（按 font-size 折算） |
| `em` `rem` `vw` `vh` `vmin` `vmax` | ✅ `em`/`rem` 构建期折 px；`vw`/`vh`/`vmin`/`vmax` 在布局期按视口解析 |
| `calc()` / `min()` / `max()` / `clamp()` | ✅ 编译成后缀字节码，布局期求值；可互相嵌套 |
| `ch` `ex` `lh` `rlh` | ❌ `不支持的长度 '2ch'`（要字符宽就得先有字体度量，构建期拿不准） |

百分比的基准（和浏览器一样，只有两处例外）：

| 属性 | `%` 相对谁 |
|---|---|
| `width` / `margin` / `padding`（横） | 包含块的**内容宽** |
| `height` | 包含块的**内容高** —— **父级高度确定时才有解**（`height: auto` 的父级下按 0 处理） |
| 根元素的 `width/height` | 视口（这就是整页自适应的写法） |

`margin` / `padding` 的 `%` **支持**（与 `width` 同一套基准：包含块的**内容宽**），
`vw` / `vh` / `vmin` / `vmax` 也支持。解析期存的是**原始数值 + 单位**，布局期才折成 px ——
扩展代码里读边距请走 `margin_of()` / `padding_of()`，直接读 `style->margin.left` 会拿到原始数值。

**三条例外要记牢**：

1. **根元素的 `width/height: %` 按视口解析** —— 这是让页面自适应窗口的做法。
   想让整页铺满窗口就写：

   ```css
   #root { width: 100%; height: 100%; box-sizing: border-box; }
   ```

   `box-sizing: border-box` 很重要：`width: 100%` 是**内容盒**宽度，再加上
   padding 就会超出视口（和浏览器一样）。
2. **子元素的 `height: %` 要求父级高度已经确定**。`#root` 有了确定高度，
   它下面一层就好办；但"父级是 `height: auto`、靠内容撑开"的那一层，
   再往下写 `height: 50%` 会解析成 0 高 —— 和浏览器一致，不是 bug。
   要让"子元素跟着窗口长高"仍然推荐**竖向 flex**：

   ```css
   #root { width: 100%; height: 100%; box-sizing: border-box;
           display: flex; flex-direction: column; }
   .list { flex: 1 1 0px; overflow: auto; }   /* 吃掉剩余高度 */
   ```

   ⚠️ 必须是 `1 1 0px`，不能只写 `flex: 1` —— `flex: 1` 的 basis 是 `auto`
   （= 内容高度），内容一多就把容器撑爆、后面兄弟节点被挤出视口。
3. **`min-` / `max-` 也吃 `%`**（相对包含块），和 `width/height` 同一套解释。

---

## 5. 属性白名单：写错就报错

所有属性都过一张白名单，不在表里直接报错并给出行号：

```
css:12: 不支持的属性 'float'
```

> **先别照旧文章排除**：`grid`、`table`、`filter` 全家、`mix-blend-mode` / `mask` / 真 group
> `opacity`、`transform: rotate/skew/matrix`、`transform-origin`、`clip-path` 的
> `circle/ellipse/inset/polygon`、`@font-face` + 自由字体列表 + 真字重、`outline`、
> 多重 `box-shadow`、`border-style: dashed/dotted`、CSS 变量、`@media`、`calc()`
> **都已经能用**（其中 `filter` / 离屏图层一族默认关，靠能力包打开，见[清单 §21](../manual/ui/CSS3覆盖与裁剪.html)）。

**没有**这些常见属性（都需要替代方案）：

| 没有 | 替代 |
|---|---|
| `float` / `clear` | 用 flex；文字环绕没有实现 |
| `transform` 的 3D（`perspective()` / `rotate3d()` / `matrix3d()`） | 只有**仿射** 2D：`translate()` / `scale()` / `rotate()` / `skew()` / `matrix()` |
| `clip-path: path()` / `clip-path: url(#…)` | `circle()` / `ellipse()` / `inset()` / `polygon()` 都支持；`at x y` / `round R` / `evenodd` 报错 |
| `backdrop-filter` 除 `blur()` / `saturate()` 之外的函数 | 报错，不静默忽略（`brightness()` / `hue-rotate()` 等） |
| `vertical-align: middle` / 长度值 | 只有 `baseline` / `sub` / `super` |
| `flex-wrap: wrap-reverse` | 只有 `nowrap` / `wrap` |
| `overscroll-behavior` / `scroll-snap-*` | 无（惯性 / 吸附是平台层的事） |
| `list-style`（`::marker`） / `@supports` / `@layer` / `@container` | 符号自己写字符；条件 / 分层用构建期变量或分文件 |

有值的限制（都是**实测**，不是猜的）：

| 属性 | 限制 |
|---|---|
| `border` | 四种线型都行：`solid` / `dashed` / `dotted` / `none` |
| `box-shadow` | **必须给颜色**（不支持默认色）；最多两层，支持 `inset` / `spread` |
| `background-image` | `linear-gradient()` / `radial-gradient()` / `url(<资源名>)` |
| `text-decoration` | `underline` / `overline` / `line-through`，线型 `solid`/`double`/`dotted`/`dashed`（`wavy` 报错） |
| `animation-timing-function` / `transition-timing-function` | 5 个具名缓动 + `steps(n)` + `cubic-bezier(a,b,c,d)` |
| `filter` | 颜色矩阵族（`brightness`/`contrast`/`saturate`/`grayscale`/`sepia`/`invert`/`hue-rotate`）+ `blur()`，同一条最多 6 个；`url(#svg)` 报错。**默认关**（`QL_FEAT_CSS_FILTER_LAYER`），见 [CSS 覆盖清单 §21](../manual/ui/CSS3覆盖与裁剪.html) |

`transition` 能过渡的属性是固定的七类：`background-color` / `opacity` / `color` /
`border-color` / `border-radius` / `transform` / `clip-path`；`transition-delay` 支持
（长写与 `transition: opacity .3s ease .2s` 形式的第四个时间值都行）。
逗号写法（`transition: a 200ms, b 200ms`）可以，但**各段必须共用同一组时长与缓动** ——
库里只有一份 `trans_ms` / `trans_ease`；想要不同时长就把两层拆开写。

---

## 6. 布局：flex 是子集，行内排版是简化版

| 项 | 情况 |
|---|---|
| `flex-direction` / `justify-content` / `align-items` / `align-content` / `flex-wrap` / `gap` / `flex-grow` / `flex-shrink` / `flex-basis` | ✅ |
| `order` / `align-self` / `flex-flow` | ✅ |
| `grid` | ✅ `display: grid` 全家（轨道 / `fr` / `minmax()` / `repeat()` / 具名区域 / 跨格 / 自动放置 / 对齐）；实测比嵌套 flex 拼网格**快 46%** |
| `display: table` 全家 | ✅ 表格布局（`table-layout: auto/fixed`、`colspan/rowspan`、`border-collapse`、`<colgroup>/<col>`）；默认关（`QL_FEAT_CSS_TABLE`） |
| 行内布局 | 简化版：`vertical-align` 只有 `baseline`/`sub`/`super`，行盒按最大行高算 |
| `margin` 折叠 | ❌ 不折叠（相邻块的外边距直接相加） |
| 文本换行 | 只在 `white-space: normal` 时；断行规则见[文本](02_text.html) |

**垂直居中**：单行的 flex 容器里 `align-items: center` 是能用的（单行的交叉尺寸
会撑满容器）；纯块级盒子里的文字仍然靠 `line-height` 等于 `height`。

---

## 7. 运行期：没有 DOM，也没有 JS

这是最大的心智差异。

| Web | QLGL |
|---|---|
| `document.createElement` | ❌ 结构在**编译期**固定 |
| `element.style.x = ...` | ❌ 只能 `ql_ui_set_text()` 改文本 |
| `addEventListener('click')` | ✅ 全局一个点击回调 + `:hover/:active/:focus` 状态样式 |
| 事件冒泡 / 捕获 | ❌ 没有 |
| `requestAnimationFrame` | 主循环自己控制；动画走脏区 |
| CSS 匹配 | ❌ 运行期一次都不做（全部编译期算完） |

也就是说：**界面结构是"数据"，不是"对象树"**。要换界面就重新编译（或者换一份
`.qlui` blob），不是去改节点。

---

## 8. 滚动：没有惯性，也不会传给父级

| 项 | 情况 |
|---|---|
| `overflow: auto` | ✅ 能滚（滚轮 / 拖滚动条 / PageDown） |
| `overflow-x` / `overflow-y` 分轴 | ✅ 只有 `auto` 的那一轴能滚、能画滚动条 |
| 惯性（松手继续滑） | ❌ 松手就停 |
| 滚到底后外层跟着滚（链式滚动） | ✅ 内层到头后继续滚最近的可滚祖先（最多 8 层） |
| 一格滚多远 | 一格 = `QL_CFG_UI_WHEEL_STEP_PX`（默认 50px ≈ 3 行） |
| 自定义滚动条样式 | ❌ 固定 4px 覆盖式 |

见[滚动](09_scroll.html)。

---

## 9. 为什么这么设计

三条架构红线解释了上面几乎所有"缺失"：

1. **没有保留式场景图、没有运行期 CSS 匹配**
   → 所以结构固定、只能在编译期算样式、只能改文本。
2. **外来数据必须有界**
   → 所以 `<img>` / `<video>` 这类"库外的字节"必须由宿主给（没有文件系统 / 网络），
   解析上限全是 `QL_CFG_*` 常量（节点数、嵌套深度、CSS 规则数…）。
3. **明确失败优于半个正确**
   → 所以宁可不支持也不静默降级。`float: left` / `:has()` / `transform: rotate3d()`
   都直接报错，而不是"大概按最接近的东西处理一下"。

代价是学习一套子集；收益是**没有任何运行期不确定性**：一帧要画多少、要花多久，
在编译期就定了。

---

## 10. 迁移清单

从浏览器稿子搬过来的顺序建议：

1. **先删掉外壳**：`<html>` `<head>` `<body>` `<script>` `<style>`，只留一个根 `div`；
   CSS 单独放（或者用 `ql_ui_load_text` 直接内联字符串）。
2. **单位**：`px` / `%` / `em` / `rem` / `vw` 族 / `calc()` 都能用，一般不用改；
   `ch` / `ex` 这类字体相对单位不支持，要换成 px。
3. **删掉仍然不支持的**：`float` / `clear`、3D `transform`、`:has()` / `:is()` / `:where()`、
   `clip-path: path()`、`@supports` / `@layer`、`list-style`。
   （`cursor` / `outline` / `grid` / `table` / `::before` / `@media` / CSS 变量 / `calc()`
   / 字体列表 / `filter` 一族**都已经能用**，别照旧文章删掉它们。）
4. **给每段长文本加 `white-space: normal`**（否则不折行）。
5. **给每个固定高度的文字盒子加 `line-height`**（否则字贴顶）。
6. **把 `:hover` 之外的状态都换成事件**：没有 JS，交互靠点击回调 + 三个伪类。
7. **表单控件换成回调**：`<input>` / `<select>` 的值不会随提交一起走，
   要用 `ql_ui_input_value()` / `ql_ui_select_index()` 自己读。
8. **跑一遍**：报错会一条条告诉你剩下什么，按报错改。

---

上一章：[滚动](09_scroll.html) ｜ 下一章：[输入控件](11_input.html)
