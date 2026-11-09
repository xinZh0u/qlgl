# CSS3 覆盖与裁剪

这份文档回答两个问题，口径与 [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) 一致：

1. **CSS3 本库到底支持到哪一步？** —— 按模块逐条给状态，不支持的写清报错原文。
2. **哪一批先做？** —— 按"收益 / 风险 / 是否动 `QLUIStyle`"排成五批。

一句话承诺：

> **布局与绘制的骨架已经齐了**（盒模型、flexbox、定位、圆角、阴影、渐变、变换、过渡、动画）；
> 缺的是**真实页面天天用的细节**（`min/max-*`、`em/rem`、`:nth-child`、`::before`、
> `letter-spacing`、`text-overflow: ellipsis`、`border-style`、`margin: auto`…）。
> 加这些是"补细节"，不是"补架构"。

---

## 1. 记号

| 记号 | 含义 |
|---|---|
| ✅ | 已支持（解析 + 生效） |
| ⚠️ | 支持但有语义偏差（文档里写明差在哪） |
| ⏳ | 计划做（本文 §5 给了批次） |
| ❌ | 明确不做（写了就**编译期报错**，不静默降级） |

本库的一条红线没变：**不支持的东西必须报错并给行号**。下面所有 ❌/⏳ 项，
今天的表现都是 `ql_ui_text_last_error()` 里一句能读懂的话，而不是"静默忽略、排版错位"。

---

## 2. 全量清单

### 2.1 选择器

| 选择器 | 状态 | 备注 / 报错 |
|---|---|---|
| `tag` / `.class`（可重复）/ `#id` | ✅ | |
| `a b`（后代）、`a > b`（子代）、`a + b`（相邻兄弟） | ✅ | |
| `a ~ b`（一般兄弟） | ✅ | |
| `:hover` `:active` `:focus` | ✅ | 构建期编译成状态变体样式表，运行期只切下标 |
| `:disabled` `:checked` | ⏳ | 状态位机制已就绪，差"变体样式 + 判定" |
| `:first-child` `:last-child` `:nth-child(an+b)` `:only-child` | ⏳ | `不支持的伪类 ':first-child'（只支持 :hover :active :focus）` |
| `:not(<简单选择器>)` | ⏳ | 同上 |
| `[attr]` / `[attr=v]` / `[attr^=]` `$=` `*=` | ⏳ | `不支持的简单选择器：'[data-x]'` |
| `::before` / `::after` + `content: "文本"` | ⏳ | 今天是 `不支持的属性 'content'` |
| `::first-line` `::selection` `:root` `:nth-of-type` 等 | ❌ | |
| `@media` / `@supports` | ❌ | 见 §6 |

> 选择器全部在**构建期**展开：`:nth-child` 结算成"给第 n 个孩子加一个内部类"，
> `::before` 直接生成一个真节点。**运行期零成本**，这是 B2 风险最低的原因。

### 2.2 盒模型与尺寸

| 属性 | 状态 | 备注 / 报错 |
|---|---|---|
| `width` `height` | ✅ | `px` / `%` / `auto` |
| `margin` `padding`（1~4 值简写 + 四个方向） | ✅ | **只支持 px**；负 margin 已支持 |
| `margin: 0 auto`（水平居中） | ⏳ | `margin/padding 只支持 px` |
| `padding: 5%` | ⏳ | `此处不支持百分比 '5%'` |
| `border-width` `border-color` `border-radius` | ✅ | 四边统一；`border-radius` 单值（px 或 %） |
| `border-top-width` / `border-left-color` / `border-bottom: none` | ⏳ | `不支持的属性 'border-top-width'` |
| `border-radius: 4px 8px 12px 16px`（四角） | ⏳ | `不支持的长度 '...'（只支持 px 与 %）` |
| `min-width` `max-width` `min-height` `max-height` | ⏳ | `不支持的属性 'min-width'` |
| `box-sizing` | ✅ | 表单控件 UA 默认 `border-box` |
| `width: fit-content` / `min()` / `max()` / `calc()` | ⏳ | `不支持的长度 'fit-content'` |
| `aspect-ratio` | ⏳ | |
| `overflow` `overflow-x` `overflow-y` | ⚠️ | `overflow` 有 `visible/hidden/auto/scroll`；**没有分轴**（`overflow-x` 报错） |

### 2.3 值与单位

| 方式 | 状态 | 备注 |
|---|---|---|
| `px` / `%` / `auto` | ✅ | |
| 无单位倍数（`line-height: 1.5`） | ✅ | 仅 `line-height` |
| `em` / `rem` | ⏳ | `不支持的长度 '1.2em'（只支持 px 与 %）` |
| `vh` / `vw` / `vmin` / `vmax` | ⏳ | 根元素的 `%` 已按视口解析，`vh/vw` 是同一件事的语法糖 |
| `deg` | ⚠️ | 只出现在渐变方向（`90deg`/`180deg`） |
| `inherit` / `initial` / `currentColor` | ⏳ | `不支持的颜色写法 'inherit'` |
| `!important` | ⏳ | `不支持的 display 'block !important'` |
| 命名色 / `#rgb` / `#rrggbb` / `rgb()` / `rgba()` | ✅ | 命名色见 `k_named[]`（CSS 常用色 + `transparent`） |
| `hsl()` / `#rrggbbaa` | ⏳ | |

### 2.4 flexbox

| 属性 | 状态 |
|---|---|
| `display: flex` / `inline-flex` | ✅ |
| `flex-direction` | ✅ `row` / `row-reverse` / `column` / `column-reverse` |
| `flex-wrap` | ✅ `nowrap` / `wrap` |
| `flex-grow` `flex-shrink` `flex-basis` / `flex: 1 1 0px` | ✅ |
| `justify-content` | ✅ 含 `space-between` / `space-around` / `space-evenly` |
| `align-items` | ✅ 含 `stretch` |
| `gap` `row-gap` `column-gap` | ✅ |
| `align-self` | ⏳ |
| `align-content`（多行交叉轴分配） | ⏳ |
| `order` | ⏳ |
| `flex-flow` 简写 | ⏳ |

### 2.5 定位

| 属性 | 状态 | 备注 |
|---|---|---|
| `position: static / relative / absolute / fixed` | ✅ | |
| `top` `right` `bottom` `left` | ✅ | |
| `z-index` | ✅ | |
| `position: sticky` | ⏳ | `不支持的 position 'sticky'` |

### 2.6 背景与边框绘制

| 属性 | 状态 | 备注 |
|---|---|---|
| `background-color` / `background`（纯色） | ✅ | |
| `linear-gradient()` | ⚠️ | **必须写方向**：`to right` / `to bottom` / `90deg` / `180deg`；`to bottom right` 报错 |
| `radial-gradient()` | ✅ | 需要 3 个参数（位置, 起色, 止色） |
| `background-image: url(...)` | ⏳ | `只支持 linear-gradient() / radial-gradient()`；图片请用 `<img>` |
| `background-size` / `position` / `repeat` | ⏳ | |
| `border-style: solid` | ✅ | **只有 solid**；`dashed` 报错 |
| `box-shadow`（单条） | ✅ | CPU SDF + GPU `mode=2` 各一份实现 |
| 多重 `box-shadow` / `inset` | ⏳ | `box-shadow 里无法识别 ','` |
| `outline` | ⏳ | |
| `opacity`（普通属性） | ⏳ | **只有 `@keyframes` 里能用**；见 §6 的语义说明 |
| `filter: blur()` | ⏳ | 只有 `backdrop-filter: blur()` |
| `mix-blend-mode` | ❌ | |

### 2.7 文本

| 属性 | 状态 | 备注 |
|---|---|---|
| `color` | ✅ | |
| `font-size` | ✅ | `px` / `%`（相对父级）/ `smaller` / `larger` |
| `font-family` | ⚠️ | 只有 `DEFAULT` / `MONO` 两族（位图字体 + 矢量字体各一套），不是自由字体列表 |
| `font-weight` `font-style` | ⚠️ | **合成**：加粗 = 画两遍右移 1px，斜体 = 按行剪切（MCU 上不加载第二份字体） |
| `text-decoration` | ✅ | `underline` / `line-through` |
| `text-align` | ✅ | `left` / `center` / `right` |
| `line-height` | ✅ | `normal` / `px` / 无单位倍数 |
| `white-space` | ✅ | `normal` / `nowrap` / `pre` / `pre-wrap` / `pre-line` |
| `vertical-align` | ✅ | `baseline` / `sub` / `super` 等 |
| `letter-spacing` `word-spacing` `text-indent` | ⏳ | `不支持的属性` |
| `text-transform` | ⏳ | |
| `text-overflow: ellipsis` | ⏳ | 列表里最常用的一个，配合 `overflow: hidden` + `white-space: nowrap` |
| `word-break` / `overflow-wrap` | ⏳ | 长 URL 必须靠它 |
| `text-shadow` | ⏳ | |
| `list-style` / `::marker` | ❌ | 没有生成内容，符号请自己写字符 |

### 2.8 变换、过渡、动画

| 属性 | 状态 | 备注 |
|---|---|---|
| `transform: translate()` / `scale()` / `scaleX()` / `scaleY()` | ✅ | 支持负缩放（镜像），行高按垂直轴缩放 |
| `transform: rotate()` / `skew()` / `matrix()` | ⏳ | `只支持 translate()/scale()/scaleX()/scaleY()`；rotate 要新的旋转光栅路径 |
| `transition`（时长 / 属性 / 缓动） | ✅ | 可过渡：`background-color` `opacity` `color` `border-color` `border-radius` `transform` `clip-path` |
| `transition-delay` | ⏳ | |
| `@keyframes` + `animation` 全 8 个子属性 | ✅ | 关键帧在编译期摊平成静态表 |
| `clip-path: circle(n%)` | ✅ | 只有 `circle()`，一层 |
| `object-fit` | ✅ | `fill` / `contain` / `cover` 等 |

---

## 2.9 覆盖率实测（2026-11-09）

口径：拿本机 23 份**真实 CSS**（Bootstrap 5、mkdocs 主题、Mercurial 的四套主题、
matplotlib web_backend、Font Awesome、VLC 的 jQuery UI 皮肤）当语料，
抽出全部声明后按属性名逐条对照 `k_supported_props[]`。
去掉 CSS 自定义属性（`--x`）与厂商前缀（`-webkit-` 等）后剩 **5391 条声明**。

| 口径 | 结果 | 说明 |
|---|---|---|
| **属性名级** | **57 / 150 = 38%** | 去重后的属性名，不考虑用得多不多 |
| **声明级（推荐口径）** | **3968 / 5391 = 73.6%** | 按真实出现次数加权 —— 常用的那些基本都有 |
| **能直接照搬** | **3305 / 5391 = 61.3%** | 属性支持 **且** 值里不含 `!important`/`var()`/`calc()` |

分模块（同一个语料，按出现次数加权）：

| 模块 | 覆盖 |
|---|---|
| flexbox | **99.8%** |
| 定位 / 层叠 | **100%** |
| 文本 | **98.6%** |
| 盒模型 / 尺寸 | **95.2%** |
| 背景 / 边框绘制 | **82.7%** |
| 变换 / 动画 | **100%**（语料里用得少，只有 43 条） |

按文件看：Mercurial 的四套主题 89~95%、mkdocs 主题 84%、Bootstrap 80.5%、
**Font Awesome 8.8%**（整份就是 `content` + `::before`，见下）。

### 缺口到底在哪

| 来源 | 声明数 | 占比 | 归属 |
|---|---|---|---|
| `content`（图标字体） | 706 | 13.1% | **B2**（`::before` + `content` 一次解决） |
| `max-width` / `min-width` / `max-height` | 65 | 1.2% | **B1** |
| `border-top/right/bottom/left`（四边独立） | 106 | 2.0% | **B4** |
| `border-*-radius`（四角） | 94 | 1.7% | **B1** |
| `!important`（出现在**已支持**的属性里） | 293 | 5.4% | **B1** |
| `var(--x)`（CSS 变量） | 537 | 10.0% | ❌ 明确不做 |
| `calc()` | 65 | 1.2% | **B1** |

### 补完之后能到多少（模拟）

| 阶段 | 声明级覆盖 |
|---|---|
| 现在 | **73.6%** |
| + B1 值与尺寸 | 76.5% |
| + B2 选择器（`content`/`::before`） | **89.6%** |
| + B3 文本细节 | 90.0% |
| + B4 绘制细节 | **94.0%** |
| + B5 布局细节 | 94.5% |

剩下那 5.5% 主要是 `float`（不做）、`cursor`（鼠标形状，与本库无关）、
`list-style`（没有生成内容）、`visibility`、`pointer-events`、`@font-face` 的 `src`。

### 选择器侧

| 特性 | 全部语料 | 去掉图标字体库 |
|---|---|---|
| `::before` / `::after` | 57.4% | **25.3%** |
| `:nth-` / `:first-` / `:last-child` | 4.0% | **7.3%** |
| `[attr]` 系列 | 2.2% | **3.9%** |
| `:hover` / `:active` / `:focus` | 3.9% | 7.0%（已支持） |

> 图标字体库把 `::before` 的占比顶得很高（57%），去掉之后是 25% —— 也就是说
> **B2 的 `::before`/`content` 是"用一行代码换 25% 的选择器"那一档**。

### 三道"属性全支持也过不去"的墙

就算属性面做到 94%，把一份手写页面直接搬进来仍然会卡在这三处（都是**明确不做**）：

| 墙 | 语料中出现 | 后果 |
|---|---|---|
| `var(--x)` | 537 条声明 | 写 `var()` 就报错。Bootstrap 5 一半的样式靠它 |
| `@media` | 191 个块 | 响应式断点整段不认 |
| `!important` | 293 条声明 | 值里带它就报错（B1 补上后消失） |

所以**"CSS3 兼容度"取决于你要搬的是什么**：

- **自己按本库能力写新页面**：现在就能到 **85~90% 的手感**（常用属性都有，
  只是不能写 `var()`/`@media`）；
- **直接搬 Bootstrap 这类框架**：约 **80% 的声明**能过，剩下的要靠改写成
  固定值（去掉 `var()` 与 `@media`）；
- **搬图标字体库**：现在 9%，B2 之后接近 100%。

---

## 3. 哪些"看起来很简单"的东西其实不简单

这三处值得单独说，免得当成"漏了"：

1. **`opacity` 只能做近似**。真正的 CSS `opacity` 是**组不透明度**：整棵子树先合成到
   一张离屏图层，再整体混合。本库现在只有"元素自己的背景 alpha"，也就是关键帧路径的
   做法。做真 group opacity 要在 CPU 与 GL 两端各加一层离屏合成 —— 成本和
   `transform: rotate()` 同级，属于独立一项，不该顺手塞进"补属性"里。
2. **`margin: auto` 是个布局概念，不是值解析**。它要求块级盒在**有确定可用宽度**时
   把剩余空间分给左右 margin；flex 里的 `auto` margin 又是另一套（吸收剩余空间）。
   改动落在 `layout_node()` 的宽度求解与 flex 求解器两处，不是加一行解析。
3. **`!important` 落在级联层**。本库的样式在构建期就摊平了（优先级已在 §选择器里算过），
   加 `!important` 意味着声明层要多存一个比特、比较函数要多一级。成本低但要动
   `QLUIStyle` 的生成路径与 golden，适合和 B1 一起做。

---

## 4. 与 MCU 裁剪的关系

CSS 的处理方式与标签一致：**属性名与结构体字段永远全量，裁的是"会不会去用"**。

| 允许 | 禁止 |
|---|---|
| 在 `QLUIStyle` **末尾追加**字段 | 重排/删除已有字段（`style_struct_size` 会变，blob 跨平台就废了） |
| 用宏裁掉**绘制路径**（如 `QL_FEAT_CSS_SHADOW` 关掉后阴影不画） | 用宏裁掉**解析分支**导致同一份 CSS 在两端解释不同 |
| 用宏裁掉**报错文案** | 让同一份 `.qlui` 在两个平台上样式不同 |

> 注意：解析期报错**不能**因为裁剪而消失。MCU 上关掉 `QL_FEAT_CSS_SHADOW` 之后，
> 写 `box-shadow` 仍然要报"本构建未启用阴影"，而不是静默忽略 —— 否则
> "PC 上看着对、板子上错位"的老问题会从标签那边原样搬过来。

---

## 5. 五批计划

排序依据：**先补"真实页面没它写不出来"的，再补"锦上添花"的**；同一批内先做不动
`QLUIStyle` 的（只动解析期），后做动结构体的（要抬 blob 版本 + 重生成 golden）。

| 批 | 内容 | 收益 | 风险 | 动 `QLUIStyle` | 验收 |
|---|---|---|---|---|---|
| **B1 值与尺寸** | `min/max-width/height`（含 %）、`em`/`rem`、`margin: auto`、`inherit`/`initial`/`currentColor`、`!important`、`border-radius` 四角、`aspect-ratio` | 真实页面的**基础语法**：响应式需要一个上限、居中需要 `auto` | 中：动级联与宽度求解 | **是**（min/max 四组 + radius 四角）→ blob v16 | `test_css_units`(新) + golden |
| **B2 选择器** | `:first/last/nth-child(an+b)/only-child`、`:not()`、`[attr]` 系列、`::before/::after`+`content`、`:disabled/:checked` | 兼容性收益最大；斑马纹、图标、状态样式全靠它 | **低**：全在构建期展开，运行期零成本 | **否**（伪元素在构建期变成真节点） | `test_selectors`(新，逐选择器命中/不命中) + golden |
| **B3 文本细节** | `letter-spacing`、`word-spacing`、`text-indent`、`text-transform`、`text-overflow: ellipsis`、`word-break`/`overflow-wrap` | 列表、标题、长 URL —— 日常页面高频 | 中：要动 `ql_text_wrap()`（布局与绘制共用同一份断行，是红线） | **是**（设位 + 字距）→ blob v16 | 扩 `test_text_wrap` / `test_white_space` |
| **B4 绘制细节** | `border-style: dashed/dotted`、四边独立 `border-*`、多重/`inset` 阴影、`outline`、`background-size/position/repeat`、`filter: blur()` | 视觉还原度 | **中高**：每个都要 CPU + GL 两份实现，必须过 `test_gl_vs_cpu` | 部分（`border_style` 位、背景参数） | `test_effects` 扩 + `test_gl_vs_cpu` |
| **B5 布局细节** | `align-self`、`align-content`、`order`、`overflow-x/y` 分轴、`position: sticky` | 布局表达力 | 中：动 flex 求解器与滚动容器 | 是 | `test_flex*` 扩 |

**另外两项单独立项**（成本与一批相当，但语义是"新子系统"）：

- **`transform: rotate()`**：要一条旋转光栅路径（CPU 的逐像素仿射 + GL 的顶点变换，
  还得处理字形与文字的旋转、裁剪盒的变换）。做完 `clip-path` 之外的变换才齐。
- **真 group `opacity`**：要离屏图层。

### 建议顺序

**B1 + B2 一起做**，一次抬 blob 版本、一次重生成 golden。
理由：B2 纯构建期、零运行期成本、兼容性收益最大；B1 是"没有它就没法写真实页面"的地基。
之后按 B3 → B4 → B5，中间可以随时插入 `rotate`（它对动效类页面比 B5 更实用）。

---

## 6. 明确不做

| 不做 | 理由 |
|---|---|
| `display: grid` | 实现成本接近 flex 的两倍；本库的 flex 已能覆盖绝大多数卡片网格（`flex-wrap` + `gap`） |
| `float` 布局 | 与 flex 混用行为诡异，现代页面基本不用 |
| CSS 变量 `--x` / `var()` | 会让"运行期样式"变复杂，破坏构建期预计算的收益（这是本库最核心的取舍） |
| `@media` / `@supports` | 目标设备尺寸在构建期已知；真需要多套布局，请用两套 doc 或由宿主切换 |
| `list-style` / `::marker` | 没有生成内容机制；符号请自己写字符（`•`） |
| `mix-blend-mode` / `background-clip: text` | 都要离屏图层或专用着色器，收益远小于成本 |
| 嵌套 `::before` / 多层 `clip-path` | 只有一层够用，多层会引入"裁剪栈"这个新概念 |

> 这一节的落地方式：写到就**构建期报错**，文案点名"本库不做 X，替代方案是 Y"。
> 静默降级是"PC 上看着对、板子上错位"的根源。

---

## 7. 怎么新增一个 CSS 属性（检查清单）

照着这个顺序走，漏一步就会留下"能解析但没生效"或"只在一个后端生效"的坑：

1. **解析**：`src/ui/qlui_parse.c`
   - 加进 `k_supported_props[]`（否则报"不支持的属性"）；
   - 写 `parse_xxx()`，**错误文案要能定位**（`set_error("%s: ...", where)`）；
   - 若要进关键帧，再加进 `k_keyframe_props[]`。
2. **字段**：`include/qlgl/qlui.h` 的 `QLUIStyle`
   - **只在末尾追加**；抬 `QL_UI_BLOB_VERSION`；`tools/qltext` 的 Python 侧同步。
3. **生效**：
   - 布局类 → `layout_node()` / flex 求解器；
   - 绘制类 → `src/ui/qlui.c` 的 `render_node()`，且**同时**改 `src/draw/qldraw2d.c`（CPU）
     与 `src/port/qlgfx_gl.c`（GL）两条路径。
4. **测试**：
   - 语义类：新建/扩一个 `test/web/test_*.c`，用**命中与不命中**两侧断言；
   - 绘制类：扩 `test_effects`（CPU 像素）+ 跑 `test_gl_vs_cpu`（两后端一致）。
5. **golden**：`test_golden` 的基准图会变，重生成并逐张看差异（别一把 `cp` 过去）。
6. **文档**：本文 §2 改状态；`QLUI阅读指南` 补"实现要点/踩坑"；教程里给个最小例子。
7. **裁剪**：如果要能被 MCU 裁掉，加 `QL_FEAT_CSS_*` 宏，按 §4 的口径裁**绘制路径**。

---

## 8. 验证策略

| 层次 | 手段 | 现在有没有 |
|---|---|---|
| 解析/级联 | 每个特性一个最小用例，**命中与不命中都断言** | 部分（`test_web_ui`/`test_text_style`） |
| 布局 | 几何断言（盒子的 x/y/w/h） | `test_flex*` / `test_layout` |
| CPU 绘制 | 像素断言（`test_effects` 已有 26 项） | ✅ |
| GPU 绘制 | `test_gl_vs_cpu`：同一份 doc 两后端逐像素比对 | ✅ |
| 回归 | `test_golden`：28 个用例 × blob 逐字节 + 基准图容差 | ✅ |
| 真实页面 | `example/01..13` 每加一批就挑一个例程用上新特性 | ✅ |

---

## 9. 相关文档

- [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) —— 标签侧的同口径清单
- [QLUI 阅读指南](QLUI阅读指南.html) —— 样式/布局/绘制的实现细节与踩坑
- [Web 兼容设计](../../design/Web兼容设计.html) —— 分层与"为什么不做 JS/Grid"
- [开发规划](../../design/开发规划.html) —— 批次排期与验收
