# CSS 覆盖与裁剪（全量清单）

这份文档回答三个问题，口径与 [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) 一致：

1. **CSS 的每一样东西，本库支持到哪一步？** —— 按模块穷举，一条不漏。
2. **不支持的为什么不做？** —— 每条给一个**原因码**（§2），不写"暂不支持"这种废话。
3. **哪一批先做？** —— 按"收益 / 风险 / 是否动 `QLUIStyle`"排批次（§22）。

## 0. 一份代码，两档目标（先读这一节）

从这一版起，"支持 / 不支持"**按平台分开回答**。两档的差距不是程度问题，是数量级问题：

| | **PC / Mobile 档** | **MCU 档** |
|---|---|---|
| 目标 | **尽可能完整兼容 HTML/CSS**，第三方库（FreeType / HarfBuzz / SheenBidi / libpng…）该上就上 | **确定性性能 + 自由裁剪**，一个能力包一行 `-D` 就能摘掉 |
| Flash | 不设上限（MB 级随便用） | 典型 M4 板 256 KB~1 MB；当前全量 **131.5 KB**（§2.1） |
| RAM | 几十 MB 随便用 | 几十~几百 KB |
| 帧率 | 60~144 fps，掉一帧要查原因 | 30 fps 够用，宁可少画也不掉帧 |
| 布局算法 | **该有的都要有**：Grid、表格、多栏、`border-image` | 按能力包裁剪；**但 Grid 反而是减负**（§11） |
| 文本 | 自由字体列表 + 真整形（连字/字距/双向） | 内建 ASCII 位图字体（≈9 KB），或裁剪后的子集 TTF |
| "不做"的东西 | 只剩**与"构建期定型"根本冲突**的（运行期 CSS 匹配、DOM/JS 引擎） | 按需裁剪，不是"不做" |

**因此本文里的 ❌ 只有三种含义**：① 与"构建期定型"冲突（`P`）；② 与 GUI 无关（`N`）；
③ 已明确排到某个后续批次（`⏳`）。**"MCU 上装不下"不再是 ❌ 的理由** ——
那属于能力包（§21）的裁剪口径，PC/Mobile 档照样全做。

一句话现状：

> **布局与绘制的骨架已经齐了**（盒模型、flexbox、定位、圆角、阴影、渐变、变换、过渡、动画），
> 五批（B1–B5）把"真实页面天天用的细节"补完了。
> 下一阶段的重点是**PC/Mobile 档的兼容广度**：Grid（B6）、表格与多栏（B7）、
> 第三方文本栈（B8：FreeType + HarfBuzz + 双向）、`filter` / `rotate`（独立立项）。
> MCU 档同步只做一件事：**把这些新能力都做成可裁的能力包**。

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

**还没做**，分两类看（下表仍标 ⏳ / ❌ 的就是全部）：

| 类 | 项 | 归宿 |
|---|---|---|
| **PC/Mobile 必做**（MCU 归能力包） | Grid、`border-image`（B6）；表格、多栏（B7）；`@font-face` + 真字体栈、连字整形、双向（B8）；离屏图层（`filter` / 真 group `opacity` / `mix-blend-mode` / `mask`）；`transform: rotate()`/`skew()`/`matrix()`；`clip-path` 的 `inset`/`ellipse`/`polygon` | §22 |
| **小件**（随时插队） | `text-align: justify`、阴影 `spread`、椭圆 `border-radius`（`10px / 20px`）、`font` 简写、`word-wrap` 别名、`::first-line` / `::first-letter`、`:empty`、`revert` | 各节 ⏳ |
| **不排期** | 3D 变换（`perspective()` / `rotate3d()`）；bidi 的完整算法之外的冷门项；打印相关 | 各节 ❌ |

五批（B1–B5）已全部落地，见 §18.5 / §18.6 / §18.7；B6–B8 见 §22。
下表已逐行对齐当前实现（`qltext check` 实测）。

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

**平台维度**（§0）：每行还有一个"档位"标记，说明它在两档里各是什么下场。

| 档位 | PC / Mobile | MCU |
|---|---|---|
| **双档** | ✅ 默认开 | ✅ 默认开（`CSS_BASE`） |
| **包裁** | ✅ 默认开 | ⚙️ 由能力包控制（§21），可整块摘掉 |
| **大核** | ✅ 默认开 | ⚙️ 默认**关**（Grid / 表格 / 多栏 / `border-image` / 第三方文本栈） |
| **仅 PC** | ✅ | ❌ 恒关（离屏图层类：`filter`、真 group `opacity`、`backdrop-filter` 的完整链） |

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
| **P** | **与"静态节点 / 构建期定型"冲突**（本库最核心的取舍） | `content: counter()`、`@supports`、`@layer` |
| **S** | **要一个新的排版/绘制子系统**（不是"加个属性"，是一次立项） | Grid 列轨道、表格列宽分配、多栏分裂、离屏图层 |
| **G** | **要第三方库**（FreeType / HarfBuzz / SheenBidi…）：PC/Mobile 档上，MCU 档裁 | 真字体列表、连字整形、双向文本、彩色 emoji |
| **N** | **收益极低 / 已被取代 / 与 GUI 无关** | `float`、`text-emphasis`、打印属性 |

> 旧版的 `M`（"与 MCU 目标冲突"）**已废除**：MCU 的体积问题是能力包（§21）解决的问题，
> 不是"不做"的理由。原来标 `M` 的行现在分别是 `S`（Grid / 表格 / 多栏 / `border-image`）
> 与 `G`（`@font-face` / 多变体字体 / 双向），并且**都进了后续批次**（§22）。

---

### 2.1 两档的实测数字（2026-11-10，本机）

这些数字是决定"哪些能力配 MCU 档"的依据，都是**本轮实测**，不是估值。

| 项目 | 平台 / 配置 | 数值 |
|---|---|---|
| 全量固件 `text` | Cortex-M4 Thumb-2，`-O2`，240×160 RGB565（`test/mcu`） | **139,260 B**（含 B6） |
| 同上，关掉图像管线（`QL_FEAT_IMG_*=0`） | 同上 | 131,144 B（**−8,116 B，−6.2%**） |
| 同上，关掉 Grid（`QL_FEAT_CSS_GRID=0`） | 同上 | 134,732 B（**−4,528 B，−3.3%**） |
| 同上，再关 border-image（`QL_FEAT_CSS_BORDER_IMAGE=0`） | 同上 | 132,556 B（**−2,176 B，−1.6%**） |
| 内建 ASCII 位图字体（12px + 16px，全字节表） | 同上 | ≈9 KB |
| 布局：21 节点（`test/mcu` 的 bench 页） | QEMU `mps2-an386` `-icount` @200 MHz 折算 | **0.42 ms** |
| 布局：12 节点 | 同上（同一批，相对比较有效） | 0.23 ms |
| 布局：39 节点 | 同上 | 0.82 ms |
| 布局：75 节点 | 同上 | 1.94 ms |
| **布局边际成本** | 同上（三点拟合） | **≈4.4k 指令/节点 ≈ 22 µs/节点** |
| **多一层 wrapper 容器**（15 节点嵌套 vs 12 节点扁平，视觉相同） | 同上 | **+0.20 ms（+86%）**，一层 ≈ 3 个叶子节点 |
| 光栅一帧 240×160 RGB565（21 节点页） | 同上 | **7.99 ms**（≈42 周期/像素） |
| 重排 | x86-64 `-O3`（`bench --scene scroll`，302/902/1802 节点拟合） | **0.53 µs/节点** |
| 整帧 `cards` 800×480 / 362 节点 | 同上，CPU 后端 | 0.72–0.77 ms |
| `libharfbuzz.so` `text` | x86-64 系统库 | **1,178 KB** |
| `libfreetype.so` `text` | 同上 | **813 KB** |
| `libfribidi.so` `text` | 同上 | 106 KB |
| 一份全量 CJK TTF | `uming.ttc` | **21 MB**（GB2312 子集化后典型 200–400 KB） |

> **口径**：MCU 那一列按 `test/mcu/README.md` 的换算——QEMU `-icount` 下"虚拟 ns ≈ 指令数"，
> 换到 M4@200 MHz 乘 5。这是**乐观下界**（M4 无 cache，实际 IPC 常 0.6~0.8），
> 真实板子上乘 1.5~2 更接近。库体积（`text`）是 ELF 实测，不含栈与堆。
>
> **横比优先于绝对值**：绝对值会随 QEMU 版本 / 编译选项漂（本机两次测量差过 ~30%），
> 但"同一批、同一台机器、连着跑"的相对比较是可用的 —— 下文所有"快 xx%"都是这么来的。

三条能直接推出结论的读数：

1. **MCU 档的瓶颈是 Flash，不是"布局算法多"**：一个 HarfBuzz 就是当前整个固件的
   8.9 倍；一份全量 CJK 字体是 160 倍。所以"字体栈"必须按 `G` 处理（PC 上，MCU 裁），
   而**布局算法（Grid/表格）加起来也就几 KB**，够不上一个数量级。
2. **Grid 在 MCU 上不是负担，是减负**：见 §11 的实测对比 —— 用嵌套 flex 拼 3×3
   网格要 0.427 ms 布局，换成 Grid（同一视觉）只要 ≈0.229 ms，**省 46%**，
   因为省掉了 3 个 wrapper 容器（每层 ≈3 个叶子节点的代价）。
3. **PC/Mobile 档完全不缺这点性能**：整帧 0.72 ms 的预算里，加一个 Grid 求解器
   增加的是"每容器一次 O(行×列) 的加法"，量级在 µs；真正的成本只有代码体积（MB 级预算里的几 KB）。

### 2.2 两档"能不能加"的判据

给一个新属性/新能力，按这个顺序问：

```
① 与"构建期定型"冲突吗？        → 是：P（不做，任何档）
② 与 GUI 无关吗？               → 是：N（不做，任何档）
③ 要第三方库吗？                → 是：G（PC/Mobile 上，MCU 裁）
④ 要新子系统吗？                → 是：S → 进 §22 的批次；PC/Mobile 必做，MCU 归"大核"包（默认关）
⑤ 只是"多解析一个值 / 多画一种线"？ → 直接做，进对应能力包（§21）
```

**没有"因为 MCU 装不下所以不做"这一条了。** MCU 装不下就裁；
PC/Mobile 装得下就必须做 —— 这是这次重新定档的核心变化。

---

## 3. 选择器与规则

### 3.1 简单选择器

| 选择器 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `tag` | ✅ | B | |
| `.class`（可重复 `.a.b`） | ✅ | B | |
| `#id` | ✅ | B | |
| `*`（通配） | ✅ | B | 构建期展开 |
| `[attr]` | ✅ | B | |
| `[attr=v]` `[attr~=v]` `[attr\|=v]` | ✅ | B | |
| `[attr^=v]` `[attr$=v]` `[attr*=v]` | ✅ | B | |

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
| `:disabled` / `:enabled` / `:checked` | ✅ | B | 三份变体样式（§18.7）；`<input type=checkbox>` 已就位，`:checked` 可直接用 |
| `:first-child` | ✅ | B | 构建期算好兄弟位置（`sib_idx`），运行期零成本 |
| `:last-child` | ✅ | B | |
| `:only-child` | ✅ | B | |
| `:nth-child(an+b)` | ✅ | B | 支持 `N` / `odd` / `even` / `an+b` / `-n+b`；斑马纹的刚需 |
| `:nth-last-child()` | ✅ | B | |
| `:first-of-type` / `:last-of-type` / `:only-of-type` / `:nth-of-type()` / `:nth-last-of-type()` | ✅ | B | 只数**同标签**的兄弟 |
| `:not(<简单选择器>)` | ✅ | B | 不支持嵌套 `:not()` |
| `:root` | ✅ | B | = 元素树最外层第一个元素；变量常写在这里（Bootstrap 的 `:root { --bs-*: … }`） |
| `:empty` | ⏳ | B | 判据明确（没有文本也没有子节点），便宜 |
| `:link` `:visited` | — | N | 本库没有超链接跳转语义（`<a>` 只是行内文本） |
| `:target` `:lang()` `:dir()` | ❌ | P | 需要运行期状态/文档语言上下文 |
| `:has()` `:is()` `:where()` `:focus-visible` `:focus-within` | ❌ | B | L4 选择器；`has()` 会引入"反向依赖"，与构建期定型冲突 |
| `:hover` 之外的交互态（`:active` 之外） | — | N | |

### 3.4 伪元素

| 伪元素 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `::before` + `content: "文本"` | ✅ | B | 构建期生成真节点 |
| `::after` + `content: "文本"` | ✅ | B | 同上 |
| `::first-line` `::first-letter` | ❌ | F | 要"改变行盒内字形"的能力，与逐行排版冲突 |
| `::selection` | ❌ | P | 选区颜色是运行期状态；输入框选区已经硬编码为 `text_color @ 0x50` |
| `::placeholder` | ✅ | B | `placeholder` 属性 + 一份独立占位样式（§18.7） |
| `::marker` | ❌ | P | 没有生成内容机制，符号请自己写字符 |
| `content: counter()` / `attr()` / `url()` | ❌ | P | 需要计数器/属性求值，等于引入一个小运行期 |

### 3.5 @ 规则

| @ 规则 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `@keyframes` | ✅ | B | 编译期摊平成 `QLUIKeyframe[]` / `QLUIAnimation[]` 静态表 |
| `@font-face` | ⏳ | G | **B8**：PC/Mobile 档解析成"字体表条目"，喂给 FreeType/HarfBuzz；MCU 档裁掉（只留内建位图字体） |
| `@media (min-width / max-width)` | ✅ | — | **构建期切成视口宽区间**，每个区间编译一套样式；运行期只按宽度选下标（见 §18.2） |
| `@media` 的其它条件（`orientation` / `prefers-*` / 分辨率 / `em` 单位） | ❌ | P | 需要运行期条件求值；`em` 单位要设备字号，明确报错。`print` 整块跳过 |
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
| `em` | ✅ | L | 相对**元素自己的字号**；构建期就折成 px（字号那时已确定），运行期零成本 |
| `rem` | ✅ | L | 相对**根元素字号**；整棵树走完才折（根的 `font-size` 自己也可能是 em/px） |
| `vw` `vh` | ✅ | L | 相对视口；只有布局期才知道视口多大，所以在 `resolve_length()` 里解析（窗口缩放自动跟着变） |
| `vmin` `vmax` | ✅ | L | 相对视口**短边 / 长边**；同样在布局期解析 |
| `s` / `ms` | ✅ | T | `transition-duration` / `animation-duration` |
| `deg` | ⚠️ | L | 只在渐变方向里（`90deg` / `180deg`），不进 `transform` |
| `turn` / `rad` / `grad` | ❌ | N | 只有旋转用得上，而 `transform: rotate()` 还没做 |
| `fr` | ⏳ | L | Grid 专用，随 B6 一起做（§11）；flex 里写了报错 |
| `ch` / `ex` / `lh` / `rlh` / `cap` | ❌ | N | 都要"字体度量 × 系数"，收益极低 |
| `calc()` | ✅ | L | `+ - * /` 与括号；标量乘除在构建期折进每个操作数（见 §18.3） |
| `min()` / `max()` / `clamp()` | ✅ | L | 参数是长度表达式，可嵌套 `calc()` |
| `fit-content` / `max-content` / `min-content` | ⏳ | L | 布局内部已有 max-content 计算，只是没暴露成关键字 |
| 命名色 | ✅ | R | `k_named[]`（CSS 常用色 + `transparent`） |
| `#rgb` / `#rrggbb` | ✅ | R | |
| `rgb()` / `rgba()` | ✅ | R | |
| `#rrggbbaa` | ✅ | R | |
| `hsl()` / `hsla()` | ✅ | R | |
| `hwb()` / `lab()` / `oklch()` / `color()` | ⏳ | R | 纯换算，无副作用；上屏是 8 位色，宽色域没有意义 |
| `inherit` | ✅ | R | |
| `initial` / `unset` | ✅ | R | 回到 UA 默认值 |
| `revert` | ⏳ | R | 层级语义差别很小，暂报错 |
| `currentColor` | ✅ | R | 解析期替换成 `color` 的值 |
| `!important` | ✅ | L | 级联两趟：普通先应用，important 再应用 |
| 转义 / CSS 注释 | ✅ | B | `/* … */` 已支持 |

---

## 5. 盒模型与尺寸

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `display` | ⚠️ | L | 支持 `block` `inline` `inline-block` `flex` `inline-flex` `none`；不支持 `grid` `table` |
| `width` | ✅ | L | |
| `height` | ✅ | L | |
| `min-width` | ✅ | L | px / `%`（blob v16） |
| `max-width` | ✅ | L | 同上 |
| `min-height` | ✅ | L | |
| `max-height` | ✅ | L | |
| `aspect-ratio` | ✅ | L | `宽 / 高` 或单个数字；只补"缺的那一边"（高确定推宽、宽度撑满推高），两边都写了以具体值为准 |
| `margin` + `-top/right/bottom/left` | ✅ | L | px / em / rem（构建期折 px）；`auto` 见下一行；负值可用 |
| `margin: … auto` | ✅ | L | **横向**把剩余空间分掉（`margin: 0 auto` 居中）；纵向 auto 按 0 处理（本库还不做外边距合并） |
| `padding` + 四方向 | ✅ | L | px / em / rem；`%` 与 `vw/vh` 报错（边距的百分比相对包含块宽度，算边距时还没有基准） |
| `box-sizing` | ✅ | L | 表单控件 UA 默认 `border-box` |
| `overflow` | ✅ | L | `visible` / `hidden` / `auto` / `scroll`（简写 = 两个轴都写） |
| `overflow-x` / `overflow-y` | ✅ | L | 分轴裁剪；只有 `auto` 的那一轴能滚、能画滚动条 |
| `visibility` | ✅ | L | `visible` / `hidden`（`collapse` 按 hidden）；**继承**，子级可写 `visible` 显示回来 |
| `pointer-events` | ✅ | L | `auto` / `none`：自己不参与命中，子节点照常 |
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
| `position: sticky` | ✅ | L | 布局末尾的一次补偿平移（`top/right/bottom/left` 哪个给了贴哪边），约束在包含块里 |
| `inset`（简写） | ✅ | L | `inset: <t> <r> <b> <l>`（1/2/4 值），展开成四条长写 |
| `top` / `right` / `bottom` / `left` | ✅ | L | |
| `z-index` | ✅ | L | |

---

## 7. 背景与边框

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `background-color` | ✅ | R | |
| `background`（简写） | ⚠️ | R | 只认**纯色或单个渐变**；写成 `background: #fff url(a.png)` 会报错 |
| `background-image: linear-gradient()` | ⚠️ | R | **必须写方向**（`to right` / `to bottom` / `90deg` / `180deg`）；`to bottom right` 报错 |
| `background-image: radial-gradient()` | ✅ | R | 需要 3 个参数（位置, 起色, 止色） |
| `background-image: url(...)` | ✅ | R | 与 `<img src>` 共用资源表，按名字注册（§18.7） |
| `background-size` | ✅ | R | `cover` / `contain` / `<w> <h>`；渐变不受影响（渐变的尺寸就是元素尺寸） |
| `background-position` | ✅ | R | `left/center/right + top/center/bottom` 与 px 混写 |
| `background-repeat` | ✅ | R | `repeat` / `no-repeat` / `repeat-x` / `repeat-y` |
| `background-attachment` / `-clip` / `-origin` | ❌ | N | 在"界面元素"上用不到 |
| `border`（简写） | ✅ | R | `<width>px <style> <color>`，三段任意顺序 |
| `border-width` | ✅ | R | 1 / 2 / 4 个值（上右下左） |
| `border-color` | ✅ | R | 1 / 2 / 4 个颜色（上右下左） |
| `border-style` | ✅ | R | `solid` / `dashed` / `dotted` / `none`。虚线是**沿边切段填矩形**（不碰 SDF，CPU 与 GL 同一条路径），图案尺寸跟线宽走（dashed 段 3w 缝 2w、dotted 点 1w 缝 1w）。已知差异：与 `border-radius` 同用时按直角画；`double` / `groove` 等仍报错 |
| `border-{top,right,bottom,left}`（单边简写） | ✅ | R | 与 `border` 同一套 token 判别，写进那一边的长写属性 |
| `border-*-width` / `-style` / `-color` | ✅ | R | 单边长写；布局按四边各自的宽度内缩内容盒 |
| `border-radius`（1 / 2 / 4 个值） | ✅ | R | `4px` 四角同；`4px 8px` 是"左上右下 / 右上左下"；四个值按 左上 右上 右下 左下。≥ 短边一半时是胶囊/内切圆 |
| `border-{corner}-radius` | ✅ | R | 四角长写，可单独覆盖 |
| `border-radius` 斜杠椭圆角 | ⏳ | R | `10px / 20px` 报错：椭圆角要改两套 SDF，收益低 |
| 四边不同色 + 圆角 | ⚠️ | R | 四条边各自裁一次整圈描边；**角部归上/下边**（浏览器沿 45° 斜线切分） |
| `border-image` + `-source` / `-slice` / `-width` / `-repeat`（`stretch`/`repeat`/`round`/`space`） | ✅ | R | **已完成（B6）**：九宫格切片，复用 `blit_image(dst, src)`（§18.9） |
| `border-image-outset` | ❌ | N | 报错：边框图不向盒子外扩 |
| `box-shadow`（单条） | ✅ | R | CPU SDF + GPU `mode=2` 各一份实现 |
| `box-shadow`（多重 / `inset`） | ✅ | R | 最多**两层**（多写的报错，不静默丢弃）；`inset` 用反向 SDF（GPU `mode=7`）。`spread`（展开半径）报错 |
| `text-shadow` | ✅ | R | 一层（多层的语义是叠色）；`blur` 用 5 次采样近似（0~16px） |
| `outline` + `-width/-style/-color/-offset` | ✅ | R | 画在边框盒**外侧**、**不占布局**（focus 环标配）；`outline-offset` 可为负（压进边框里）。CSS 语义照抄：只写 `outline-width` 不画（`outline-style` 初值 `none`），要画就写 `outline: 2px solid <色>` |
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
| `font-family` | ⚠️ | G | 现在是 `DEFAULT` / `MONO` 两族（矢量 + 内建位图）。**B8 起 PC/Mobile 档变成自由列表**（`@font-face` 注册）；MCU 档仍是两族 |
| `font-size` | ✅ | L | `px` / `%`（相对父级）/ `smaller` / `larger` |
| `font-weight` | ⚠️ | G | 现在是**合成**加粗（画两遍右移 1px）。**B8 起 PC/Mobile 档**按 `font-weight` 选真实字重（`@font-face` + FreeType）；MCU 档保持合成 |
| `font-style` | ⚠️ | G | 现在是**合成**斜体（按行剪切）。**B8 起 PC/Mobile 档**加载真斜体字面；MCU 档保持合成 |
| `font`（简写） | ⏳ | L | 目前要分开写 |
| `font-variant` / `font-stretch` / `font-feature-settings` | ⏳ | G | **B8**：`small-caps` / `tabular-nums` 等由 HarfBuzz 特性位实现；MCU 档裁 |
| `font-size-adjust` / `font-kerning` / `font-optical-sizing` | ❌ | X | 同上 |
| `@font-face` | ⏳ | G | **B8**，见 §3.5 |

### 9.2 文本

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `text-align` | ✅ | L | `left` / `center` / `right`（没有 `justify`） |
| `text-align: justify` | ❌ | B | 要改行内分配算法；中文排版里常用但成本不低，且断行是布局/绘制共用的一份实现 |
| `line-height` | ✅ | L | `normal` / `px` / 无单位倍数 |
| `white-space` | ✅ | L | `normal` / `nowrap` / `pre` / `pre-wrap` / `pre-line` |
| `vertical-align` | ⚠️ | L | **只有 `baseline` / `sub` / `super`**；`middle` / `text-top` / 长度值都报错（`<sub>`/`<sup>` 够用） |
| `text-decoration` | ✅ | R | `underline` / `overline` / `line-through` / `none`（可叠加）；`wavy` 报错 |
| `text-decoration-color/-style/-thickness` | ✅ | R | 形态 `solid` / `double` / `dotted` / `dashed`；不写颜色/粗细就跟文字色、跟字号 |
| `letter-spacing` | ✅ | L | 加在**每个字符之后**（含最后一个，与 CSS 一致）；只吃 px / em / rem，`%` 与 `vw/vh` 报错 |
| `word-spacing` | ✅ | L | 只加在空格之后；量宽与绘制共用一份推进 |
| `text-indent` | ✅ | L | 只作用在**整段第一行**（可用宽度减一截，绘制起点右移）；px / em / rem |
| `text-transform` | ✅ | B | `uppercase` / `lowercase` / `capitalize`；**建表期**改文本（与空白折叠同一处）。输入框/多行输入跳过（值是运行期的） |
| `text-overflow: ellipsis` | ✅ | L | 一行比盒子宽时末尾换 `…`（字体没有 U+2026 时退回三个点）；**没有裁切盒也成立** —— 它只是"少画几个字"，不像浏览器必须配 `overflow: hidden` |
| `word-break: break-all` / `overflow-wrap: break-word` | ✅ | L | 与原来的"只在空格/CJK 处断"共用一份折行；`keep-all` 目前等同 `normal` |
| `word-wrap` | ⏳ | L | 就是 `overflow-wrap` 的旧名字，加个别名即可 |
| `hyphens` | ❌ | X | 要连字词典（各语言各一份） |
| `text-shadow` | ✅ | R | 复用 `box-shadow` 的 SDF 路径（字形级） |
| `text-justify` / `text-align-last` | ❌ | X | 见 `justify` |
| `direction` / `unicode-bidi` | ❌ | X | RTL 需要 bidi 算法 + 整形（HarfBuzz/SheenBidi），是独立立项 |
| `writing-mode` / `text-orientation` | ❌ | X | 竖排同样要重整排版引擎 |
| `text-emphasis` / `ruby-align` / `ruby-position` | ❌ | X | 注音排版；`<ruby>` 标签本身也还没做 |
| `tab-size` | ✅ | L | 1~32；**建表期**把 `\t` 展开成 N 个空格 |
| `text-underline-offset` | ✅ | R | px / em / rem，可负 |
| `text-decoration-skip-ink` | ❌ | N | 微观排版，收益极低 |
| `-webkit-line-clamp` | ❌ | N | 非标准；多行截断可以用固定高度 + `overflow: hidden` 近似 |

### 9.3 生成内容

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `content`（纯文本） | ✅ | B | 只能写在 `::before` / `::after` 上，构建期生成真节点 |
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
| `align-self` | ✅ | L | 覆盖自己在交叉轴上的对齐；`auto` = 跟容器的 `align-items` |
| `align-content` | ✅ | L | 多行交叉轴分配：stretch（默认）/ start / center / end / space-between / space-around / space-evenly |
| `order` | ✅ | L | 就地排子链表，布局/绘制/命中一起跟着走（稳定，相等时保持 HTML 顺序） |
| `flex-flow`（简写） | ✅ | L | `<flex-direction> \|\| <flex-wrap>`，两个 token 任意顺序 |
| `wrap-reverse` | ❌ | N | 用得极少，`flex-direction` 反向可近似 |
| `gap` / `row-gap` / `column-gap` | ✅ | L | Grid 也用同一组，但本库只在 flex 里生效 |

---

## 11. Grid（B6 ✅ 已完成，MCU 归"大核"包）

**这一块从"不做"改成"要做"、并且已经做完了**，理由是实测出来的：

| 同一视觉：3×3 卡片格（240×160） | 节点数 | 布局耗时 @200 MHz |
|---|---|---|
| 嵌套 flex（3 个 `.row` 各 3 张卡） | 15 | **0.427 ms** |
| 扁平 flex（`flex-wrap: wrap` + 固定宽） | 12 | **0.229 ms** |
| *Grid 版（1 个容器 + 9 张卡，无 wrapper）* | *12* | *≈0.229 ms*（与扁平同量级） |

也就是说：**Grid 在 MCU 上比"用嵌套 flex 拼"更快**，因为它省掉了 wrapper 层
（实测一层 wrapper ≈ 3 个叶子节点的代价）。它唯一的成本是 Flash 里的求解器代码，
和 `layout_node`（6.1 KB）同量级 —— 在一个 MB 级预算的 PC 档里可以忽略。

| 属性 | 状态 | 档位 | 备注 |
|---|---|---|---|
| `display: grid` / `inline-grid` | ✅ | 大核 | 单层轨道求解（不支持 `subgrid`） |
| `grid-template-columns` / `-rows` | ✅ | 大核 | `<长度>` / `%` / `fr` / `auto` / `min-content` / `max-content` / `minmax()` / `repeat(<n>, …)` |
| `grid-template-areas` | ✅ | 大核 | 具名区域；**必须是矩形**（非矩形构建期报错） |
| `grid-auto-columns` / `-rows` | ✅ | 大核 | 单个长度值（`auto` = 内容） |
| `grid-auto-flow` | ⚠️ | 大核 | `row` / `row dense` 支持；**`column` 报错**（列流要转置整张网格，见 §19.1） |
| `grid-row` / `grid-column` / `grid-area`（含 `span`） | ✅ | 大核 | 线号 / `span n` / `auto` / 区域名 / `区域名-start|end` / 负线号 |
| `justify-items` / `justify-self` / `place-items` / `place-self` | ✅ | 大核 | 与现有的 `align-*` 对称；`place-*` 只取第一个词 |
| `gap` / `row-gap` / `column-gap` | ✅ | 双档 | flex 与 grid 共用同一组字段 |
| 具名网格线 `[name]` | ❌ | — | `S`：报错并提示改用 `grid-template-areas` |
| `grid` / `grid-template` 简写 | ❌ | — | `N`：报错，分开写长写即可 |
| `subgrid` / `masonry` | ❌ | — | `S`：要"子网格反向依赖父轨道"，收益远小于成本 |

**为什么现在不做、但进批次**：它是一个**独立求解器**（轨道定尺 → 放置 → 对齐三段），
与 flex 那套"主轴/交叉轴"不是同一份代码，所以要按"新子系统"立项（`S`）。
PC/Mobile 档缺它就说不上"兼容现代页面"——Bootstrap 5 / Tailwind 的栅格都建在它上面。

**MCU 档**：`QL_FEAT_CSS_GRID` 默认 **0**。裁掉之后 `display: grid` 报错并提示用 flex。

---

## 12. 变换

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `transform: translate()` | ✅ | R | 加到绘制偏移上，**不进布局**（零重排） |
| `transform: scale()` | ✅ | R | 允许负值（几何压扁 + 镜像） |
| `transform: scaleX()` / `scaleY()` | ✅ | R | 假 3D 翻转就靠它 |
| `transform: rotate()` | ⏳ | **F** | **单独立项，PC/Mobile 必做**（§22）：要旋转光栅路径（CPU 逐像素仿射 + GL 顶点变换 + 字形旋转）。MCU 档恒关 |
| `transform: skew()` | ⏳ | F | 同上（与 `rotate` 同一份仿射路径，一起做几乎不加成本） |
| `transform: matrix()` / `matrix3d()` | ⏳ | F | `matrix()` 随之解锁（`rotate`/`skew` 落地后只是"拆矩阵"）；`matrix3d()` 归 3D，不排期 |
| `transform: perspective()` / `rotate3d()` 等 3D | ❌ | S | 要真 3D 管线（透视除法 + 深度排序）。PC 档也**不排期**：`scaleX` 已能近似"翻面"，而 3D 需要一整套矩阵 + 分层合成 |
| `transform-origin` | ⏳ | R | 现在固定绕元素中心；随 `rotate` 一起做（有旋转才需要原点） |
| `transform-style` / `backface-visibility` | ❌ | S | 3D 专用，同上 |

---

## 13. 过渡与动画

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `transition`（简写） | ⚠️ | T | 可过渡：`background-color` `opacity` `color` `border-color` `border-radius` `transform` `clip-path`。逗号分组支持，但**各段必须共用同一组时长/缓动**（库里只有一份 `trans_ms`/`trans_ease`），不同时长报错 |
| `transition-property` | ✅ | T | `all` 或上面那批，可列多个（位图） |
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
| `filter: blur()` | ⏳ | O | **单独立项"离屏图层"，PC/Mobile 必做**（§22）：要"元素自己离屏后再模糊"，与 `backdrop-filter` 不是一回事。MCU 档恒关 |
| `filter: brightness/contrast/saturate/grayscale/sepia/invert/hue-rotate/drop-shadow` | ⏳ | O | 同样进"离屏图层"立项；颜色矩阵那几族也可以先做"单色乘法融进绘制入口"的便宜版 |
| `clip-path: circle()` | ✅ | R | 子树级圆形遮罩；CPU 与 GPU 各一份 |
| `clip-path: inset()` / `ellipse()` / `polygon()` / `path()` | ⏳ | F | PC/Mobile 排期：`inset()`/`ellipse()` 便宜（SDF 扩展），`polygon()`/`path()` 要扫线 |
| `mask` / `mask-image` / `-webkit-mask` | ⏳ | O | 进"离屏图层"立项（A8 通道 + 逐像素乘） |
| `mix-blend-mode` | ⏳ | O | 进"离屏图层"立项：混合模式要求"读到下层已画好的像素"，与 `backdrop-filter` 同一条件 |

---

## 15. 多栏、分页、打印

| 属性 | 批次 | 档位 | 备注 |
|---|---|---|---|
| `columns` / `column-count` / `column-width` | B7 | 大核 | 列高平衡要"先排一遍量高度、再排一遍落列"，是**唯一真正与"一个节点一个盒"冲突**的一项 |
| `column-rule` / `column-gap` | B7 | 大核 | 画在两列之间的竖线，复用 `border` 的绘制路径 |
| `column-span` / `column-fill` / `break-*` | ❌ | — | `S`：跨列要打断"盒内分裂"的不变量；`break-inside` 在 GUI 里几乎用不到 |
| `page-break-*` / `orphans` / `widows` | — | — | `N`：本库不打印 |
| `@page` / `@media print` | — | — | `N`：同上（`@media print` 目前**整块跳过**，不报错） |

**为什么排到 B7 而不是不做**：长文阅读界面（设置页的帮助、`<article>` 正文）真的会用它，
而且它是纯 CPU 的排版计算，PC 档毫无压力。MCU 档归"大核"包，默认关。

**风险**：`column-count` 的语义要求"列高尽量相等"，意味着**同一份内容要排两遍**
（第一遍测总高、第二遍按目标列高断列）。这是 §2.1 里"布局边际成本 ×2"，所以它比
Grid 贵 —— 但仍是我们自己的代码，没有第三方依赖。

---

## 16. 表格（B7：PC/Mobile 必做，MCU 归"大核"包）

| 属性 | 批次 | 档位 | 备注 |
|---|---|---|---|
| `display: table` / `table-row` / `table-cell` / `table-caption` … | B7 | 大核 | `<table>` `<tr>` `<td>` `<th>` `<thead>` `<tbody>` `<tfoot>` `<caption>` 一并打开 |
| `table-layout: auto` | B7 | 大核 | 列宽按内容分配（要两遍：测 min/max-content，再分配） |
| `table-layout: fixed` | B7 | 大核 | 一遍就够，先做这个 |
| `border-collapse` / `border-spacing` | B7 | 大核 | `collapse` 要"半边边框"相加规则 |
| `caption-side` / `empty-cells` / `vertical-align: middle`（表格单元格内） | B7 | 大核 | |
| 跨行跨列 `rowspan` / `colspan` | B7 | 大核 | 占位格（被跨过的那格不画但占位置） |
| `<colgroup>` / `<col>` | ❌ | — | `N`：用 CSS 给 `td` 写宽度即可 |

**为什么不做了改成"要做"**：数据表格是**后台管理界面**的刚需，
而"用嵌套 flex 拼表格"拼不出列宽对齐 —— 这是真实的表达力缺口，不是省事。
PC/Mobile 档没有任何理由不做；MCU 档归"大核"包（`QL_FEAT_CSS_TABLE` 默认 0）。

**替代方案**：用嵌套 flex 画表格（`display: flex` 每一行，单元格给固定宽度）。

---

## 17. UI 与交互

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `cursor` | ✅ | L | 解析 + 命中测试算出"意图"（`ql_ui_cursor_at()`）；**形状由宿主设**（后端暂无光标钩子） |
| `outline` 系列 | ✅ | R | 见 §7；不占布局，`offset` 可负 |
| `user-select` | ✅ | L | `none` 时输入框不再拖拽连选（本库只有输入框能选字，其余取值等价于可选中） |
| `scroll-behavior: smooth` | ✅ | T | 由 `ql_ui_anim_tick()` 逐帧推进（160ms ease-out），与 CSS 动画同一个时钟 |
| `overscroll-behavior` | ❌ | N | 惯性/回弹是平台层的事 |
| `touch-action` | ❌ | N | 触摸还没接（见开发规划 M6.5） |
| `contain-intrinsic-size` / `will-change` | ❌ | N | 渲染优化提示，本库架构里没有意义 |
| `appearance` | ❌ | N | 表单控件的"原生外观"由 UA 默认样式负责 |
| `zoom` | ❌ | N | 全局缩放由宿主 `ql_disp_set_scale()` / `ql_gfx_gl_set_scale()` 负责 |

---

## 18. 其他模块

| 模块 / 属性 | 状态 | 原因 |
|---|---|---|
| CSS 变量 `--x` / `var()` | ✅ | 构建期替换（见 §18.1）：级联之后再查一次表，`QLUIStyle` 一个字节没变 |
| `all`（重置简写） | ❌ | **P**：它是"整套样式重置"，与构建期逐属性编译的结构冲突 |

### 18.1 CSS 变量为什么能便宜地支持

早期结论是"❌ 明确不做：会让运行期样式变复杂"。**这个理由站不住** —— 本库根本没有
运行期样式表（没有 JS、没有动态改样式的入口），所以变量天然只可能是**编译期**的值：

```
UA 默认 + 级联 + 行内 + 继承  →  out（这个元素最终可见的声明）
out 里先把 `--x` 自己的值解析掉   →  再把所有 var() 换成字面量
```

替换完，`compile_style()` 看到的就是字面量，`QLUIStyle` 与 blob 版本都不用动。
语料里 `var()` 占 **10.0%** 的声明（537 条，Bootstrap 5 一半样式靠它），
是整个"补细节"清单里收益最高的一条。

| 规则 | 实现 |
|---|---|
| `--x` 继承 | `collect_inherited()` 把父级所有 `--*` 带下来 |
| 后代覆盖 | 就是普通级联（`decls_set` 语义） |
| `var(--x, <fallback>)` | 有定义用定义值，否则用 fallback；fallback 里可再套 `var()` |
| 嵌套 `--a: var(--b)` | 先解析 `--x` 自己的值，再解析普通属性 |
| 循环引用 | 深度上限 8，超了报错（不会挂死） |
| 找不到且无 fallback | **构建期报错**（不静默留成字面量） |
| 简写里用（`border: 1px solid var(--c)`） | 简写先按 token 形态展开成长属性，值是 `var(...)` 字符串 → 之后统一替换 |
| 名字大小写 | 属性名统一折小写，所以 `--Brand` 与 `--brand` 是同一个（与 CSS 的"大小写敏感"不同，写进限制） |
| `content: var(--x)` | **报错**：伪元素在级联之前就展开了，那时还没有自定义属性 |
| 运行期改主题 | 不支持 —— 换肤走"换一份 CSS / 热重载"（见 [对比与取舍 §12](../../design/对比与取舍.html)） |

### 18.2 `@media` 也是构建期的事

早期结论是"❌ 目标设备尺寸构建期已知" —— 这条对**定死的板子**成立，但 PC 窗口是可以拖的，
所以现在按"状态变体"的同一套办法做：

```
条件   @media (max-width: 620px) { … }
  ↓ 构建期：把所有 min/max 宽度当分界点，切成互不相交的区间
区间   [0,620]  [621,∞)        ← qlui.h 的 QLUIMedia[]
  ↓ 每个区间编译一套样式（base / hover / active / focus 各一个下标）
运行期 ql_ui_layout(w) 线性扫区间 → 取该区间的变体装回节点
```

| 项 | 支持 |
|---|---|
| `(min-width: Npx)` / `(max-width: Npx)`，可用 `and` 串 | ✅ |
| 前缀 `screen` / `all` | ✅（可省略） |
| `print` | 整块跳过（永远不生效） |
| `(orientation: …)` / `(prefers-color-scheme: …)` / `em` 单位 | ❌ 构建期报错 |
| 区间个数上限 | `QL_CFG_UI_MAX_MEDIA`（默认 6） |

三条实现注意：

1. **多趟编译**：每个区间都要把整棵树按该区间重算一遍样式，所以"每个断点独有的声明"
   必须在每趟开始**清空**（`final_decls` 是"往 out 里合并"的，不清会残留 —— 踩过）；
2. **状态变体要一起装**：`:hover` 也有 per-区间 的下标，否则一悬停就把响应式尺寸丢掉；
3. **媒体变体在 `style_copy` 清理之后装回**：清理会把 `n->style` 复位成最窄区间那份，
   放在它前面会被当场覆盖（表现是"宽的窗口里只有 flex 子项还是窄布局"—— 踩过）。
| `writing-mode` / `text-orientation` | ❌ | **X**：竖排要重整排版引擎 |
| `direction` / `unicode-bidi` | ❌ | **X**：RTL 需要 bidi 算法 + 文本整形 |
| `will-change` / `contain` / `backface-visibility` | ❌ | **N**：GPU 优化提示，自有渲染器不消费 |
| Speech（`speak` / `volume` / `pause`…） | — | **N**：与 GUI 无关 |
| `scrollbar-width` | ✅ | **R**：`auto`(4px) / `thin`(2px) / `none`（不画）。只影响覆盖式滚动条（本库滚动条不占布局） |
| `scrollbar-color` | ✅ | **R**：`<thumb> [track]`，轨道暂不画；`auto` 用默认中灰 |
| `scrollbar-gutter` | ❌ | **N**：本库滚动条不占布局，没有"占位"问题 |
| `scroll-snap-*` | ❌ | **T**：要持续重绘 + 吸附动画 |
| `image-rendering` | ✅ | **R**：`pixelated` 走最近邻；`auto`/`smooth` 交给后端默认。**CPU 后端只有最近邻**，所以这个属性在 CPU 上没有可见差异（GL 放大时才有） |
| `object-fit` | ✅ | **R** | `fill` / `contain` / `cover` 等（`background-size` 的兄弟） |
| `object-position` | ✅ | **R** | 1~2 个值（百分比 / px / `center`/`left`/`right`/`top`/`bottom`）；`cover` 时改的是源矩形（决定露哪一块） |
| `image-orientation` | ❌ | **N** | EXIF 旋转由解码器负责 |

---

### 18.3 `calc()`：为什么不能算成一个数

`calc()` 里可以有 `%`、`vw`、`vh` —— 它们的基准（父内容盒、视口）**只有布局时才知道**。
所以不能像 `em` 那样在构建期折成一个 px 数（`em` 能折，是因为那时字号已定）。

做法：把公式编译成一段**后缀字节码**，与样式分开存放。

```
calc((100% - 16px) / 3)
  ↓ 构建期：先做常量折叠（/3 折进每个操作数）
TERM(33, %)  TERM(5, px)  SUB  END        ← doc->calc_ops[]（每条 4 字节）
  ↓ 样式里只留一个下标
#a { width: 148 }                          ← QLUILength{ .value = 下标, .unit = CALC }
  ↓ 运行期：ql_ui_layout 里求值（一份 16 深的栈，没有递归、没有分配）
33% × 基准 - 5
```

| 项 | 支持 |
|---|---|
| `+` `-` `*` `/`、括号、嵌套 `calc()` | ✅ |
| `min()` / `max()`（2..8 个参数）、`clamp(lo, val, hi)` | ✅ 参数可以是长度表达式 |
| 单位 | `px` / `%` / `em` / `rem` / `vw` / `vh` / `vmin` / `vmax` |
| 能用在哪 | `width` / `height` / `min-width` / `max-width` / `min-height` / `max-height` / `top` / `right` / `bottom` / `left` |
| 只能"常量"的字段 | `font-size` / `border-width` / `border-radius` / `letter-spacing` / `text-indent` / `margin` / `padding`：只接受构建期能折成 px 的 `calc()`（如 `calc(10px + 6px)`），带 `%`/`em`/`vw` 的报错 —— 这些字段存的是 `int16` 数值，没有放表达式下标的地方 |
| 上限 | `QL_CFG_UI_MAX_CALC_OPS`（默认 256 条，0 = 整套裁掉） |

四条实现注意：

1. **标量乘除只能对纯数字**：`calc(10px * 2)` 可以（构建期折成 20px），`calc(10px * 2px)` 报错。
   折进操作数是"逐项乘"，所以 `calc(2 * (100% - 10px))` = `200% - 20px`；
2. **精度按存储单位走**：百分比存整数，所以 `calc(100% / 3)` 是 33%（不是 33.33%）。
   一个像素级别的偏差，比"支持不了"更可接受；
3. **百分比高度仍然守父级确定的规矩**：父级高度 auto 时，`height: calc(50% - 5px)`
   按 `auto` 处理（与 `height: 50%` 一致），不会算出一个负高度；
4. **flex 项主轴上的 `%` / `calc()` 会先折成 px**：`measure` 与真正的 `layout_node`
   各解析一遍的话，`calc(100% - 40px)` 会被减两次（`width: 50%` 也会变成四分之一 ——
   这是本轮顺带修掉的老 bug）。

---

### 18.4 B5 小件：十二个"只加开关"的属性

这一批的共同点是**不动布局大框架**，各自挂到一个已有机制上：

| 属性 | 挂在哪 | 实现要点 |
|---|---|---|
| `visibility: hidden` | 绘制 + 命中 | 只是"不画自己"，布局照常 → 与 `display: none` 的区别就在这里。它是**继承**属性，子级写 `visible` 能显示回来 |
| `pointer-events: none` | 命中测试 | 自己不作为命中目标，**子节点照常**（子级写 `auto` 能重新接住）——盖在按钮上的装饰层用它 |
| `align-self` | flex 交叉轴 | `auto` = 跟容器的 `align-items`；`stretch` 只在交叉轴尺寸是 `auto` 时撑满 |
| `order` | flex 子链表 | 布局开头按 order **稳定** 排一遍子链表，之后测量/断行/摆放/绘制/命中全都自动跟着走（比"每次遍历按 order 挑下一个"少六七处改动） |
| `flex-flow` | 简写展开 | `<flex-direction> \|\| <flex-wrap>`，两个 token 任意顺序；展开进 `flex-direction` / `flex-wrap` |
| `overflow-x` / `overflow-y` | 裁剪 + 滚动 | 分轴只改裁剪矩形的一个方向（另一轴沿用外层裁剪）；只有 `auto` 的轴算可滚范围、才画滚动条 |
| `align-content` | flex 交叉轴 | 多行时按 stretch / start / center / end / space-* 分配行间空间；间距同样用"累计推进"（余数不丢） |
| `position: sticky` | 布局末尾补偿平移 | 在流里占位；滚动平移之后再把自己推回可视边缘，**约束在包含块内**（这就是"跟到头就一起滚走"）。改的是 `border_box`，所以绘制/命中/脏区/`paint_box` 全都自动正确 |
| `inset` | 简写展开 | `inset: <t> <r> <b> <l>` → 四条长写，复用 margin/padding 的 1/2/4 值解析 |
| `cursor` | 命中测试 | 只算**意图**（`ql_ui_cursor_at()`）：命中节点往上找第一个写了 `cursor` 的祖先，都没写时输入框给 `text`、其余 `default` |
| `user-select` | 输入框选字 | `none` → 点还是能点（光标落点不变），但拖拽不再连选 |
| `scroll-behavior: smooth` | `ql_ui_anim_tick` | 只记目标偏移，由每帧推进（160ms ease-out）；"跳"和"滑"共用同一份 `scroll_x/y` 状态 |

**顺带修掉一个真缺陷（这是本轮最重要的收获）**：`align-items: center / flex-end`
在"容器有确定高度、只有一行"这个最常见的情况下**完全没有效果** —— 因为单行的
交叉尺寸取的是"各项最大值"，`room` 恒为 0。CSS 的默认 `align-content: stretch`
要求单行撑满容器的交叉尺寸，补上之后 `align-items` 才真的能用（golden 的
`align` 页因此变化，肉眼核对过：两个方块现在在 120px 高的容器里居中）。

同一轮把 `align-content` 也补上了（多行的行间分配），所以现在单行、多行两种
情况的交叉轴都按 CSS 语义走。

---

### 18.4.1 `position: sticky` 与分轴 `overflow` 为什么便宜

这两件看起来都要"动滚动子系统"，实际都只挂到已有机制上：

```
滚动      apply_scroll：把子树整体平移 -scroll         （早就有）
sticky    平移之后再把自己推回可视边缘，改的是 border_box
          → 绘制/命中/脏区/paint_box 读的都是 border_box，一行都不用改
overflow-x 裁剪矩形只改一个方向（另一轴沿用外层裁剪）
          → QLDraw2D 的裁剪本来就是矩形，逐轴覆盖一个边就够
```

sticky 的三条约束（都写在 `sticky_adjust` 里）：

1. **只贴给了 inset 的方向**（CSS 也是这样：不给 `top` 的 sticky 等于 relative）；
2. 贴边位置按**最近的滚动容器**的内容盒算；没有滚动容器就按视口；
3. 不能推出**包含块**（父内容盒）—— 这就是"跟着列表滚到头就一起走"，
   而不是永远钉在屏幕上。

`cursor` 则刻意只做一半：CSS 值 + 命中测试算出**意图**（`ql_ui_cursor_at()`），
真正设光标要窗口系统配合（X11 `XDefineCursor` / Wayland `wl_cursor`），
后端目前没有接 —— 写进 API 手册 §3.13，不让它悄悄失效。

### 18.5 B4 第一批：`border-style: dashed/dotted` 与 `outline`（blob v25）

这两样都**只改绘制、不动布局**，所以实现上都不碰布局求解器：

```
border-style: dashed/dotted
  → 绘制期把四条边切成小段，每段一个 fill_rect（点线宽 >= 4 时用 fill_round_rect 画圆点）
  → 图案尺寸跟线宽缩放：dashed = 段 3w + 缝 2w，dotted = 点 1w + 缝 1w
  → CPU 与 GL 走的是同一条 fill_rect，天然像素一致（golden 的 b4 页差异 0.5%）

border-style: none
  → 解析期把 border-width 按 0 存（CSS 的 computed border-width = 0）
  → 不改布局代码，但内容盒不会再莫名缩水 2×width

outline: W style color
  → 绘制期用 stroke_round_rect：外沿 = 边框盒外扩 (offset + width)，厚度 = width
    → 这圈线正好落在 [offset, offset + width] 区间；offset 为负就压进边框里
  → 不参与布局：盒子位置/尺寸、兄弟位置、命中测试都不受影响
  → CSS 语义照抄：outline-style 初值是 none，只写 outline-width **不画**
```

为什么 dashed 不走 SDF：SDF 里只有"到形状边界的距离"，没有"沿周长走了多远"，
要画虚线段得在着色器里反解周长参数，成本远高于"切段填矩形"。代价是与
`border-radius` 同用时圆角按直角画 —— 这条写在覆盖表里，不藏着。

验收：`test/web/test_b4.c`（64 项：解析 + 逐点取色）、
golden 新页 `b4`（CPU vs GPU 0.52%）、例程 `example/25_b4`。

### 18.6 B4 收尾：四边四角 / 两层阴影 / 细节属性（blob v26）

这一批把 `QLUIStyle` 里的"单值边框"换成了"四边 + 四角"：

```
uint8_t  border_w[4]        /* 上 右 下 左 */
uint8_t  border_style4[4]   /* 每边自己的样式 */
uint32_t border_color4[4]   /* 每边自己的颜色 */
int16_t  radius[4]          /* 左上 右上 右下 左下 */
QLUIShadow shadow[2]        /* 两层，先写的画在下面（视觉上在最上） */
QLUIShadow text_shadow
```

布局侧的改动很机械：`2 * border_width` 变成了 `ql_ui_border_h(s)`（左右之和）
与 `ql_ui_border_v(s)`（上下之和），内容盒原点按 `border_w[3]` / `border_w[0]` 内缩 ——
CSS 的"四边宽度不同"就是这样影响内容盒的。

绘制侧分三条路（`paint_border`）：

```
四边全同        → 一次整圈描边（最常见，也是最快的一条）
有 dashed/dotted → 逐边切段（角上按直角）
各边不同        → 整圈描边被"按边裁四次"，每次换一种颜色
```

第三条里的裁剪**必须原样恢复**：一开始忘了恢复，结果 `.sides` 之后的所有兄弟节点
都被裁进那一个小盒子里 —— 画面表现为"某一行之后整页空白"。写进注释，别再踩。

四角圆角则靠两个新入口：

- `QLDraw2D::fill_round_rect4` / `stroke_round_rect4` / `fill_round_rect_gradient4`
  （四角不全同时才走；全同仍走单半径的快路径）
- CPU：`round_row_span4()` —— 每行左右两侧各算一次圆心到边的 `dx`
- GPU：新增顶点属性 `a_radii`（vec4），片元里按象限挑半径，SDF 不变

投影与文字：

- `box-shadow` 最多**两层**，`inset` 用反向 SDF（GPU `mode=7`，CPU 逐像素但只算"环"）
- `text-shadow` 一层，`blur` 用 5 次采样近似（不需要离屏缓冲，MCU 也能跑）

细节属性都很便宜：`image-rendering`（GL 切纹理滤波）、
`scrollbar-width/color`（内置滚动条的宽度与颜色）、
`object-position`（`object-fit` 之后在余量里摆位，`cover` 时改的是源矩形）。

验收：`test/web/test_b4.c`（104 项）、golden 的 `b4` 页（CPU vs GPU 0.05%）、
例程 `example/25_b4`（九节，含滚动条与 focus 环）。

### 18.7 B2 / B3 尾巴与背景图（blob v27）

这一节收掉五批里最后三项。它们共同的思路是：**能在构建期算清的一律不算在运行期**。

**状态选择器**（`QLUINode` 20 → 28 字节）：

```
style_disabled / style_checked / style_placeholder   /* 各一份变体样式下标 */
placeholder                                          /* 文案在文本池里的下标 */
flags: QL_UI_NODE_CHECKBOX / _CHECKED / _DISABLED
```

- 与 `:hover` 完全同构：多带一个状态位再跑一遍级联，与基础样式不同才建一份新样式；
- 运行期优先级 `disabled > active > focus > checked > hover`；
- `:enabled` 是"没有 disabled"，所以**基础样式**按节点属性决定要不要打开这个位；
- 复选框有自己的 UA 默认（内部标签名 `#cb` → 16×16、无内边距），
  并且**不进输入框池**（它没有文本）；
- `::placeholder` 在构建期把 `input::placeholder` 的声明**合并进宿主样式**，
  单独存一份"占位样式"，输入框为空时用它画。

**文本细节**（`QLUIStyle` 240 → 272 字节）：

- `word-spacing` 只加在空格之后（量宽与绘制走同一个 `adv_of`）；
- `tab-size` 在**建表期**把 `\t` 展开成 N 个空格 —— 运行期一个字符判断都没有；
- `text-decoration-*` 的形态用 `fill_rect` 拼（double 两条、dotted 点、dashed 段+缝），
  不需要新图元，CPU / GPU 天然一致；`overline` 占 `deco_style` 的高位。

**背景图**：与 `<img src>` 共用资源表。

```
background-image: url(checker)  → 名字进文本池，样式里存 QL_UI_NO_IMAGE 之外的下标
运行期 ql_ui_register_image("checker", ...) 按名字填像素（同一张图只解码一次）
绘制：object-fit 的同一套几何（cover / contain / <w> <h>）+ position + repeat
```

平铺时先把起点对齐到可见瓦片、再把裁剪设成边框盒 —— 不设裁剪会画到兄弟节点上
（与"四边不同色"那次裁剪没恢复是同一类错误，这次提前防住了）。

**没做的：`filter`**。颜色矩阵类（brightness/grayscale/...）技术上可行，
但 `blur()` / `drop-shadow()` 需要**离屏图层**（子树先合成到一张图再整体模糊），
成本与 `transform: rotate()` 同级，所以一起留作单独立项 —— 覆盖表里如实标 ⏳。

### 18.8 B6/B7 的代价是怎么估出来的

新批次（Grid / 表格 / 多栏 / `border-image`）都要在"进 MCU 能力包"之前先说清代价。
这一节说明**怎么量**，数字见 §2.1。

**① 布局的边际成本**（`test/mcu` 的同一份 CSS，只改卡片数量）：

```
12 节点 0.23 ms → 39 节点 0.82 ms → 75 节点 1.94 ms
拟合斜率 ≈ 4.4k 指令/节点 ≈ 22 µs/节点 @200 MHz
```

**② wrapper 容器的成本**（同一视觉，两种结构）：

```
3×3 卡片用嵌套 flex（3 个 .row，15 节点）   0.427 ms
同一视觉用扁平 flex-wrap（12 节点）         0.229 ms
```

差 0.198 ms 只换来 3 个"什么都不画"的 `.row` —— **一层 wrapper ≈ 3 个叶子节点**。
这条读数直接推翻了旧文档"Grid 太贵所以不做"的判断：

- Grid 的求解是"轨道定尺 → 放置 → 对齐"三段，都是整数加法，**没有逐像素**；
- 而"用 flex 拼网格"要付 wrapper 的钱，**每次重排都付**；
- 所以 Grid 落地后，网格类页面的 MCU 布局时间是**下降**的（省 46%，见上）。

**③ 表格为什么比 Grid 贵**：`table-layout: auto` 的列宽分配要**两遍**
（第一遍量每列 `min-content` / `max-content`，第二遍按可用宽度分配），
即"布局边际成本 ×2"；`border-collapse` 还要"半边边框相加"。所以它估 6–10 KB。

**④ 多栏最贵**：`column-count` 要求列高尽量相等，只能"先排一遍量总高、
算出目标列高、再排一遍断列"。同样是 ×2 遍，但还要维护"一个节点跨两列"的中间态 ——
这是**唯一真正与"一个节点一个盒"冲突**的一项，所以估 8–15 KB。

**⑤ `border-image` 最便宜**：九宫格 = 把源图切 9 块，四角原样、四边单向拉伸、
中间双向拉伸。绘制走的是现成的 `ql_canvas_blit_image()`（只是采样区间不同），
解析期切一次就存进样式，**运行期零额外状态**。估 2–3 KB。

**⑥ 第三方库为什么必须按 `G` 处理**（`@font-face` / 整形 / 双向）：

```
libharfbuzz.so   1,178 KB   = 当前整个 MCU 固件（131.5 KB）的 8.9 倍
libfreetype.so     813 KB   = 6.2 倍
libfribidi.so      106 KB   = 0.8 倍（fribidi 本身还勉强，但单独它不解连字）
一份全量 CJK TTF    21 MB   = 160 倍
```

这几样在 PC/Mobile 档是**必须的**（否则中文以外的语言、真字重、连字全都没有），
在 MCU 档是**必须裁的**（裁掉后仍是内建 ASCII 位图字体 + 可选子集 TTF）。
这就是 `G` 这个原因码存在的意义：**它标的是"依赖关系"，不是"不做"**。

---

### 18.9 B6 落地：Grid 与 border-image 是怎么做的

#### ① 数据放哪：模板进池，摆放进样式

`QLUIStyle` 只长了 40 字节（272 → **312**），因为**轨道列表不进样式**：

```c
/* QLUIStyle：容器 3 个字段、子项 4 个字段 */
uint16_t grid;          /* doc->grids[] 下标；QL_UI_NO_GRID = 不是 grid 容器 */
int16_t  grid_col, grid_row;        /* 起始线（1-based）；0 = 自动 */
uint8_t  grid_col_span, grid_row_span;
uint8_t  grid_flow, justify_items, justify_self;
int16_t  grid_auto_row, grid_auto_col;

/* doc 里三段池（所有容器共享；同一模板经样式去重后只存一份） */
QLUIGrid      grids[];       /* col/row 轨道的起止下标 + 具名区域起止 */
QLUIGridTrack grid_tracks[]; /* { min_kind, max_kind, min_val, max_val } */
QLUIGridArea  grid_areas[];  /* { 名字, 列, 行, 列跨, 行跨 } */
```

一条轨道是 `[min, max]` 区间：`100px` → 两边都定长；`1fr` → `min=auto, max=fr`；
`minmax(60px, auto)` → `min=60px, max=auto`。定尺时先满足 min，再把剩余按 max 侧的
"拉伸能力"分配（`fr` 权重 / `auto` 取内容）。

#### ② 摆放与模板搜索为什么在构建期

`grid-area: main` 要查**父容器**的具名区域，`grid-column: 1 / -2` 要查父容器有几列 ——
这些在样式编译时都还不知道（编译是逐节点、按声明集去重的）。所以分两趟：

1. `compile_style()` 只把 Grid 属性的**原文**挂到"按样式下标"的侧表上；
2. 整棵树都有了样式之后，`grid_resolve_all()` 统一解析：模板 → 池；
   每个子项的线号/跨度 → 4 个整数写进样式。

去重带来一个坑：同一个 `.cell` 可能被两个不同模板的容器使用，摆放值就不一样。
这里用**写时复制**——样式第一次被占用时直接写，第二次要不同的值就复制一份样式
（`sig = NULL`，不再参与去重）。运行期因此永远只读整数，没有任何字符串比较。

#### ③ 布局：三段，全是整数

```
① 放置   ：显式位置 → 只给列 → 只给行 → 全自动（游标扫描，dense 时回原点）
② 列定尺 ：min → 定长上限 → auto 取内容 max-content → fr 分剩余（累计推进）
③ 行定尺 ：先按"列宽已定"把每项真排一遍拿高度 → auto 行取最大值 →
           span>1 按差额均摊 → fr 行分剩余（容器高度确定时）
④ 定位   ：按 justify-self / align-self 在格子里摆，默认双向 stretch
```

`fr` 的份额用**累计分子**（`num += 总额×权重; 份额 = num/权重和; num %= 权重和`），
与 flex 的 grow 同一套 —— 三列 `1fr 1fr 1fr` 在 100px 里是 33+34+33，右边界严丝合缝。

#### ④ 嵌套 grid 的临时区：按栈的纪律切

轨道数组、列/行起点、每项的放置结果都是临时数据。**不能**用一份全局数组：
测量阶段会递归进入内层 grid，把外层刚算好的列宽冲掉（实测：内层列宽对、外层整体
缩小一半）。所以 `QLUIRoot` 里放一个只增不减的缓冲 + bump 指针：

```c
const uint16_t saved_place = ui->grid_place_used;
const uint32_t saved_coord = ui->grid_size_used;
... 用 ...
ui->grid_place_used = saved_place;   /* 函数出口（含降级路径）都还原 */
ui->grid_size_used  = saved_coord;
```

`ql_ui_layout()` 每次把两个指针归零，所以稳态下**零分配**（不违反"热路径不 malloc"）。

#### ⑤ `border-image`：九宫格 = 9 次 blit

后端早就有 `blit_image(dst, src)`（子矩形采样），所以九宫格不需要新图元：

```
四角：src 原样 → dst 角块（缩放）
四边：src 单条 → dst 单条（stretch / repeat / round / space）
中间：src 中心 → dst 中心（stretch，或裁剪后平铺）
```

切片值 `border-image-slice` 是**源图**上的像素（或百分比）；
画出来的宽度用 `border-image-width`，默认等于切片值。
写了 `border-image` 就不再画 `border-style`（CSS 语义）。

资源走的是 `<img src>` / `background-image` 的**同一张表**：
`ql_ui_register_image(ui, "frame", ...)` 填一次像素，三种用法共享。

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
| `!important`（出现在**已支持**的属性里） | 293 | 5.4% | ✅ 已支持 |
| `var(--x)`（CSS 变量） | 537 | 10.0% | **✅ 已支持**（§18.1，构建期替换） |
| `calc()` | 65 | 1.2% | **✅ 已支持**（§18.3） |

### 19.3 补完之后能到多少（模拟同一份语料）

| 阶段 | 声明级覆盖 |
|---|---|
| 现在（含 `var()` / `calc()`） | **≈ 84.8%**（73.6% + 变量 10.0% + calc 1.2%） |
| `var()` 支持之前 | 73.6% |
| + B1 值与尺寸 | 76.5% |
| + B2 选择器（`content` / `::before`） | **89.6%** |
| + B3 文本细节 | 90.0% |
| + B4 绘制细节 | **94.0%** |
| + B5 布局细节 | 94.5% |

剩下 5.5% 主要是 `float`（不做）、`cursor`（鼠标形状）、`list-style`（没有生成内容）、
`@font-face` 的 `src`、`overflow-x/y` 分轴、`align-content`。

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
| `var(--x)` | ~~537 条声明~~ | **已解决**（构建期替换） |
| `@media` | 191 个块 | **已支持**宽度断点（`min/max-width`）；`orientation` / `prefers-*` 等仍报错 |
| `!important` | 293 条声明 | 值里带它就报错（B1 补上后消失） |

所以**"CSS3 兼容度"取决于你要搬的是什么**：

| 搬什么 | 现在 | B1+B2 后 |
|---|---|---|
| 自己按本库能力写新页面 | **85~90% 的手感** | 95%+ |
| 直接搬 Bootstrap 这类框架 | ~90% 声明能过（`var()` 已支持，`@media` 仍要改写） | ~95% |
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

> 但"真实样式表几乎不写"**不等于可以不做**：`border-image` 与多栏已经进 B6/B7
> （§22），因为 PC/Mobile 档的目标是兼容，不是"够用"。这份覆盖率是**现状**，
> 不是**目标**。

---

## 19.1 已支持但**语义受限**的速查表

这些不用改架构，但**写的时候要知道边界**，否则会以为是 bug：

| 属性 | 实际行为 | 边界 |
|---|---|---|
| `background`（简写） | 只认纯色或**单个**渐变 | `background: #fff url(a.png)` 报错；图片请用 `<img>` |
| `linear-gradient()` | **必须写方向** | `to bottom right` 不支持；用 `to right` / `90deg` |
| `border` / `border-style` | `solid` / `dashed` / `dotted` / `none` | `double` / `groove` / `ridge` / `inset` / `outset` 报错 |
| `grid-auto-flow` | `row` / `row dense` | **`column` 报错**：列流要把整张网格转置，收益远小于成本 |
| Grid 的 `min-content` / `max-content` | 按 `auto`（= max-content）处理 | 差异只在"内容比轨道大"时可见 |
| Grid 跨列项的内容宽 | 超出的部分记到**它跨过的最后一列** | 浏览器按"各列按比例分摊"；两者只在"跨列项比各列内容之和还宽"时不同 |
| Grid 自动放置 | 稀疏 + 圆角游标 | 与浏览器的 `dense` / 稀疏规则**行为一致**，只是没有实现 `masonry` |
| `border-image` | 四角原样、四边/中间按 `repeat`/`round` | `space` 按 `repeat` 处理；`outset` 报错；不跟随 `border-radius` 裁 |
| `border-radius` | 单值（px 或 %） | 四角 / 斜杠椭圆角都不行 |
| `box-shadow` | 单条 | 多重、`inset` 都不行 |
| `opacity` | 逐图元降透明 | 子元素会一起淡，但**同一子树内部互相覆盖处会略透**（真 group 要离屏图层，见 §20.1） |
| `transform` | 只有 `translate` / `scale` / `scaleX` / `scaleY` | `rotate` / `skew` / `matrix` 报错 |
| `transform-origin` | **固定为元素中心** | 属性本身没实现 |
| flex 项的**主轴尺寸** | 用 `box-sizing: border-box` 时精确 | **content-box 时少算 padding + border**（`force_size` 把主轴尺寸按边框盒解释）—— flex 子项请写 `box-sizing: border-box`，这是真实页面的常规写法 |
| `@media` | 只有宽度断点 | `orientation` / `prefers-*` / `em` 单位报错；`print` 整块跳过 |
| `transition-timing-function` | 5 个具名缓动 | 没有 `cubic-bezier()` |
| `transition-delay` | 没实现 | |
| `font-family` | 只有 `DEFAULT` / `MONO` | 不是自由字体列表；字体文件由宿主提供 |
| `font-weight` / `font-style` | **合成**（画两遍 / 按行剪切） | 不加载第二份字体文件 |
| `text-decoration` | `underline` / `overline` / `line-through` | 没有 `wavy` 线型 |
| `vertical-align` | `baseline` / `sub` / `super` | 没有 `middle` / 长度值 |
| `text-align` | `left` / `center` / `right` | 没有 `justify` |
| `overflow` | 一个值管两轴 | 没有 `overflow-x/y` |
| `flex-wrap` | `nowrap` / `wrap` | 没有 `wrap-reverse` |
| `display` | `block` / `inline` / `inline-block` / `flex` / `inline-flex` / `none` | 没有 `grid` / `table` / `contents` |
| `%` 长度 | 根元素按视口；子元素主用于 `width/height` | `padding: 5%` 报错 |
| 混排（文本 + 行内子元素） | 单行混排正常（`<p>文字<b>粗</b>结尾</p>`） | **行内子元素后面的那段文本不会自动折行**（超长会溢出）；长段落请拆成块级元素 |
| 长度单位 | `px` / `%` / `em` / `rem` / `vw` / `vh` / `vmin` / `vmax` / `calc()` | 没有 `ch` / `ex` / `lh` |
| `calc()` 能用在哪些属性 | `width` / `height` / `min-*` / `max-*` / `top` / `right` / `bottom` / `left` | `font-size` / `border-*` / `letter-spacing` / `text-indent` / `margin` / `padding` 只接受**构建期就能折成 px** 的 `calc()`（其余报错） |
| `calc()` 的标量乘除 | 构建期折进每个操作数 | 百分点精度会掉（`calc(100% / 3)` 是 33%，不是 33.33%）；长度 × 长度报错 |
| `align-items` / `align-self` | 单行 + 交叉轴确定时行会撑满容器 | 多行 `align-content` 支持 `stretch`/`center`/`end`/`space-between`/`space-around`/`space-evenly`；`baseline` 不支持 |
| `order` | 就地排子链表 | 只对 **flex 容器的直接子项** 生效；与 `z-index` 同时出现时先按 order、再按 z（同 z 保持 order 顺序） |
| `pointer-events` | 只有 `auto` / `none` | 没有 `all` / `visiblePainted` 那套 SVG 取值 |
| `cursor` | ✅ `auto`/`default`/`pointer`/`text`/`move`/`not-allowed` 等 | 只是**意图**：形状由宿主用 `ql_ui_cursor()` 取出来交给窗口系统 |
| `visibility` | `visible` / `hidden`（`collapse` 当 hidden） | 只影响"画不画自己"与命中；布局照常（这正是与 `display:none` 的区别） |
| `!important` | 两趟级联 | 支持 |

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
   已经落地的做法：折行参数收进 `QLTextWrapOpt`（`ql_text_wrap_opt`），
   布局与绘制都从 `QLUIStyle` 用同一个 `wrap_opt_of()` 取值。

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
| `CSS_BORDER_STYLE` | `QL_FEAT_CSS_BORDER_STYLE` | `dashed` / `dotted`、四边独立 `border-*`（✅ 全部落地） | ✅ | ❌ | BASE | — |
| `CSS_SELECTOR_ADV` | `QL_FEAT_CSS_SELECTOR_ADV` | **B2 之后**：`:nth-child` 等结构伪类、`[attr]`、`::before/::after` | ✅ | ⚠️ 见下 | BASE | 只省解析期 |
| `CSS_IMAGE_FIT` | `QL_FEAT_CSS_IMAGE_FIT` | `object-fit` / `object-position`（只在有图片管线时有意义） | ✅ | ❌ | `QL_FEAT_UI_IMAGE` | 与图像管线共用 |
| `CSS_UI` | `QL_FEAT_CSS_UI` | **B5 之后**：`cursor` / `pointer-events` / `user-select` / `scrollbar-*` | ✅ | ❌ | BASE | — |
| `CSS_GRID` | `QL_FEAT_CSS_GRID` | **B6 ✅**：`display: grid` 全家（轨道定尺 / 自动放置 / 跨格 / `justify-*`） | ✅ | **默认开**（可裁） | BASE | **实测 4,528 B**（−3.3%） |
| `CSS_TABLE` | `QL_FEAT_CSS_TABLE` | **B7**：`display: table` 全家 + `<table>` 标签 + 列宽分配 + `border-collapse` | ✅ | **默认关** | BASE | 估 6–10 KB（`table-layout: auto` 要两遍测量） |
| `CSS_COLUMNS` | `QL_FEAT_CSS_COLUMNS` | **B7**：`columns` / `column-rule`（列高平衡 = 排两遍） | ✅ | **默认关** | BASE | 估 8–15 KB（最贵的一项：要"盒内分裂"） |
| `CSS_BORDER_IMAGE` | `QL_FEAT_CSS_BORDER_IMAGE` | **B6 ✅**：`border-image` 九宫格 | ✅ | **默认开**（可裁） | BASE + `QL_FEAT_UI_IMAGE` | **实测 2,176 B**（−1.6%） |
| `CSS_FONT_STACK` | `QL_FEAT_CSS_FONT_STACK` | **B8**：`@font-face` + 自由 `font-family` + 真字重/斜体 —— 依赖 FreeType | ✅ | **恒关** | BASE + `QL_FEAT_FONT_FREETYPE` | 依赖库体积见 §2.1（FreeType 813 KB） |
| `CSS_TEXT_SHAPE` | `QL_FEAT_CSS_TEXT_SHAPE` | **B8**：连字 / 字距 / `font-feature-settings` —— 依赖 HarfBuzz | ✅ | **恒关** | `CSS_FONT_STACK` | 依赖库体积见 §2.1（HarfBuzz 1,178 KB） |
| `CSS_BIDI` | `QL_FEAT_CSS_BIDI` | **B8**：`direction` / `unicode-bidi` —— 依赖 SheenBidi（或 fribidi，106 KB） | ✅ | **恒关** | BASE | 依赖库体积见 §2.1 |

### 21.3 四个反直觉的判断

1. **`CSS_SELECTOR_ADV` 是唯一"裁了不省多少"的包**。B2 全部在**构建期**展开
   （`:nth-child` → 内部类、`::before` → 真节点），运行期零成本。
   裁它只省解析器代码（几 KB），**不省 RAM、不省每帧开销**。
   MCU 上更值得的做法是：**保留它**，因为它让"同一份 HTML"能两端跑，
   而省下的那点 Flash 比"改两套 HTML"便宜得多。
2. **`CSS_GRADIENT` 在 GPU 上几乎免费**（顶点颜色插值，不加顶点、不加 draw call），
   CPU 侧才是逐像素成本。所以"给 MCU 留渐变、裁掉阴影"是合理组合 ——
   包之间是**正交**的，不搞"要么全有要么全无"。
3. **`CSS_GRID` 裁掉之后页面会变慢，不是变快**（§11 实测：嵌套 flex 拼 3×3
   要 0.427 ms，Grid 只要 ≈0.229 ms）。所以"MCU 上关 Grid"只在**Flash 紧张**
   时才划算；Flash 够的板子建议留着，让同一份 HTML 两端通用。
4. **`CSS_ANIM` 与省电相关**。关掉它不仅是省代码：没有 `animation` 就没有
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

## 22. 批次计划（B1–B5 已完成，B6–B8 是 PC/Mobile 档的主线）

排序依据：**先补"真实页面没它写不出来"的，再补"锦上添花"的**；同一批内先做不动
`QLUIStyle` 的（只动解析期），后做动结构体的（要抬 blob 版本 + 重生成 golden）。

**B1–B5** 是"补细节"（已完成）；**B6–B8** 是"补架构"（PC/Mobile 档，MCU 归能力包）。

| 批 | 内容（对应上面各节） | 归属能力包 | 收益 | 风险 | 动 `QLUIStyle` | 验收 |
|---|---|---|---|---|---|---|
| **B1 值与尺寸**（已完成：`min/max-*`、`!important`、`hsl()`、`#rrggbbaa`、`currentColor/inherit/initial`、`em`/`rem`/`vw`/`vh`/`vmin`/`vmax`、`margin: auto`、`aspect-ratio`；**剩 `border-radius` 四角**） | `min/max-*`、`em`/`rem`/`vw`/`vh`、`calc()`/`min()`/`max()`、`margin: auto`、`inherit`/`initial`/`currentColor`、`!important`、`border-radius` 四角、`aspect-ratio`、`hsl()`、`#rrggbbaa` | BASE（少量进 RADIUS） | 真实页面的**基础语法** | 中：动级联与宽度求解 | **是** → blob v16 | 新 `test_css_units` + golden |
| **B2 选择器**（✅ 已完成：结构伪类 / `[attr]` 系列 / `:not()` / `*` / `::before`+`::after`+`content` / `:disabled` / `:enabled` / `:checked` / `::placeholder`（含复选框控件），见 §18.7） | `:first/last/nth-child(an+b)/only-child/of-type`、`:not()`、`[attr]` 系列、`*`、`::before/::after`+`content`、`:disabled/:checked`、`::placeholder` | `CSS_SELECTOR_ADV` | **兼容性收益最大**（12.5% 声明 + 25% 选择器） | **低**：全在构建期展开，运行期零成本 | **否**（伪元素在构建期变成真节点） | `test_pseudo_text` / `test_selectors` + golden |
| **B3 文本细节**（✅ 已完成：`letter-spacing` / `text-indent` / `text-transform` / `text-overflow: ellipsis` / `word-break`（v20）、`word-spacing` / `tab-size` / `text-decoration-*`（v27，见 §18.7）） | `letter-spacing`、`word-spacing`、`text-indent`、`text-transform`、`text-overflow: ellipsis`、`word-break`/`overflow-wrap`、`tab-size`、`text-decoration-*` | `CSS_TEXT_ADV` | 列表、标题、长 URL | 中：动 `ql_text_wrap()`（见 §20.3） | **是** → blob v20 | 新 `test_text_adv`（25 项，含像素层）+ golden |
| **B4 绘制细节**（✅ 已完成：`border-style: dashed/dotted` + `outline`（§18.5）、四边独立 `border-*`、四角 `border-*-radius`、两层 + `inset` 阴影、`text-shadow`、`image-rendering`、`scrollbar-*`、`object-position`（§18.6）、`background-image: url()` + `size/position/repeat`（§18.7）。**只剩 `filter`**（颜色矩阵可做，`blur()` 需离屏图层 —— 与 `rotate` 同级，单独立项） | `border-style: dashed/dotted`、四边独立 `border-*`、多重/`inset` 阴影、`outline`、`background-size/position`、`object-position`、`text-shadow`、`image-rendering`、`scrollbar-*` | `CSS_BORDER_STYLE` + `CSS_SHADOW` / `CSS_GRADIENT` | 视觉还原度 | **中高**：每个都要 CPU + GL 两份，必须过 `test_gl_vs_cpu` | 部分 | 扩 `test_effects` + `test_gl_vs_cpu` |
| **B5 布局与交互**（✅ 已完成：`align-self`/`align-content`/`order`/`flex-flow`/`overflow-x/y`/`sticky`/`inset`/`visibility`/`pointer-events`/`cursor`/`user-select`/`scroll-behavior`，见 §18.4 与 §18.4.1） | 上列十二项 | `CSS_FLEX` 补全 + `CSS_UI` | 表达力 + 桌面手感 | 中：动 flex 求解器与命中测试 | 是 | 扩 `test_flex*` / `test_scroll` / `test_b5` |

| **B6 布局广度** ✅ **已完成（2026-11-10）** | **Grid**（`display: grid` 全家、`fr`/`minmax()`/`repeat()`/具名区域/自动放置/跨格/对齐）、**`border-image`** 九宫格 | `CSS_GRID` + `CSS_BORDER_IMAGE` | Bootstrap/Tailwind 栅格、卡片墙 | 中高：新求解器（§11 有实测对比） | **是** → blob v28（`QLUIStyle` 272 → 312） | `test_grid`（74 项，含跨列/跨行/区域/负线号/9 条报错）+ 例程 29/30 |
| **B7 数据排版**（PC/Mobile 必做） | **表格**（`table` 全家 + `<table>` 系列标签 + 列宽分配 + `border-collapse`）、**多栏**（`columns` 平衡列高） | `CSS_TABLE` + `CSS_COLUMNS` | 后台管理界面、长文正文 | 高：`auto` 列宽与列高平衡都要排两遍 | 是 | 新 `test_table` / `test_columns` + golden |
| **B8 文本栈**（PC/Mobile 必做，MCU 恒裁） | `@font-face`、自由 `font-family`、真字重/斜体、连字与字距、`direction`/`unicode-bidi` | `CSS_FONT_STACK` / `CSS_TEXT_SHAPE` / `CSS_BIDI` | **多语言真实页面**（中文、阿拉伯语、印度语系） | 高：引入 FreeType + HarfBuzz + SheenBidi（§2.1 有体积实测） | 是（字体表、整形缓存） | 扩 `test_font_ft` / 新 `test_shape` |

**另外几项单独立项**（成本与一批相当，但语义是"新子系统"）：

| 项 | 目标档 | 为什么单独立项 | 成本 |
|---|---|---|---|
| **离屏图层**（`filter: blur()` / 真 group `opacity` / `mix-blend-mode`） | **PC/Mobile 必做** | 三者共用同一块能力："子树先合成到一张图，再整体处理"。做完它，这三样一起解锁（见 §20.1） | 高：CPU 要一块临时缓冲 + 一次合成；GL 要 FBO 复用 + 纹理采样。RAM 成本 = 一个元素盒子大小的 RGBA 缓冲 |
| **`transform: rotate()` / `skew()`** | **PC/Mobile 必做** | 要一条旋转光栅路径（CPU 逐像素仿射 + GL 顶点变换 + 字形旋转 + 裁剪盒变换） | 中高：与 flex 求解器不同量级，但比离屏图层便宜（不需要额外缓冲） |
| **`filter` 的颜色矩阵族**（brightness/contrast/saturate/grayscale…） | **PC/Mobile 必做** | 不一定要离屏：单色乘法可以直接融进每个绘制入口（CPU 乘、GL 用一个 uniform）。但要保证"子树整体"语义才准 | 中：CPU + GL 各一份 |
| **`clip-path` 的任意形状**（`polygon()` / `path()` / `inset()`） | PC/Mobile | 要通用裁剪扫线（现在只有 `circle()` 的 SDF） | 中 |
| **3D 变换**（`perspective()` / `rotate3d()`） | ❌ 不排期 | 要透视除法 + 深度排序 + 一个真正的合成器 | 极高，且与"一个节点一个盒"冲突 |

**建议顺序**：B1–B5 已完成 → **B6（Grid + `border-image`）→ B7（表格 + 多栏）→ B8（文本栈）**；
`离屏图层` 可以插在 B6 之后（它一次解锁三样，性价比最高）。
每一批都要同时给出"MCU 档裁掉后的数字"（§21.2 的"足迹"列），这是本库的老规矩。

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
| 效果检阅 | `example/16_css_units` / `17_css_selectors` / `18_css_paint`：每个已支持的效果摆成一格，改一行 CSS 重跑就能看出哪格不对 | ✅ |
| 性能 | `benchmark/bench --scene all`（8 个维度 × 2 后端 × full/dirty） | ✅ |

**效果检阅比单元测试更容易抓到真 bug**：单元测试每绿一项就只看一项，
三个 gallery 例程把特性和特性摆在一起，才看得出它们的相互作用 ——
例如"`::before` 配宿主文本"的重叠问题，就是 17 号例程第一次跑出来的。

---

## 25. 相关文档

- [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) —— 标签侧的同口径清单
- [QLUI 阅读指南](QLUI阅读指南.html) —— 样式 / 布局 / 绘制的实现细节与踩坑
- [Web 兼容设计](../../design/Web兼容设计.html) —— 分层与"为什么不做 JS / Grid"
- [开发规划](../../design/开发规划.html) —— 批次排期与验收
