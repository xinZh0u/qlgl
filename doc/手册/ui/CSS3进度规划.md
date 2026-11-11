# CSS 进度规划

## 章节分布（三份文档）

这份文档只装**排期**：已完成 / 待做、批次与排序依据。逐条状态见
[CSS 覆盖清单](CSS3覆盖与裁剪.html)，落地细节见 [CSS 开发日志](CSS3开发日志.html)。
章节编号沿用原文档（外部引用按 § 号定位）。

---

## 0.1 已完成 vs 待做（2026-11-10 更新）

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
| `visibility` / `pointer-events` / `align-self` / `order` / `flex-flow` | blob v23（见 §18.4） |
| `border-style` / `outline` | blob v25（见 §18.5） |
| 四边 `border-*` / 四角 `border-*-radius` / 两层 + inset 阴影 / `text-shadow` / `image-rendering` / `scrollbar-*` / `object-position` | blob v26（见 §18.6） |
| `word-spacing` / `tab-size` / `text-decoration-*` / `:disabled` / `:enabled` / `:checked` / `::placeholder` / `background-image: url()` | blob v27（见 §18.7） |
| **Grid**（`display: grid` 全家 + 具名区域 + `fr`/`minmax()`/`repeat()`/自动放置/跨格/对齐）、**`border-image`** 九宫格 | blob v28（见 §18.9） |
| `calc()` / `min()` / `max()` / `clamp()` | 编译成**后缀字节码**存进文档，布局期求值（见 §18.3）；标量乘除在构建期折掉 |
| **离屏图层**：`filter` 全家 / 真 group `opacity` / `mix-blend-mode` / `mask` | blob v29（见 §18.10） |
| **旋转光栅**：`transform: rotate()` / `skew()` / `skewX/Y` / `matrix()` / `transform-origin` | blob v30（见 §18.11） |
| **表格**：`display: table` 全家 + `<table>` 系列标签 + `table-layout: auto/fixed` + `colspan`/`rowspan` + `border-spacing`/`collapse` + `<colgroup>/<col>` | blob v31（见 §18.12） |
| **多栏**：`column-count` / `column-width` / `columns` / `column-gap` / `column-rule*`（贪心分栏 + 等高平衡） | blob v32（见 §18.13） |
| **文本栈**：`@font-face` / 自由 `font-family` 列表 + 逐字回退 / 真字重与真斜体 / HarfBuzz 整形（连字、字距、复杂文字）/ `direction` + RTL（fribidi 或内建 UAX#9） | blob v33（见 §18.14） |
| **`clip-path` 四种形状**（`circle` / `ellipse` / `inset` / `polygon`）、**纯颜色 `filter` 单批次**（3×4 矩阵）、`small-caps` / `tabular-nums`、`text-align: justify`、阴影 `spread`、椭圆 `border-radius`、`font` 简写、`word-wrap`、`:empty`、`revert` | blob v34（见 §18.15） |

**还没做**，分两类看（下表仍标 ⏳ / ❌ 的就是全部）：

| 类 | 项 | 归宿 |
|---|---|---|
| ~~**PC/Mobile 必做**~~ **✅ B9 已完成（2026-11-11）** | `clip-path` 的 `inset`/`ellipse`/`polygon`（§18.15）；`font-variant: small-caps` / `tabular-nums`（§9.1）；纯颜色 `filter` 单批次（§14） | §18.15 |
| **小件** —— **✅ B9 也已全部落地** | `text-align: justify`、阴影 `spread`、椭圆 `border-radius`（`10px / 20px`）、`font` 简写、`word-wrap` 别名、`:empty`、`revert` 全部 ✅；`::first-line` / `::first-letter` 保持 ❌（明确报错并给替代写法） | 各节 ✅ |
| **不排期** | 3D 变换（`perspective()` / `rotate3d()`）；bidi 的完整算法之外的冷门项；打印相关 | 各节 ❌ |

五批（B1–B5）已全部落地，见 §18.5 / §18.6 / §18.7；B6–B8 见 §22。
下表已逐行对齐当前实现（`qltext check` 实测）。

---

---

## 22. 批次计划（B1–B8 + M9.3/M9.4 已完成）

排序依据：**先补"真实页面没它写不出来"的，再补"锦上添花"的**；同一批内先做不动
`QLUIStyle` 的（只动解析期），后做动结构体的（要抬 blob 版本 + 重生成 golden）。

**B1–B5** 是"补细节"（已完成）；**B6–B8** 是"补架构"（PC/Mobile 档，MCU 归能力包）；
**M9.3 / M9.4** 是两轮"新光栅路径"（离屏图层 → 旋转），已完成，见 §18.10 / §18.11。

| 批 | 内容（对应上面各节） | 归属能力包 | 收益 | 风险 | 动 `QLUIStyle` | 验收 |
|---|---|---|---|---|---|---|
| **B1 值与尺寸**（已完成：`min/max-*`、`!important`、`hsl()`、`#rrggbbaa`、`currentColor/inherit/initial`、`em`/`rem`/`vw`/`vh`/`vmin`/`vmax`、`margin: auto`、`aspect-ratio`；**剩 `border-radius` 四角**） | `min/max-*`、`em`/`rem`/`vw`/`vh`、`calc()`/`min()`/`max()`、`margin: auto`、`inherit`/`initial`/`currentColor`、`!important`、`border-radius` 四角、`aspect-ratio`、`hsl()`、`#rrggbbaa` | BASE（少量进 RADIUS） | 真实页面的**基础语法** | 中：动级联与宽度求解 | **是** → blob v16 | 新 `test_css_units` + golden |
| **B2 选择器**（✅ 已完成：结构伪类 / `[attr]` 系列 / `:not()` / `*` / `::before`+`::after`+`content` / `:disabled` / `:enabled` / `:checked` / `::placeholder`（含复选框控件），见 §18.7） | `:first/last/nth-child(an+b)/only-child/of-type`、`:not()`、`[attr]` 系列、`*`、`::before/::after`+`content`、`:disabled/:checked`、`::placeholder` | `CSS_SELECTOR_ADV` | **兼容性收益最大**（12.5% 声明 + 25% 选择器） | **低**：全在构建期展开，运行期零成本 | **否**（伪元素在构建期变成真节点） | `test_pseudo_text` / `test_selectors` + golden |
| **B3 文本细节**（✅ 已完成：`letter-spacing` / `text-indent` / `text-transform` / `text-overflow: ellipsis` / `word-break`（v20）、`word-spacing` / `tab-size` / `text-decoration-*`（v27，见 §18.7）） | `letter-spacing`、`word-spacing`、`text-indent`、`text-transform`、`text-overflow: ellipsis`、`word-break`/`overflow-wrap`、`tab-size`、`text-decoration-*` | `CSS_TEXT_ADV` | 列表、标题、长 URL | 中：动 `ql_text_wrap()`（见 §20.3） | **是** → blob v20 | 新 `test_text_adv`（25 项，含像素层）+ golden |
| **B4 绘制细节**（✅ 已完成：`border-style: dashed/dotted` + `outline`（§18.5）、四边独立 `border-*`、四角 `border-*-radius`、两层 + `inset` 阴影、`text-shadow`、`image-rendering`、`scrollbar-*`、`object-position`（§18.6）、`background-image: url()` + `size/position/repeat`（§18.7）。**`filter` 已由 M9.3 补上**（§18.10） | `border-style: dashed/dotted`、四边独立 `border-*`、多重/`inset` 阴影、`outline`、`background-size/position`、`object-position`、`text-shadow`、`image-rendering`、`scrollbar-*` | `CSS_BORDER_STYLE` + `CSS_SHADOW` / `CSS_GRADIENT` | 视觉还原度 | **中高**：每个都要 CPU + GL 两份，必须过 `test_gl_vs_cpu` | 部分 | 扩 `test_effects` + `test_gl_vs_cpu` |
| **B5 布局与交互**（✅ 已完成：`align-self`/`align-content`/`order`/`flex-flow`/`overflow-x/y`/`sticky`/`inset`/`visibility`/`pointer-events`/`cursor`/`user-select`/`scroll-behavior`，见 §18.4 与 §18.4.1） | 上列十二项 | `CSS_FLEX` 补全 + `CSS_UI` | 表达力 + 桌面手感 | 中：动 flex 求解器与命中测试 | 是 | 扩 `test_flex*` / `test_scroll` / `test_b5` |

| **B6 布局广度** ✅ **已完成（2026-11-10）** | **Grid**（`display: grid` 全家、`fr`/`minmax()`/`repeat()`/具名区域/自动放置/跨格/对齐）、**`border-image`** 九宫格 | `CSS_GRID` + `CSS_BORDER_IMAGE` | Bootstrap/Tailwind 栅格、卡片墙 | 中高：新求解器（§11 有实测对比） | **是** → blob v28（`QLUIStyle` 272 → 312） | `test_grid`（74 项，含跨列/跨行/区域/负线号/9 条报错）+ 例程 29/30 |
| **B7 数据排版 ✅ 2026-11-11** | **表格**（`table` 全家 + `<table>` 系列标签 + `table-layout: auto/fixed` + `colspan/rowspan` + `border-collapse` + `<colgroup>/<col>`）✅、**多栏**（`column-count/width/rule`，贪心分栏 + 等高平衡）✅ | `CSS_TABLE` ✅ + `CSS_COLUMNS` ✅ | 后台管理界面、长文正文 | 表格：实测 5,332 B；多栏：实测 640 B（不做真分栏，所以没有"排两遍"） | 是 → blob v31/v32（`QLUIStyle` 344 → 364、`QLUINode` 28 → 30） | `test_table`（78 项）+ `test_columns`（34 项）+ 例程 33/34 ✅ |
| **B8 文本栈 ✅ 2026-11-11** | `@font-face`、自由 `font-family` + 逐字回退、真字重/斜体、连字与字距、`direction`/`unicode-bidi` + RTL 排布 | `CSS_FONT_STACK` ✅ / `CSS_TEXT_SHAPE` ✅ / `CSS_BIDI` ✅ | **多语言真实页面**（中文、阿拉伯语、印度语系） | 中：HarfBuzz / fribidi 走**运行期 dlopen**，不是链接依赖；MCU 上三个开关 +0 B | 是 → blob v33（`QLUIStyle` 364 → 368） | `test_font_stack`（38 项）+ `test_text_shape`（27 项）+ 例程 35 ✅ |

**另外几项单独立项**（成本与一批相当，但语义是"新子系统"）：

| 项 | 目标档 | 为什么单独立项 | 成本 |
|---|---|---|---|
| ~~**离屏图层**（`filter: blur()` / 真 group `opacity` / `mix-blend-mode`）~~ | ✅ 已完成（M9.3） | 三者共用同一块能力："子树先合成到一张图，再整体处理"（见 §18.10） | 实测 Flash +15,344 B / RAM +676 B |
| ~~**`transform: rotate()` / `skew()` / `matrix()`**~~ | ✅ 已完成（M9.4） | 复用离屏图层：合成后做一次仿射重采样（见 §18.11） | 实测 Flash +3,488 B（开着图层包时）/ +804 B（只有解析器） |
| ~~**`filter` 的颜色矩阵族**~~ ✅ **B9 已完成** | PC/Mobile | 折成 3×4 矩阵挂到 draw2d 上，**不进离屏图层**（见 §18.15.2） | CPU + GL 各一份，已落地 |
| ~~**`clip-path` 的任意形状**~~ ✅ **B9 已完成**（`inset`/`ellipse`/`polygon`；`path()` 仍 ❌） | PC/Mobile | `inset` 求交、`ellipse` 扩 SDF、`polygon` 走解析覆盖率（见 §18.15.1） | 已落地 |
| **3D 变换**（`perspective()` / `rotate3d()`） | ❌ 不排期 | 要透视除法 + 深度排序 + 一个真正的合成器 | 极高，且与"一个节点一个盒"冲突 |

**建议顺序**：B1–B9 + M9.3/M9.4 **全部完成**。§22 上面"单独立项"那栏里的
`clip-path` 任意形状、`filter` 颜色矩阵、`font-variant` 的 small-caps/tabular-nums
**已经在 B9 一起做完了**（见 §18.15）；剩下的是 3D 变换（明确不排期）与 `path()`。
每一批都要同时给出"MCU 档裁掉后的数字"（§21.2 的"足迹"列），这是本库的老规矩。

---
