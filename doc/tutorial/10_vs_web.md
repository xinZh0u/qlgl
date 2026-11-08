# 与 Web 的差别

这一章把"从浏览器稿子搬到 QLGL"会踩到的坑一次讲完。

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

## 2. HTML：认 114 个标签，画 78 个

| 在浏览器里能写 | 在 QLGL 里 |
|---|---|
| `<html>` `<body>` | ✅ 当普通块级容器（**整个文档仍然只有一个根元素**） |
| `<head>` `<title>` `<meta>` `<link>` `<style>` `<base>` | ✅ **整段跳过**（不进树、不占位；`<style>` 内容不会变成文字） |
| `<!DOCTYPE html>` `<!-- 注释 -->` | ✅ 直接跳过 |
| `<a>` | ⚠️ 只是带颜色的文字：`href` 被忽略，没有光标、没有导航 |
| `<img src>` | ❌ 没有图片解码器（有计划，见下） |
| `<input type="text\|password">` | ✅ 单行输入（见下） |
| `<select>` `<option>` | ✅ 覆盖式下拉面板（见下） |
| `<textarea>` | ❌ 还没有多行输入 |
| `<table>` `<tr>` `<td>` | ❌ 用嵌套 flex 代替（表格布局还没做） |
| `<style>` `<script>` | ❌ 没有样式表标签，也没有脚本 |
| 其余 78 个标签 | ✅ 见[标签与语义](08_tags.html) |

> 完整的 114 个元素逐条状态（含 MCU 能力包裁剪）在
> [HTML5 标签覆盖与裁剪](../manual/ui/HTML5标签覆盖与裁剪.html)。

**属性只有五个有用**：`class` / `id` / `style` / `value` / `type`。其余（`data-*`、
`aria-*`、`href`、`title`…）会被**静默忽略** —— 这是本库唯一"不报错的宽容"，
因为属性不影响渲染。`value` / `type` 只管 `<input>`，`type` 写别的值会**报错**
（`只支持 text / password`）。

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
| `placeholder` | ❌ 静默忽略；用 `<input value="...">` 或旁边放一行灰色提示 |
| `disabled` / `readonly` | ❌ 静默忽略 |
| `required` / `pattern` / `maxlength` | ❌ 没有校验；长度上限是编译期常量 `QL_CFG_UI_INPUT_LEN` |
| `<progress>` / `<meter>` | ✅ 画成一根条；`low`/`high`/`optimum` 分色没有 |
| `<details>` / `<summary>` | ✅ 能折叠；**没有三角标记**（没有 `::marker`），自己写字符 |
| `type="date"` `type="number"` | ❌ 报错（只支持 `text` / `password`） |
| `Tab` 切换焦点 | ✅ 在 `input` / `select` / `button` 之间按树序循环，到头绕回 |
| 剪贴板（Ctrl+C/V） | ❌ 没有（平台层不提供） |
| 中文输入法（IME） | ⚠️ 要平台层把上屏文本翻译成 `QL_INPUT_TEXT` |
| 光标闪烁 | ❌ 不闪（闪烁要求主循环一直醒着，省电优先） |
| `<select>` 展开的面板 | ✅ 是**覆盖层**：不占布局、不被父级 `overflow` 裁剪、同一时刻只开一个 |
| `<option>` | ✅ 自动 `display: none`（只活在面板里），不要再给它写尺寸 |

**为什么 `<input>` 的值是"运行期唯一可变文本"**：整套数据模型是编译期算完的静态表，
输入框是唯一例外 —— 它有一条自己的池（`QL_CFG_UI_INPUT_MAX` 条 ×
`QL_CFG_UI_INPUT_LEN` 字节），界面里没有 `<input>` 时是零字节。

---

## 3. CSS：选择器只有一小撮

支持：

```css
div { }              /* 标签 */
.card { }            /* 类 */
#id { }              /* id */
.nav .item { }       /* 后代 */
.list > .row { }     /* 子代 */
.btn:hover { }       /* :hover :active :focus */
```

不支持（都会报错）：

| 写法 | 报错信息 |
|---|---|
| `* { }` | `不支持的简单选择器：'*'` |
| `div[id]` | `不支持的简单选择器：'div[id]'` |
| `.a + .b` / `.a ~ .b` | `不支持相邻/兄弟选择器 '+'` |
| `:not(.a)` | `不支持的伪类 ':not'（只支持 :hover :active :focus）` |
| `::before` / `::after` | `不支持的简单选择器` |

**没有生成内容**（`::before` / `::after` / `content`）是这条线最大的连锁影响：
列表符号、装饰性分隔线、工具提示三角形，全都得写成真实元素。

还有几条整块缺失的：

| 缺失 | 替代 |
|---|---|
| `@media` / `@supports` / `@import` | 没有响应式；尺寸靠 `%` 或运行期改文本 |
| CSS 变量 `--x` / `var()` | 要么重复写，要么构建期用一个脚本做替换 |
| `!important` | 会当成值的一部分 → `不支持的颜色写法 'red !important'` |
| `@font-face` | 只有一套字体（字号任意） |

---

## 4. 单位与数值：只有 px 和 %

| 写法 | 结果 |
|---|---|
| `px` / `%` | ✅ |
| `line-height: 1.5` | ✅ 无单位倍数（按 font-size 折算） |
| `em` `rem` `vh` `vw` `ch` | ❌ `不支持的长度 '2em'（只支持 px 与 %)` |
| `calc()` | ❌ 同上 |

百分比**只相对父容器的内容宽**（不是所有属性都用百分比，比如 margin 不行）。

---

## 5. 属性白名单：写错就报错

所有属性都过一张白名单，不在表里直接报错并给出行号：

```
css:12: 不支持的属性 'outline'
```

**没有**这些常见属性（都需要替代方案）：

| 没有 | 替代 |
|---|---|
| `opacity` | 用 `rgba()` 背景色；关键帧里可以动 `opacity` |
| `font-family` | 只有一套字体 |
| `text-indent` / `letter-spacing` / `word-spacing` | 无 |
| `text-overflow: ellipsis` | 无（超长会直接溢出） |
| `overflow: scroll`（**总是**显示滚动条） | 只接受 `visible` / `hidden` / `auto`；写 `scroll` 等同 `auto`（不溢出就没有条） |
| `outline` / `cursor` / `float` / `clear` | 无 |
| `display: grid` | 用嵌套 flex |
| `position: sticky` | 只有 `static` / `relative` / `absolute` / `fixed` |
| `filter: blur()` | 只有 `backdrop-filter: blur()`（模糊背景） |

有值的限制：

| 属性 | 限制 |
|---|---|
| `border` | 只支持 `solid` |
| `box-shadow` | **必须给颜色**（不支持默认色） |
| `background-image` | 只接 `linear-gradient(...)` / `radial-gradient(...)`，没有 `url()` |
| `transform` | 只有 `translate()` / `scale()` / `scaleX()` / `scaleY()`（无 `rotate`、无 `matrix`） |
| `clip-path` | 只有 `circle(<百分比>)` |
| `text-decoration` | 只有 `underline` / `line-through`（无 `overline`） |
| `animation-timing-function` | 没有 `steps()` / `cubic-bezier()` |

`transition` 能过渡的属性是固定的七类：`background-color` / `opacity` / `color` /
`border-color` / `border-radius` / `transform` / `clip-path`，而且**不支持 `delay`**。

---

## 6. 布局：flex 是子集，没有完整的行内排版引擎

| 项 | 情况 |
|---|---|
| `flex-direction` / `justify-content` / `align-items` / `flex-wrap` / `gap` / `flex-grow` / `flex-shrink` / `flex-basis` | ✅ |
| `order` / `align-self` / `align-content` / `flex-flow` | ❌ 无 |
| `grid` | ❌ 无 |
| 行内布局 | 简化版：没有 `vertical-align`、没有基线对齐、没有完整 line box 概念 |
| `margin` 折叠 | ❌ 不折叠（相邻块的外边距直接相加） |
| 文本换行 | 只在 `white-space: normal` 时；断行规则见[文本](02_text.html) |

**垂直居中**必须靠 `line-height`（和 `height` 相等）；flex 的 `align-items: center`
只居中**盒子**，不居中一个盒子里的文字。

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
| 惯性（松手继续滑） | ❌ 松手就停 |
| 滚到底后外层跟着滚（链式滚动） | ❌ 内层到头就停住 |
| 自定义滚动条样式 | ❌ 固定 4px 覆盖式 |

见[滚动](09_scroll.html)。

---

## 9. 为什么这么设计

三条架构红线解释了上面几乎所有"缺失"：

1. **没有保留式场景图、没有运行期 CSS 匹配**
   → 所以结构固定、只能在编译期算样式、只能改文本。
2. **外来数据必须有界**
   → 所以 `<img>` / `<table>` 这种"库外的东西"直接拒绝；
   解析上限全是 `QL_CFG_*` 常量（节点数、嵌套深度、CSS 规则数…）。
3. **明确失败优于半个正确**
   → 所以宁可不支持也不静默降级。`opacity: 0.5` 报错，
   而不是"大概按不透明度处理一下"。

代价是学习一套子集；收益是**没有任何运行期不确定性**：一帧要画多少、要花多久，
在编译期就定了。

---

## 10. 迁移清单

从浏览器稿子搬过来的顺序建议：

1. **先删掉外壳**：`<html>` `<head>` `<body>` `<script>` `<style>`，只留一个根 `div`；
   CSS 单独放（或者用 `ql_ui_load_text` 直接内联字符串）。
2. **换成 px**：`em` / `rem` / `vh` → `px` 或 `%`；`calc()` 手算。
3. **删掉浏览器专属**：`cursor` / `outline` / `float` / `grid` / `::before` / `@media` / CSS 变量。
4. **给每段长文本加 `white-space: normal`**（否则不折行）。
5. **给每个固定高度的文字盒子加 `line-height`**（否则字贴顶）。
6. **把 `:hover` 之外的状态都换成事件**：没有 JS，交互靠点击回调 + 三个伪类。
7. **表单控件换成回调**：`<input>` / `<select>` 的值不会随提交一起走，
   要用 `ql_ui_input_value()` / `ql_ui_select_index()` 自己读。
8. **跑一遍**：报错会一条条告诉你剩下什么，按报错改。

---

上一章：[滚动](09_scroll.html) ｜ 下一章：[输入控件](11_input.html)
