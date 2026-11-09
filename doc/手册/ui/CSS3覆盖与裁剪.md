# CSS 覆盖与裁剪（全量清单）

这份文档回答三个问题，口径与 [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) 一致：

1. **CSS 的每一样东西，本库支持到哪一步？** —— 按模块穷举，一条不漏。
2. **不支持的为什么不做？** —— 每条给一个**原因码**（§2），不写"暂不支持"这种废话。
3. **哪一批先做？** —— 按"收益 / 风险 / 是否动 `QLUIStyle`"排成五批（§10）。

一句话现状：

> **布局与绘制的骨架已经齐了**（盒模型、flexbox、定位、圆角、阴影、渐变、变换、过渡、动画）；
> 缺的是**真实页面天天用的细节**（`min/max-*`、`em/rem`、`:nth-child`、`::before`、
> `letter-spacing`、`text-overflow`、`border-style`、`cursor`…）。
> 加这些是"补细节"，不是"补架构"。

---

## 0. 已完成 vs 待做（2026-11-09 更新）

**已完成（B2 全部 + B1 第一片）**：

| 能力 | 说明 |
|---|---|
| `*`、`[attr]` 系列、`:first/last/only-child`、`:nth-child(an+b)`、`:*-of-type`、`:not()` | 全部构建期求值 |
| `::before` / `::after` + `content: "文本"` | **构建期展开成真节点**（内部 tag `#pe`） |
| `+` / `~` 兄弟组合符 | 之前直接报错，文档却写着支持 —— 现在真的支持了 |
| `min-width/max-width/min-height/max-height`（px / %） | blob v16；border-box / content-box 各自正确 |
| `!important` | 级联两趟：普通先应用，important 再应用 |
| `hsl()` / `hsla()`、`#rrggbbaa` | |
| `currentColor` / `inherit` / `initial` / `unset` | 颜色位置识别成特殊值 |

**还没做**（下表里仍标 ⏳ 的）：`em`/`rem`/`calc()`、`margin: auto`、
`border-radius` 四角、`letter-spacing` / `text-overflow`（B3）、
`border-style: dashed`（B4）、`align-self` / `order`（B5）、`transform: rotate()`。

> 下面各表的 ⏳ 里混着"已做但表还没逐行翻"的可能 —— **以本节为准**，
> 逐行状态在下一次整理时同步。

---

## 1. 状态记号

| 记号 | 含义 |
|---|---|
| ✅ | 已支持（解析通过且生效） |
| ⚠️ | 支持但**语义有偏差**，偏差写在该行备注里 |
| ⏳ | 计划做（§10 有批次编号） |
| ❌ | 明确不做（写了就**构建期报错**，不静默降级） |
| — | 属性存在但本库没有对应概念（如打印相关），写了报错 |

**红线**：不支持的东西必须报错并给行号。下面每一个 ⏳/❌ 今天的实际表现都是
`ql_ui_text_last_error()` 里一句能读懂的话（多数还带替代方案），而不是"忽略这条规则继续渲染"。

---

## 2. 原因码

每行的"原因"列用下面的码。一个属性可能同时命中多个，只写主因。

| 码 | 含义 | 典型例子 |
|---|---|---|
| **L** | **布局必需**：没有它写不出真实页面 | `min-width`、`margin: auto` |
| **R** | **绘制原语已具备**：`QLDraw2D` 有对应调用，接上就行 | `border-radius`、`box-shadow` |
| **B** | **构建期可算**：样式在编译期摊平，运行期零成本 | 所有选择器、简写展开 |
| **T** | **要时间轴 / 持续重绘**，与"省电优先"冲突 | `animation-play-state: paused` 之外的场景 |
| **O** | **要离屏图层**（组不透明度、混合、滤镜链） | 真 `opacity`、`mix-blend-mode` |
| **F** | **要新光栅路径**（旋转 / 任意仿射 / 非矩形） | `transform: rotate()` |
| **X** | **要外部引擎**（文本整形、排版算法、打印引擎） | `text-justify`、`hyphens` |
| **P** | **与"静态节点 / 构建期定型"冲突**（本库最核心的取舍） | CSS 变量、`@media`、`content: counter()` |
| **M** | **与 MCU 目标冲突**（体积 / 内存 / 代码量） | Grid、多栏、`border-image` |
| **N** | **收益极低 / 已被取代 / 与 GUI 无关** | `float`、`text-emphasis`、打印属性 |

---

## 3. 选择器与规则

### 3.1 简单选择器

| 选择器 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `tag` | ✅ | B | |
| `.class`（可重复 `.a.b`） | ✅ | B | |
| `#id` | ✅ | B | |
| `*`（通配） | ⏳ | B | 加一条规则就能支持；暂未实现，写了报错 |
| `[attr]` | ⏳ | B | `不支持的简单选择器：'[data-x]'` |
| `[attr=v]` `[attr~=v]` `[attr\|=v]` | ⏳ | B | 同上 |
| `[attr^=v]` `[attr$=v]` `[attr*=v]` | ⏳ | B | 同上 |

### 3.2 组合器

| 组合器 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `a b`（后代） | ✅ | B | |
| `a > b`（子代） | ✅ | B | |
| `a + b`（相邻兄弟） | ✅ | B | |
| `a ~ b`（一般兄弟） | ✅ | B | |

### 3.3 伪类

| 伪类 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `:hover` | ✅ | B | 构建期编译成状态变体样式表，运行期只切下标 |
| `:active` | ✅ | B | 同上 |
| `:focus` | ✅ | B | 同上 |
| `:disabled` | ⏳ | B | 状态位机制已就绪，差"变体样式 + 判定" |
| `:checked` | ⏳ | B | 同上 |
| `:first-child` | ⏳ | B | `不支持的伪类 ':first-child'（只支持 :hover :active :focus）` |
| `:last-child` | ⏳ | B | |
| `:only-child` | ⏳ | B | |
| `:nth-child(an+b)` | ⏳ | B | 斑马纹的刚需 |
| `:nth-last-child()` | ⏳ | B | |
| `:first-of-type` … `:nth-of-type()` | ⏳ | B | |
| `:not(<简单选择器>)` | ⏳ | B | 不支持嵌套 |
| `:empty` | ⏳ | B | |
| `:root` | ⏳ | B | 现在用 `#root` 或标签选择器代替 |
| `:link` `:visited` | — | N | 本库没有超链接跳转语义（`<a>` 只是行内文本） |
| `:target` `:lang()` `:dir()` | ❌ | P | 需要运行期状态/文档语言上下文 |
| `:has()` `:is()` `:where()` `:focus-visible` `:focus-within` | ❌ | B | L4 选择器；`has()` 会引入"反向依赖"，与构建期定型冲突 |
| `:hover` 之外的交互态（`:active` 之外） | — | N | |

### 3.4 伪元素

| 伪元素 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `::before` + `content: "文本"` | ⏳ | B | **收益最高的一项**：图标、装饰、前缀全靠它。构建期生成真节点 |
| `::after` + `content: "文本"` | ⏳ | B | 同上 |
| `::first-line` `::first-letter` | ❌ | F | 要"改变行盒内字形"的能力，与逐行排版冲突 |
| `::selection` | ❌ | P | 选区颜色是运行期状态；输入框选区已经硬编码为 `text_color @ 0x50` |
| `::placeholder` | ⏳ | B | `placeholder` 属性本身也还没解析，两者一起做 |
| `::marker` | ❌ | P | 没有生成内容机制，符号请自己写字符 |
| `content: counter()` / `attr()` / `url()` | ❌ | P | 需要计数器/属性求值，等于引入一个小运行期 |

### 3.5 @ 规则

| @ 规则 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `@keyframes` | ✅ | B | 编译期摊平成 `QLUIKeyframe[]` / `QLUIAnimation[]` 静态表 |
| `@font-face` | ❌ | M | 字体文件由宿主 `ql_font_set_default_file()` 提供，不在 CSS 里配 |
| `@media` | ❌ | P | 目标设备尺寸在构建期已知；要多套布局请用两套 doc 或宿主切换 |
| `@supports` | ❌ | P | 构建期就知道支持什么，没有"探测"的必要 |
| `@import` | ❌ | P | 构建期把 CSS 合并成一份即可，避免运行期加载 |
| `@charset` | ✅ | B | 解析期跳过（输入统一按 UTF-8） |
| `@layer` `@container` `@property` `@scope` `@starting-style` | ❌ | P | 都是"让样式表更动态"的东西，与本库"样式构建期定型"相反 |

---

## 4. 值与单位

| 语法 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `px` | ✅ | L | |
| `%` | ✅ | L | 根元素按视口解析；子元素主要用在 `width/height` |
| `auto` | ✅ | L | |
| 无单位倍数 | ⚠️ | L | **只有 `line-height`** 接受（`1.5`） |
| `em` | ⏳ | L | `不支持的长度 '1.2em'（只支持 px 与 %）` |
| `rem` | ⏳ | L | 同上；需要"根字号"这个概念 |
| `vw` `vh` `vmin` `vmax` | ⏳ | L | 根元素的 `%` 已等价，语法糖而已 |
| `s` / `ms` | ✅ | T | `transition-duration` / `animation-duration` |
| `deg` | ⚠️ | L | 只在渐变方向里（`90deg` / `180deg`），不进 `transform` |
| `turn` / `rad` / `grad` | ❌ | N | 只有旋转用得上，而旋转本身还没做 |
| `fr` | ❌ | M | Grid 专用 |
| `ch` / `ex` / `lh` / `rlh` / `cap` | ❌ | N | 都要"字体度量 × 系数"，收益极低 |
| `calc()` | ⏳ | L | 需要一个小表达式求值器；B1 里和 `min/max` 一起做 |
| `min()` / `max()` / `clamp()` | ⏳ | L | 同上 |
| `fit-content` / `max-content` / `min-content` | ⏳ | L | 布局内部已有 max-content 计算，只是没暴露成关键字 |
| 命名色 | ✅ | R | `k_named[]`（CSS 常用色 + `transparent`） |
| `#rgb` / `#rrggbb` | ✅ | R | |
| `rgb()` / `rgba()` | ✅ | R | |
| `#rrggbbaa` | ⏳ | R | 加一个分支 |
| `hsl()` / `hsla()` / `hwb()` / `lab()` / `oklch()` | ⏳ | R | 纯换算，无副作用；`hsl` 优先级高于其余 |
| `inherit` | ⏳ | L | 现在是 `不支持的颜色写法 'inherit'` |
| `initial` / `unset` / `revert` | ⏳ | L | 需要"UA 默认值"可达 |
| `currentColor` | ⏳ | R | 解析期替换成 `color` 的值即可 |
| `!important` | ⏳ | L | 声明层多存 1 bit、比较函数多一级；与 B1 一起做 |
| 转义 / CSS 注释 | ✅ | B | `/* … */` 已支持 |

---

## 5. 盒模型与尺寸

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `display` | ⚠️ | L | 支持 `block` `inline` `inline-block` `flex` `inline-flex` `none`；不支持 `grid` `table` |
| `width` | ✅ | L | |
| `height` | ✅ | L | |
| `min-width` | ⏳ | L | `不支持的属性 'min-width'` —— 响应式的刚需 |
| `max-width` | ⏳ | L | 同上 |
| `min-height` | ⏳ | L | |
| `max-height` | ⏳ | L | |
| `aspect-ratio` | ⏳ | L | 用于图片位、卡片缩略图 |
| `margin` + `-top/right/bottom/left` | ✅ | L | **只支持 px**；负值可用 |
| `margin: … auto` | ⏳ | L | `margin/padding 只支持 px`；是布局概念，不是值解析 |
| `padding` + 四方向 | ✅ | L | **只支持 px**（百分比报错） |
| `box-sizing` | ✅ | L | 表单控件 UA 默认 `border-box` |
| `overflow` | ⚠️ | L | `visible` / `hidden` / `auto` / `scroll`；**没有分轴** |
| `overflow-x` / `overflow-y` | ⏳ | L | `不支持的属性 'overflow-x'` |
| `visibility` | ⏳ | L | `hidden` 与 `display:none` 的区别（保留布局） |
| `float` / `clear` | ❌ | N | 与 flex 混用行为诡异，现代页面基本不用 |
| `contain` / `content-visibility` | ❌ | N | 性能优化类，收益在本库架构里已被脏区取代 |
| `resize` | ❌ | N | 没有"用户拖动元素尺寸"的概念 |

---

## 6. 定位

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `position: static` | ✅ | L | |
| `position: relative` | ✅ | L | |
| `position: absolute` | ✅ | L | |
| `position: fixed` | ✅ | L | |
| `position: sticky` | ⏳ | L | `不支持的 position 'sticky'`；要动滚动容器 |
| `top` / `right` / `bottom` / `left` | ✅ | L | |
| `inset`（简写） | ⏳ | L | 等价四条，加解析即可 |
| `z-index` | ✅ | L | |

---

## 7. 背景与边框

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `background-color` | ✅ | R | |
| `background`（简写） | ⚠️ | R | 只认**纯色或单个渐变**；写成 `background: #fff url(a.png)` 会报错 |
| `background-image: linear-gradient()` | ⚠️ | R | **必须写方向**（`to right` / `to bottom` / `90deg` / `180deg`）；`to bottom right` 报错 |
| `background-image: radial-gradient()` | ✅ | R | 需要 3 个参数（位置, 起色, 止色） |
| `background-image: url(...)` | ⏳ | M | `只支持 linear-gradient() / radial-gradient()`；图片请用 `<img>`（图像管线是独立子系统） |
| `background-size` | ⏳ | R | `cover` / `contain` / 具体尺寸；影响绘制矩形，改动小 |
| `background-position` | ⏳ | R | |
| `background-repeat` / `-attachment` / `-clip` / `-origin` | ❌ | N | 在"界面元素"上用不到（整页背景用渐变更省） |
| `border`（简写） | ⚠️ | R | 只认 `<width>px solid <color>` |
| `border-width` | ✅ | R | 四边统一 |
| `border-color` | ✅ | R | 四边统一 |
| `border-style` | ⚠️ | R | **只有 `solid`**；`dashed` / `dotted` 报错（要做虚线光栅，CPU+GL 两份） |
| `border-top/right/bottom/left-width` | ⏳ | R | `不支持的属性 'border-top-width'`；`QLDraw2D::stroke_round_rect` 只画整圈 |
| `border-*-color` / `border-*-style` | ⏳ | R | 同上 |
| `border-bottom: none` 之类的单边简写 | ⏳ | R | 同上 |
| `border-radius`（单值） | ✅ | R | px 或 %；≥ 短边一半时是胶囊/内切圆 |
| `border-radius`（四角 / 斜杠椭圆角） | ⏳ | R | `不支持的长度 '4px 8px 12px 16px'`；椭圆角要改 SDF，成本高 |
| `border-image` | ❌ | M | 九宫格缩放 + 切片解析，代码量接近圆角的三倍，收益低 |
| `box-shadow`（单条） | ✅ | R | CPU SDF + GPU `mode=2` 各一份实现 |
| `box-shadow`（多重 / `inset`） | ⏳ | O | `box-shadow 里无法识别 ','`；`inset` 要反向 SDF |
| `outline` + `-width/-style/-color/-offset` | ⏳ | R | 焦点圈常用；可以复用边框的描边路径 |
| `box-decoration-break` | ❌ | N | 跨行断开的盒子装饰，本库没有"跨行盒子"概念 |

---

## 8. 颜色与不透明度

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `color` | ✅ | R | |
| `opacity` | ✅ | R | 0~1，默认 1；**逐图元降透明并顺着子树往下乘**（背景/边框/阴影/文字/图片一起淡）。与真 group opacity 的差别见 §20.1 |
| `mix-blend-mode` / `background-blend-mode` | ❌ | O | 要离屏图层 + 混合方程 |
| `isolation` | ❌ | O | 同上（为混合服务的） |
| `color-scheme` / `accent-color` / `caret-color` | ❌ | N | 与"主题由 CSS 自己写"重复 |

---

## 9. 字体与文本

### 9.1 字体

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `font-family` | ⚠️ | M | **只有 `DEFAULT` / `MONO` 两族**（矢量字体 + 内建位图各一套），不是自由字体列表 |
| `font-size` | ✅ | L | `px` / `%`（相对父级）/ `smaller` / `larger` |
| `font-weight` | ⚠️ | M | **合成**：加粗 = 画两遍右移 1px（MCU 上不加载第二份字体） |
| `font-style` | ⚠️ | M | **合成**：斜体 = 按行剪切 |
| `font`（简写） | ⏳ | L | 目前要分开写 |
| `font-variant` / `font-stretch` / `font-feature-settings` | ❌ | M | 要完整字体引擎（HarfBuzz + 多变体字体文件） |
| `font-size-adjust` / `font-kerning` / `font-optical-sizing` | ❌ | X | 同上 |
| `@font-face` | ❌ | M | 见 §3.5 |

### 9.2 文本

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `text-align` | ✅ | L | `left` / `center` / `right`（没有 `justify`） |
| `text-align: justify` | ❌ | B | 要改行内分配算法；中文排版里常用但成本不低，且断行是布局/绘制共用的一份实现 |
| `line-height` | ✅ | L | `normal` / `px` / 无单位倍数 |
| `white-space` | ✅ | L | `normal` / `nowrap` / `pre` / `pre-wrap` / `pre-line` |
| `vertical-align` | ⚠️ | L | **只有 `baseline` / `sub` / `super`**；`middle` / `text-top` / 长度值都报错（`<sub>`/`<sup>` 够用） |
| `text-decoration` | ⚠️ | R | `underline` / `line-through` / `none`；**没有 `overline`**（本库没有上划线概念） |
| `text-decoration-color/-style/-thickness` | ⏳ | R | 装饰线颜色现在固定跟文字色 |
| `letter-spacing` | ⏳ | L | `不支持的属性`；要动 `ql_text_wrap()`（见 §11 红线） |
| `word-spacing` | ⏳ | L | 同上 |
| `text-indent` | ⏳ | L | 首行缩进；中文排版常用 |
| `text-transform` | ⏳ | B | `uppercase` / `lowercase` / `capitalize`；构建期就能改文本 |
| `text-overflow: ellipsis` | ⏳ | L | **列表里最常用的一个**；配合 `overflow: hidden` + `nowrap` |
| `word-break` / `overflow-wrap` / `word-wrap` | ⏳ | L | 长 URL 必须靠它；要动断行函数 |
| `hyphens` | ❌ | X | 要连字词典（各语言各一份） |
| `text-shadow` | ⏳ | R | 复用 `box-shadow` 的 SDF 路径（字形级） |
| `text-justify` / `text-align-last` | ❌ | X | 见 `justify` |
| `direction` / `unicode-bidi` | ❌ | X | RTL 需要 bidi 算法 + 整形（HarfBuzz/SheenBidi），是独立立项 |
| `writing-mode` / `text-orientation` | ❌ | X | 竖排同样要重整排版引擎 |
| `text-emphasis` / `ruby-align` / `ruby-position` | ❌ | X | 注音排版；`<ruby>` 标签本身也还没做 |
| `tab-size` | ⏳ | L | `white-space: pre` 下制表宽度现在固定 |
| `text-decoration-skip-ink` / `text-underline-offset` | ❌ | N | 微观排版，收益极低 |
| `-webkit-line-clamp` | ❌ | N | 非标准；多行截断可以用固定高度 + `overflow: hidden` 近似 |

### 9.3 生成内容

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `content`（纯文本） | ⏳ | B | 与 `::before/::after` 一起做（构建期生成真节点） |
| `content: attr()` / `counter()` / `url()` | ❌ | P | 需要运行期/计数器求值 |
| `quotes` | ❌ | N | 无生成内容 |
| `counter-reset` / `counter-increment` | ❌ | P | 同上 |
| `list-style` / `-type` / `-position` / `-image` | ❌ | P | 没有 `::marker`；符号请自己写字符（`•` / `1.`） |

---

## 10. Flexbox

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `display: flex` | ✅ | L | |
| `display: inline-flex` | ✅ | L | |
| `flex-direction` | ✅ | L | 四个方向 |
| `flex-wrap` | ✅ | L | `nowrap` / `wrap`（没有 `wrap-reverse`） |
| `flex`（简写） | ✅ | L | 含 `flex: 1 1 0px`；**注意 `flex: 1` 的 basis 是 auto，会撑爆容器** |
| `flex-grow` / `flex-shrink` / `flex-basis` | ✅ | L | |
| `justify-content` | ✅ | L | 含 `space-between` / `space-around` / `space-evenly` |
| `align-items` | ✅ | L | 含 `stretch` |
| `align-self` | ⏳ | L | 单个子项覆盖交叉轴对齐 |
| `align-content` | ⏳ | L | 多行交叉轴分配 |
| `order` | ⏳ | L | 重排绘制顺序（不动布局位置，改动小） |
| `flex-flow`（简写） | ⏳ | L | 等价两条，加解析即可 |
| `wrap-reverse` | ❌ | N | 用得极少，`flex-direction` 反向可近似 |
| `gap` / `row-gap` / `column-gap` | ✅ | L | Grid 也用同一组，但本库只在 flex 里生效 |

---

## 11. Grid（整块不做）

| 属性 | 状态 | 原因 |
|---|---|---|
| `display: grid` / `inline-grid` | ❌ | **M**：实现成本接近 flex 的两倍 |
| `grid-template-*` / `grid-auto-*` / `grid-area` / `grid-row` / `grid-column` | ❌ | **M**：同上 |
| `grid-gap` 及其别名 | ❌ | **M**：同上 |

**替代方案**：卡片网格用 `display: flex; flex-wrap: wrap; gap: …` + 固定宽高。
本库自己的 benchmark 就是这么排的（见 `benchmark/bench_scenes.h` 的 `cards` 维度）。

---

## 12. 变换

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `transform: translate()` | ✅ | R | 加到绘制偏移上，**不进布局**（零重排） |
| `transform: scale()` | ✅ | R | 允许负值（几何压扁 + 镜像） |
| `transform: scaleX()` / `scaleY()` | ✅ | R | 假 3D 翻转就靠它 |
| `transform: rotate()` | ⏳ | **F** | `只支持 translate()/scale()/scaleX()/scaleY()`；要旋转光栅路径（CPU 逐像素仿射 + GL 顶点变换 + 字形旋转） |
| `transform: skew()` | ⏳ | F | 同上（斜体就是按行剪切，机制类似） |
| `transform: matrix()` / `matrix3d()` | ❌ | F | 通用仿射需要完整的 3×3 变换管线 |
| `transform: perspective()` / `rotate3d()` 等 3D | ❌ | M | 要真 3D 管线；`scaleX` 已经能近似"翻面"效果 |
| `transform-origin` | ⏳ | R | 现在固定绕元素中心 |
| `transform-style` / `backface-visibility` | ❌ | M | 3D 专用 |

---

## 13. 过渡与动画

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `transition`（简写） | ✅ | T | 可过渡：`background-color` `opacity` `color` `border-color` `border-radius` `transform` `clip-path` |
| `transition-property` | ✅ | T | `all` 或上面那批 |
| `transition-duration` | ✅ | T | |
| `transition-timing-function` | ⚠️ | T | 只有具名缓动：`linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`；`cubic-bezier()` 报错（要一条三阶贝塞尔求值 + 反解，收益低） |
| `transition-delay` | ⏳ | T | |
| `transition-behavior` / `-property: display` | ❌ | P | 离散属性过渡，与本库"帧间只改绘制值"冲突 |
| `animation`（简写）+ 8 个子属性 | ✅ | T | `name/duration/timing-function/iteration-count/direction/fill-mode/delay/play-state` **全齐** |
| `animation-composition` | ❌ | N | 多个动画叠加的合成规则，用得极少 |
| `@keyframes` | ✅ | B | 编译期摊平成静态表；运行期是纯函数 `u = f(now - start)` |
| `animation-play-state: paused` | ✅ | T | 只是停止推进，不产生持续重绘 |

---

## 14. 滤镜、遮罩、混合

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `backdrop-filter: blur()` | ✅ | R | 毛玻璃；读"已画好的内容"就地模糊，有独立小着色器；结果裁进 `border-radius`；自己带 `transform: translate` 时采样区跟着走；下层动画实时跟随 |
| `filter: blur()` | ⏳ | O | 要"元素自己离屏后再模糊"，与 `backdrop-filter` 不是一回事 |
| `filter: brightness/contrast/saturate/grayscale/sepia/invert/hue-rotate/drop-shadow` | ⏳ | O | 每个都要 CPU + GL 两份实现 |
| `clip-path: circle()` | ✅ | R | 子树级圆形遮罩；CPU 与 GPU 各一份 |
| `clip-path: inset()` / `ellipse()` / `polygon()` / `path()` | ⏳ | F | 需要通用裁剪形状 |
| `mask` / `mask-image` / `-webkit-mask` | ❌ | O | 要遮罩图层 |
| `mix-blend-mode` | ❌ | O | 见 §8 |

---

## 15. 多栏、分页、打印（整块不做）

| 属性 | 状态 | 原因 |
|---|---|---|
| `columns` / `column-count` / `column-width` / `column-rule` / `column-span` / `column-fill` / `break-*` | ❌ | **M**：多栏排版要"盒内分裂"，与"一个节点一个盒"冲突；手机上界面也很少用 |
| `page-break-*` / `orphans` / `widows` | — | **N**：本库不打印 |
| `@page` / `@media print` | — | **N**：同上 |

---

## 16. 表格（CSS2.1 遗留）

| 属性 | 状态 | 原因 |
|---|---|---|
| `display: table` / `table-row` / `table-cell` … | ❌ | **M**：表格布局算法（列宽分配、跨行跨列、边框合并）极其繁琐；`<table>` 标签本身也明确报错并给出替代做法 |
| `table-layout` / `border-collapse` / `border-spacing` / `caption-side` / `empty-cells` | ❌ | **M**：同上 |

**替代方案**：用嵌套 flex 画表格（`display: flex` 每一行，单元格给固定宽度）。

---

## 17. UI 与交互

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `cursor` | ⏳ | L | **桌面上很要紧**（指针形状）；需要 `QLDispDriver` 加一个设光标的钩子 |
| `outline` 系列 | ⏳ | R | 见 §7 |
| `user-select` | ⏳ | L | 现在只有输入框能选字 |
| `pointer-events` | ⏳ | L | 命中测试跳过该节点（覆盖层常用） |
| `scroll-behavior: smooth` | ⏳ | T | 要持续重绘 |
| `overscroll-behavior` | ❌ | N | 惯性/回弹是平台层的事 |
| `touch-action` | ❌ | N | 触摸还没接（见开发规划 M6.5） |
| `contain-intrinsic-size` / `will-change` | ❌ | N | 渲染优化提示，本库架构里没有意义 |
| `appearance` | ❌ | N | 表单控件的"原生外观"由 UA 默认样式负责 |
| `zoom` | ❌ | N | 全局缩放由宿主 `ql_disp_set_scale()` / `ql_gfx_gl_set_scale()` 负责 |

---

## 18. 其他模块

| 模块 / 属性 | 状态 | 原因 |
|---|---|---|
| CSS 变量 `--x` / `var()` | ❌ | **P**：会让"运行期样式"变复杂，破坏构建期预计算的收益（本库最核心的取舍） |
| `all`（重置简写） | ❌ | **P**：同上，它是"运行期整套样式重置" |
| `writing-mode` / `text-orientation` | ❌ | **X**：竖排要重整排版引擎 |
| `direction` / `unicode-bidi` | ❌ | **X**：RTL 需要 bidi 算法 + 文本整形 |
| `will-change` / `contain` / `backface-visibility` | ❌ | **N**：GPU 优化提示，自有渲染器不消费 |
| Speech（`speak` / `volume` / `pause`…） | — | **N**：与 GUI 无关 |
| `scrollbar-*` / `scrollbar-width` / `scrollbar-color` | ⏳ | **R**：滚动条现在是内置样式（`SCROLLBAR_W` / `0x808080 @ 0x96`），暴露成属性很便宜 |
| `scroll-snap-*` | ❌ | **T**：要持续重绘 + 吸附动画 |
| `image-rendering` | ⏳ | **R**：CPU 已是最近邻放大；GL 用的是 `GL_NEAREST`，做成属性很便宜 |
| `object-fit` | ✅ | **R** | `fill` / `contain` / `cover` 等（`background-size` 的兄弟） |
| `object-position` | ⏳ | **R** | |
| `image-orientation` | ❌ | **N** | EXIF 旋转由解码器负责 |

---

## 19. 覆盖率：两份实测 + 一份规范口径

三个数回答三个不同的问题，别混着用。

### 19.1 语料实测（最可靠，推荐引用这一份）

口径：本机 **23 份真实 CSS**（Bootstrap 5、mkdocs 主题、Mercurial 的四套主题、
matplotlib web_backend、Font Awesome、VLC 的 jQuery UI 皮肤）当语料，
抽出全部声明后按属性名逐条对照 `k_supported_props[]`。
去掉 CSS 自定义属性（`--x`）与厂商前缀（`-webkit-` 等）后剩 **5391 条声明**。

| 口径 | 结果 | 说明 |
|---|---|---|
| **属性名级** | **57 / 150 = 38%** | 去重后的属性名，不考虑用得多不多 |
| **声明级（推荐口径）** | **3968 / 5391 = 73.6%** | 按真实出现次数加权 —— 常用的那些基本都有 |
| **能直接照搬** | **3305 / 5391 = 61.3%** | 属性支持 **且** 值里不含 `!important`/`var()`/`calc()` |

分模块（同一份语料，按出现次数加权）：

| 模块 | 覆盖 |
|---|---|
| flexbox | **99.8%** |
| 定位 / 层叠 | **100%** |
| 文本 | **98.6%** |
| 盒模型 / 尺寸 | **95.2%** |
| 背景 / 边框绘制 | **82.7%** |
| 变换 / 动画 | **100%**（语料里用得少，只有 43 条） |

按文件看：Mercurial 四套主题 89~95%、mkdocs 主题 84%、Bootstrap 80.5%、
**Font Awesome 8.8%**（整份就是 `content` + `::before`）。

### 19.2 缺口到底在哪（真实数字）

| 来源 | 声明数 | 占比 | 归属 |
|---|---|---|---|
| `content`（图标字体） | 706 | 13.1% | **B2**（`::before` + `content` 一次解决） |
| `max-width` / `min-width` / `max-height` | 65 | 1.2% | **B1** |
| `border-top/right/bottom/left`（四边独立） | 106 | 2.0% | **B4** |
| `border-*-radius`（四角） | 94 | 1.7% | **B1** |
| `!important`（出现在**已支持**的属性里） | 293 | 5.4% | **B1** |
| `var(--x)`（CSS 变量） | 537 | 10.0% | ❌ 明确不做 |
| `calc()` | 65 | 1.2% | **B1** |

### 19.3 补完之后能到多少（模拟同一份语料）

| 阶段 | 声明级覆盖 |
|---|---|
| 现在 | **73.6%** |
| + B1 值与尺寸 | 76.5% |
| + B2 选择器（`content` / `::before`） | **89.6%** |
| + B3 文本细节 | 90.0% |
| + B4 绘制细节 | **94.0%** |
| + B5 布局细节 | 94.5% |

剩下 5.5% 主要是 `float`（不做）、`cursor`（鼠标形状）、`list-style`（没有生成内容）、
`visibility`、`pointer-events`、`@font-face` 的 `src`。

### 19.4 选择器侧（同一份语料）

| 特性 | 全部语料 | 去掉图标字体库 |
|---|---|---|
| `::before` / `::after` | 57.4% | **25.3%** |
| `:nth-` / `:first-` / `:last-child` | 4.0% | **7.3%** |
| `[attr]` 系列 | 2.2% | **3.9%** |
| `:hover` / `:active` / `:focus` | 3.9% | 7.0%（已支持） |

> 图标字体库把 `::before` 的占比顶到 57%，去掉之后是 25% —— **B2 的
> `content`/`::before` 是"用一行代码换 25% 选择器"那一档**。

### 19.5 三道"属性全支持也过不去"的墙

| 墙 | 语料中出现 | 后果 |
|---|---|---|
| `var(--x)` | 537 条声明 | 写 `var()` 就报错。Bootstrap 5 一半样式靠它 |
| `@media` | 191 个块 | 响应式断点整段不认 |
| `!important` | 293 条声明 | 值里带它就报错（B1 补上后消失） |

所以**"CSS3 兼容度"取决于你要搬的是什么**：

| 搬什么 | 现在 | B1+B2 后 |
|---|---|---|
| 自己按本库能力写新页面 | **85~90% 的手感** | 95%+ |
| 直接搬 Bootstrap 这类框架 | ~80% 声明能过（要改写 `var()`/`@media`） | ~92% |
| 搬图标字体库 | **9%** | 接近 100% |

### 19.6 规范口径（按模块的属性个数，**估算**）

上面是"我们遇到的 CSS 长什么样"；这一份回答"规范里有 200 项，我们做了几项"。
分母是 CSS3 各模块属性数的粗略合计，**仅供参考**：

| 模块 | 规范属性数 | 本库支持 | 覆盖率 |
|---|---|---|---|
| 选择器构造 | ~40 | 9 | 22% |
| 值 / 单位 | ~25 | 6 | 24% |
| 盒模型与尺寸 | ~14 | 9 | 64% |
| 定位 | ~9 | 6 | 67% |
| 背景与边框 | ~20 | 8 | 40% |
| 颜色与不透明度 | ~4 | 1 | 25% |
| 字体 | ~10 | 5 | 50% |
| 文本 | ~22 | 10 | 45% |
| 生成内容 | ~8 | 0 | 0% |
| Flexbox | 14 | 11 | **79%** |
| 变换 | ~5 | 4 | 80% |
| 过渡 | 5 | 4 | 80% |
| 动画 | 9 | 9 | **100%** |
| 滤镜 / 遮罩 | ~7 | 2 | 29% |
| UI 与交互 | ~10 | 0 | 0% |
| **合计** | **~200** | **67** | **≈ 33%** |

**两个数差这么多的原因**：规范里一大半属性是冷门项（`page-break-*`、`text-emphasis`、
`border-image`、多栏…），真实样式表几乎不写。**判断可用性看 19.1 的声明级，看"离规范多远"看 19.6。**

---

## 19.1 已支持但**语义受限**的速查表

这些不用改架构，但**写的时候要知道边界**，否则会以为是 bug：

| 属性 | 实际行为 | 边界 |
|---|---|---|
| `background`（简写） | 只认纯色或**单个**渐变 | `background: #fff url(a.png)` 报错；图片请用 `<img>` |
| `linear-gradient()` | **必须写方向** | `to bottom right` 不支持；用 `to right` / `90deg` |
| `border` / `border-style` | 只有 `solid` | `dashed` / `dotted` / `double` 报错 |
| `border-radius` | 单值（px 或 %） | 四角 / 斜杠椭圆角都不行 |
| `box-shadow` | 单条 | 多重、`inset` 都不行 |
| `opacity` | 逐图元降透明 | 子元素会一起淡，但**同一子树内部互相覆盖处会略透**（真 group 要离屏图层，见 §20.1） |
| `transform` | 只有 `translate` / `scale` / `scaleX` / `scaleY` | `rotate` / `skew` / `matrix` 报错 |
| `transform-origin` | **固定为元素中心** | 属性本身没实现 |
| `transition-timing-function` | 5 个具名缓动 | 没有 `cubic-bezier()` |
| `transition-delay` | 没实现 | |
| `font-family` | 只有 `DEFAULT` / `MONO` | 不是自由字体列表；字体文件由宿主提供 |
| `font-weight` / `font-style` | **合成**（画两遍 / 按行剪切） | 不加载第二份字体文件 |
| `text-decoration` | `underline` / `line-through` | 没有 `overline`；颜色固定跟文字色 |
| `vertical-align` | `baseline` / `sub` / `super` | 没有 `middle` / 长度值 |
| `text-align` | `left` / `center` / `right` | 没有 `justify` |
| `overflow` | 一个值管两轴 | 没有 `overflow-x/y` |
| `flex-wrap` | `nowrap` / `wrap` | 没有 `wrap-reverse` |
| `display` | `block` / `inline` / `inline-block` / `flex` / `inline-flex` / `none` | 没有 `grid` / `table` / `contents` |
| `%` 长度 | 根元素按视口；子元素主用于 `width/height` | `padding: 5%` 报错 |
| 长度单位 | `px` / `%` | `em` / `rem` / `vw` / `vh` / `calc()` 都报错 |
| `!important` | 没实现 | 写在声明尾部会被当成值的一部分 |

---

## 20. 三处"看起来简单、其实不简单"

写在这里，免得后面有人当"漏了"顺手补成半成品：

1. **`opacity` 是"逐图元降透明"，不是严格的组不透明度**。真 CSS 把整棵子树先合成到一张
   离屏图层再整体混合；本库的做法是把整体 alpha 乘到**每一个绘制入口**上，并顺着子树
   往下乘（`QLDraw2D.alpha` / `ql_draw2d_set_alpha()`）。对"子树内部不重叠"的界面两者
   逐像素一致；只有**子树内部互相覆盖**处会略透（每个图元各自降透明，重叠区乘了两次）。
   严格版本要在 CPU 与 GL 两端各加一层离屏合成 —— 和 `transform: rotate()` 同级成本。
2. **`margin: auto` 是布局概念，不是值解析**。块级盒要在**有确定可用宽度**时分掉剩余空间；
   flex 里的 `auto` margin 又是另一套（吸收剩余空间）。改动落在 `layout_node()` 与
   flex 求解器两处。
3. **`letter-spacing` / `word-break` / `text-overflow` 都要动 `ql_text_wrap()`**。
   断行只有一份实现，布局（数行算高度）与绘制（逐行发射字形）**共用它** ——
   这是"布局与绘制对行数有分歧"这类 bug 的唯一防线。改它必须两边同时验。

---

## 21. CSS 能力包（编译期裁剪）

**与标签完全同构**：HTML5 那边一个"包"= 一组标签 + 支撑它们的引擎；
CSS 这边一个"包"= 一组属性（含值级别的子特性）+ 支撑它们的绘制/布局代码。

### 21.1 为什么要包

| 理由 | 说明 |
|---|---|
| **省 Flash** | 阴影（SDF）、毛玻璃（独立着色器）、渐变（逐像素/顶点插值）加起来是实打实的几千字节 |
| **省 RAM** | 关掉 `CONTROL` 就没有输入框池；关掉 `ANIM` 就没有过渡池与关键帧表 |
| **构建期报告的单位** | `qltext cc` 能直接说"这份界面需要 `CSS_FLEX` + `CSS_SHADOW`，而你的板子没开 `CSS_SHADOW`" |
| **确定性** | 板子上不会出现"PC 上有阴影、板子上没有"这种无法解释的差异 —— 要么全有，要么**明确报错** |

### 21.2 包表

> **现状（老实说）**：与标签那边一样，除了 `QL_FEAT_IMG_*` 之外，下表宏**还只有名字、
> 没进代码**。这张表是"裁剪口径"，不是今天能 `-D` 的东西。
> 每个包开始落地时，按 §21.7 的验收逐条验。

"足迹"是关键词命中的行数（**下限**，不含没提到关键词的辅助代码），
用来判断"这个包值不值得单独裁"。

| 包 | 宏 | 含什么（属性 / 值子特性） | PC/移动 | MCU | 依赖 | 足迹 |
|---|---|---|---|---|---|---|
| `CSS_BASE` | 恒开 | 盒模型、`display`（block/inline/inline-block/none）、`width/height`、`margin/padding`、`border`（solid）、`position`、`z-index`、`overflow`、`color`/`background-color`、`font-size`/`font-family`、`text-align`/`line-height`/`white-space`/`vertical-align`、`text-decoration` | ✅ | ✅ | 无 | — |
| `CSS_FLEX` | `QL_FEAT_CSS_FLEX` | `flex` 全家（direction/wrap/grow/shrink/basis/justify-content/align-items/gap） | ✅ | ❌（单列够用） | BASE | 113 |
| `CSS_RADIUS` | `QL_FEAT_CSS_RADIUS` | `border-radius`；关掉后 **SDF 圆角退化成直角填充** | ✅ | ✅ | BASE | 与 SHADOW 共用 SDF |
| `CSS_SHADOW` | `QL_FEAT_CSS_SHADOW` | `box-shadow`（CPU `ql_canvas_fill_shadow` / GPU `mode=2`） | ✅ | ❌ | BASE | 38 |
| `CSS_GRADIENT` | `QL_FEAT_CSS_GRADIENT` | `linear-gradient` / `radial-gradient`（GPU 是顶点插值，零成本） | ✅ | ❌ | BASE | 23 |
| `CSS_BLUR` | `QL_FEAT_CSS_BLUR` | `backdrop-filter: blur()`（**独立着色器 + 两趟 FBO**） | ✅ | ❌ | BASE + 保留式 FBO | 60 |
| `CSS_CLIP` | `QL_FEAT_CSS_CLIP` | `clip-path: circle()`（子树级圆形遮罩，CPU+GL 各一份） | ✅ | ❌ | BASE | 73 |
| `CSS_TRANSFORM` | `QL_FEAT_CSS_TRANSFORM` | `transform: translate/scale/scaleX/scaleY`，含负缩放镜像与文本几何缩放 | ✅ | ❌ | BASE | 80 |
| `CSS_ANIM` | `QL_FEAT_CSS_ANIM` | `transition` + `@keyframes` + `animation`（过渡池、关键帧表、applier） | ✅ | ❌ | BASE | 52 |
| `CSS_TEXT_ADV` | `QL_FEAT_CSS_TEXT_ADV` | **B3 之后**：`letter-spacing` / `word-spacing` / `text-indent` / `text-transform` / `text-overflow` / `word-break` | ✅ | ❌ | BASE | — |
| `CSS_BORDER_STYLE` | `QL_FEAT_CSS_BORDER_STYLE` | **B4 之后**：`dashed` / `dotted` / 四边独立 `border-*` | ✅ | ❌ | BASE | — |
| `CSS_SELECTOR_ADV` | `QL_FEAT_CSS_SELECTOR_ADV` | **B2 之后**：`:nth-child` 等结构伪类、`[attr]`、`::before/::after` | ✅ | ⚠️ 见下 | BASE | 只省解析期 |
| `CSS_IMAGE_FIT` | `QL_FEAT_CSS_IMAGE_FIT` | `object-fit` / `object-position`（只在有图片管线时有意义） | ✅ | ❌ | `QL_FEAT_UI_IMAGE` | 与图像管线共用 |
| `CSS_UI` | `QL_FEAT_CSS_UI` | **B5 之后**：`cursor` / `pointer-events` / `user-select` / `scrollbar-*` | ✅ | ❌ | BASE | — |

### 21.3 三个反直觉的判断

1. **`CSS_SELECTOR_ADV` 是唯一"裁了不省多少"的包**。B2 全部在**构建期**展开
   （`:nth-child` → 内部类、`::before` → 真节点），运行期零成本。
   裁它只省解析器代码（几 KB），**不省 RAM、不省每帧开销**。
   MCU 上更值得的做法是：**保留它**，因为它让"同一份 HTML"能两端跑，
   而省下的那点 Flash 比"改两套 HTML"便宜得多。
2. **`CSS_GRADIENT` 在 GPU 上几乎免费**（顶点颜色插值，不加顶点、不加 draw call），
   CPU 侧才是逐像素成本。所以"给 MCU 留渐变、裁掉阴影"是合理组合 ——
   包之间是**正交**的，不搞"要么全有要么全无"。
3. **`CSS_ANIM` 与省电相关**。关掉它不仅是省代码：没有 `animation` 就没有
   "主循环必须一直醒着"的理由。MCU 上关这个包等于**强制静止界面**，这对电池更实在。

### 21.4 硬约束（与标签 §3 同一条）

| 允许 | 禁止 |
|---|---|
| 在 `QLUIStyle` **末尾追加**字段 | 重排/删除已有字段（`style_struct_size` 会变，blob 跨平台就废了） |
| 用宏裁掉**绘制路径**（`CSS_SHADOW` 关掉后不画阴影） | 用宏裁掉**解析分支**导致同一份 CSS 在两端解释不同 |
| 用宏裁掉**报错文案** | 让同一份 `.qlui` 在两个平台上样式不同 |

**关键点：报错不能因为裁剪而消失。** 关掉 `CSS_SHADOW` 之后写 `box-shadow`，
必须报"本构建未启用 `QL_FEAT_CSS_SHADOW`（阴影）"，而不是静默忽略 ——
否则"PC 上看着对、板子上错位"的老问题会从标签那边原样搬过来。

因此每个包在解析期有**三态**：

| 模式 | 行为 |
|---|---|
| 启用 | 正常解析并生效 |
| `strict`（默认） | **明确报错**，文案点名缺哪个包 |
| `lenient` | 忽略该声明，计入 `doc->degraded_props`（与 `degraded_tags` 并列），`ql_ui_load_text()` 后可查 |

### 21.5 与 blob 的关系

**裁剪不影响 blob 的读写。** 理由与标签一致：结构体字段永远全量、
枚举永远全量，所以：

- 全量构建产出的 `.qlui`，在裁剪构建上**能读、能布局、能画** —— 只是被裁掉的效果不生效；
- 反过来也可以（裁剪构建产出的 blob 在全量构建上读，一切正常）。

这让"PC 上设计、板子上跑"不需要两套产物格式。代价是裁剪构建的 `.rodata` 里
仍有那些属性名字符串（约 700 字节，`k_supported_props[]`）。

### 21.6 预设

与 `QLGL_IMG_PRESET`（`full` / `lean` / `raw-only`）同形，CSS 侧三档：

| 预设 | 开哪些 | 典型目标 |
|---|---|---|
| `full` | 全开 | PC / 移动 |
| `ui-lite` | BASE + FLEX + RADIUS + POSITION + SELECTOR_ADV；关 SHADOW/GRADIENT/BLUR/CLIP/TRANSFORM/ANIM | MPU（嵌入式 Linux，弱 GPU） |
| `micro` | BASE + POSITION；其余全关 | MCU（静止界面，光标 + 文本 + 色块） |

预设只是"一堆 `-D` 的别名"，随时可以用单品开关覆盖 —— 与图像管线完全一致的用法。

### 21.6.1 与 HTML5 能力包的交叉依赖

两边不是独立的 —— 有几个交叉点必须写进构建期检查（`qltext cc` 的报错里点出来）：

| HTML5 包 | 需要哪些 CSS 包 | 说明 |
|---|---|---|
| `CONTROL`（`input`/`select`/`textarea`） | `CSS_BASE` | 输入框的框体、光标都是 BASE 的绘制原语 |
| `IMAGE`（`<img>`） | `CSS_BASE` + 可选 `CSS_RADIUS`（圆角裁图）/ `CSS_IMAGE_FIT`（`object-fit`） | 图像管线自己在 `QL_FEAT_IMG_*` |
| `EMBED` 占位盒 | `CSS_BASE` | 1px 边框 + 居中文字 |
| 任意包 + `box-shadow` / 渐变 / 模糊 / 动画 | 对应的 `CSS_*` | **缺一个就报错**，不静默丢效果 |

> 规则一句话：**HTML5 包决定"这个标签能不能用"，CSS 包决定"它能不能被画得好看"**，
> 两者都不满足时，报错信息里要同时点名（例：
> `<dialog>: 需要 QL_FEAT_UI_CONTROL；它的 box-shadow 需要 QL_FEAT_CSS_SHADOW（本构建未启用）`）。

### 21.7 每个包的验收

一个新包落地时必须同时满足四条，缺一条就是"半个功能"：

1. **关掉能编译**：`-DQL_FEAT_CSS_X=0` 全量测试仍 60/60（用 `lenient` 或不用该特性）；
2. **关掉会报错**：写该属性时错误文案点名包名，且**给出替代方案**；
3. **体积真的降**：`size build/libqlgl.a` 前后对比，降幅记在包里；
4. **两端一致**：包开着时 `test_gl_vs_cpu` 逐像素一致（绘制类包强制）。

---

## 22. 五批计划

排序依据：**先补"真实页面没它写不出来"的，再补"锦上添花"的**；同一批内先做不动
`QLUIStyle` 的（只动解析期），后做动结构体的（要抬 blob 版本 + 重生成 golden）。

| 批 | 内容（对应上面各节） | 归属能力包 | 收益 | 风险 | 动 `QLUIStyle` | 验收 |
|---|---|---|---|---|---|---|
| **B1 值与尺寸** | `min/max-*`、`em`/`rem`/`vw`/`vh`、`calc()`/`min()`/`max()`、`margin: auto`、`inherit`/`initial`/`currentColor`、`!important`、`border-radius` 四角、`aspect-ratio`、`hsl()`、`#rrggbbaa` | BASE（少量进 RADIUS） | 真实页面的**基础语法** | 中：动级联与宽度求解 | **是** → blob v16 | 新 `test_css_units` + golden |
| **B2 选择器** | `:first/last/nth-child(an+b)/only-child/of-type`、`:not()`、`[attr]` 系列、`*`、`::before/::after`+`content`、`:disabled/:checked`、`::placeholder` | `CSS_SELECTOR_ADV` | **兼容性收益最大**（12.5% 声明 + 25% 选择器） | **低**：全在构建期展开，运行期零成本 | **否**（伪元素在构建期变成真节点） | 新 `test_selectors` + golden |
| **B3 文本细节** | `letter-spacing`、`word-spacing`、`text-indent`、`text-transform`、`text-overflow: ellipsis`、`word-break`/`overflow-wrap`、`tab-size`、`text-decoration-*` | `CSS_TEXT_ADV` | 列表、标题、长 URL | 中：动 `ql_text_wrap()`（见 §20.3） | **是** → blob v16 | 扩 `test_text_wrap` / `test_white_space` / `test_textarea` |
| **B4 绘制细节** | `border-style: dashed/dotted`、四边独立 `border-*`、多重/`inset` 阴影、`outline`、`background-size/position`、`object-position`、`text-shadow`、`image-rendering`、`scrollbar-*` | `CSS_BORDER_STYLE` + `CSS_SHADOW` / `CSS_GRADIENT` | 视觉还原度 | **中高**：每个都要 CPU + GL 两份，必须过 `test_gl_vs_cpu` | 部分 | 扩 `test_effects` + `test_gl_vs_cpu` |
| **B5 布局与交互** | `align-self`、`align-content`、`order`、`flex-flow`、`overflow-x/y`、`position: sticky`、`inset`、`visibility`、`cursor`、`pointer-events`、`user-select`、`scroll-behavior` | `CSS_FLEX` 补全 + `CSS_UI` | 表达力 + 桌面手感 | 中：动 flex 求解器与命中测试 | 是 | 扩 `test_flex*` / `test_scroll` |

**另外两项单独立项**（成本与一批相当，但语义是"新子系统"）：

- **`transform: rotate()` / `skew()`**：要一条旋转光栅路径（CPU 逐像素仿射 + GL 顶点变换 +
  字形旋转 + 裁剪盒变换）。做完它，变换才算齐。
- **真 group `opacity`**：要离屏图层（见 §20.1）。现在的 `opacity` 是逐图元降透明。

**建议顺序**：**B1 + B2 一起做**（一次抬 blob 版本、一次重生成 golden），之后 B3 → B4 → B5，
中间可随时插入 `rotate`（对动效类页面它比 B5 更实用）。

---

## 23. 怎么新增一个 CSS 属性（检查清单）

照着走，漏一步就会留下"能解析但没生效"或"只在一个后端生效"的坑：

1. **解析**：`src/ui/qlui_parse.c`
   - 加进 `k_supported_props[]`（否则报"不支持的属性"）；
   - 写 `parse_xxx()`，**错误文案要能定位**（`set_error("%s: ...", where)`）；
   - 若要进关键帧，再加进 `k_keyframe_props[]`；
   - 若是简写，在 `expand_shorthands()` 里展开成长属性（**简写本身不落结构体**）。
2. **字段**：`include/qlgl/qlui.h` 的 `QLUIStyle`
   - **只在末尾追加**；抬 `QL_UI_BLOB_VERSION`；`tools/qltext` 的 Python 侧同步。
3. **生效**：
   - 布局类 → `layout_node()` / flex 求解器；
   - 绘制类 → `src/ui/qlui.c` 的 `render_node()`，且**同时**改 `src/draw/qldraw2d.c`（CPU）
     与 `src/port/qlgfx_gl.c`（GL）。
4. **测试**：
   - 语义类：新建/扩 `test/web/test_*.c`，**命中与不命中两侧都断言**；
   - 绘制类：扩 `test_effects`（CPU 像素）+ 跑 `test_gl_vs_cpu`（两后端逐像素一致）。
5. **golden**：`test_golden` 基准图会变，重生成并**逐张看差异**（别一把 `cp` 过去）。
6. **文档**：本文对应行改状态；`QLUI阅读指南` 补"实现要点/踩坑"；教程给个最小例子。
7. **裁剪**：要能被 MCU 裁掉就加 `QL_FEAT_CSS_*` 宏，按 §21 的口径裁**绘制路径**，
   并走 §21.7 的四条验收（关掉能编译 / 关掉会报错 / 体积真的降 / 两端一致）。
8. **归包**：把新属性填进 §21.2 的包表。**新建包之前先问"它能不能并进已有的包"** ——
   包太多会让"给板子选开关"变成一件苦差事。

---

## 24. 验证策略

| 层次 | 手段 | 现在有没有 |
|---|---|---|
| 解析 / 级联 / 选择器 | 每个特性一个最小用例，**命中与不命中都断言** | 部分（`test_web_ui` / `test_text_style`） |
| 布局 | 几何断言（盒子的 x/y/w/h） | `test_flex*` / `test_flex_auto` / `test_render` |
| CPU 绘制 | 像素断言 | `test_effects`（26 项） |
| GPU 绘制 | `test_gl_vs_cpu`：同一份 doc 两后端逐像素比对 | ✅ |
| 回归 | `test_golden`：28 个用例 × blob 逐字节 + 基准图容差 | ✅ |
| 真实页面 | `example/01..13`，每加一批就挑一个例程用上新特性 | ✅ |
| 性能 | `benchmark/bench --scene all`（8 个维度 × 2 后端 × full/dirty） | ✅ |

---

## 25. 相关文档

- [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) —— 标签侧的同口径清单
- [QLUI 阅读指南](QLUI阅读指南.html) —— 样式 / 布局 / 绘制的实现细节与踩坑
- [Web 兼容设计](../../design/Web兼容设计.html) —— 分层与"为什么不做 JS / Grid"
- [开发规划](../../design/开发规划.html) —— 批次排期与验收
