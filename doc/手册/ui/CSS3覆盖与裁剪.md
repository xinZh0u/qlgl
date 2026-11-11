# CSS 覆盖与裁剪（全量清单）

这份文档回答三个问题，口径与 [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) 一致：

1. **CSS 的每一样东西，本库支持到哪一步？** —— 按模块穷举，一条不漏。
2. **不支持的为什么不做？** —— 每条给一个**原因码**（§2），不写"暂不支持"这种废话。
3. **哪一批先做？** —— 按"收益 / 风险 / 是否动 `QLUIStyle`"排批次（见 [CSS 进度规划](CSS3进度规划.html) §22）。

## 章节分布（三份文档）

这份"CSS 覆盖与裁剪"拆成三份，**章节编号保持不变**（外部文档按 § 号引用，跳号不影响定位）：

| 文档 | 装什么 | 章节 |
|---|---|---|
| **本文 = 清单** | 逐条状态（支持到哪一步）、原因码、覆盖率实测、能力包 / 裁剪、验证策略 | §0、§1–§18.3、§19–§21、§23–§25 |
| [CSS 进度规划](CSS3进度规划.html) | 已完成 / 待做快照、批次计划与排序依据 | §0.1、§22 |
| [CSS 开发日志](CSS3开发日志.html) | 每一批**落地时**的做法、踩坑、代价与验证 | §18.4–§18.17 |

下文出现的 **§18.4–§18.17**（落地叙述）、**§0.1 / §22**（排期）不在本文里，按上表去对应的那份文档找。

---

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
| `:empty` | ✅ | B | 没有子元素、也没有非空文本；**只有空白**算 empty（它不产生盒子），与 CSS 一致（B9） |
| `:link` `:visited` | — | N | 本库没有超链接跳转语义（`<a>` 只是行内文本） |
| `:target` `:lang()` `:dir()` | ❌ | P | 需要运行期状态/文档语言上下文 |
| `:has()` `:is()` `:where()` `:focus-visible` `:focus-within` | ❌ | B | L4 选择器；`has()` 会引入"反向依赖"，与构建期定型冲突 |
| `:hover` 之外的交互态（`:active` 之外） | — | N | |

### 3.4 伪元素

| 伪元素 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `::before` + `content: "文本"` | ✅ | B | 构建期生成真节点 |
| `::after` + `content: "文本"` | ✅ | B | 同上 |
| 同一伪元素拆成**多条规则**写（`.x::before{content:""}` + `.x::before{…}`） | ✅ | B | 选择器完全相同的规则合成**一个**伪节点，一条源规则对应一条内部规则，声明照级联合并（开发日志 §18.17）。**已知差异**：两个*不同*选择器（`.x::before` 与 `.x.y::before`）各自成组，**都得写 `content`**；浏览器按"元素匹配到哪些规则"合并，可以只写一次 |
| `::first-line` `::first-letter` | ❌ | F | 哪几个字落在第一行要等**折行**才知道，而本库的样式是构建期编译成静态表的。B9 起**明确报错并给替代写法**（`<span class=...>`），不再只报"不支持的伪元素" |
| `::selection` | ❌ | P | 选区颜色是运行期状态；输入框选区已经硬编码为 `text_color @ 0x50` |
| `::placeholder` | ✅ | B | `placeholder` 属性 + 一份独立占位样式（§18.7） |
| `::marker` | ❌ | P | 没有生成内容机制，符号请自己写字符 |
| `content: counter()` / `attr()` / `url()` | ❌ | P | 需要计数器/属性求值，等于引入一个小运行期 |

### 3.5 @ 规则

| @ 规则 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `@keyframes` | ✅ | B | 编译期摊平成 `QLUIKeyframe[]` / `QLUIAnimation[]` 静态表 |
| `@font-face` | ✅ | G | B8：解析成"字体表条目"，喂给 FreeType/HarfBuzz；MCU 档裁掉（只留内建位图字体） |
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
| `deg` | ✅ | L | 渐变方向（`90deg`）与 `rotate()`/`skew()`/`hue-rotate()` |
| `turn` / `rad` / `grad` | ❌ | N | 只有旋转用得上；`deg` 已经够表达，加三种写法只是多三条解析分支 |
| `fr` | ✅ | L | Grid 专用（B6，§11）；flex 里写了报错 |
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
| `revert` | ✅ | R | 与 `unset` 同路径：继承属性取父值、非继承属性取初值（B9；层级差异见 §18.15） |
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
| `margin` + `-top/right/bottom/left` | ✅ | L | px / em / rem（构建期折 px）/ `%` / `vw` `vh` `vmin` `vmax`（布局期按包含块宽折 px）；`auto` 见下一行；负值可用 |
| `margin: … auto` | ✅ | L | **横向**把剩余空间分掉（`margin: 0 auto` 居中）；纵向 auto 按 0 处理（本库还不做外边距合并） |
| `padding` + 四方向 | ✅ | L | px / em / rem / `%` / `vw` `vh` `vmin` `vmax`（`%` 相对包含块**内容宽**，布局期折 px） |
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
| `background-image: linear-gradient()` | ⚠️ | R | **必须写方向**（`to right` / `to bottom` / `90deg` / `180deg`）；`to bottom right` 报错；两个色标**可以各带 alpha**（`rgba(...,0)` → 柔边/淡出） |
| `background-image: radial-gradient()` | ✅ | R | 需要 3 个参数（位置, 起色, 止色）；止色带 alpha 就是"中心实、边缘淡出"的软光斑（圆心恒居中） |
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
| `border-radius` 的**百分比**（`50%` 圆形头像） | ✅ | R | 横向半径相对盒子**宽**、纵向相对**高**；构建期存成负值当标记（`-50` = 50%），布局结束拿到边框盒后 `resolve_radius_pct()` 折成 px 写进样式副本 —— 绘制/命中代码读到的仍是 px（开发日志 §18.19）。`50%` 正方形 = 内切圆，长方形 = 胶囊（被短边夹住） |
| `border-{corner}-radius` | ✅ | R | 四角长写，可单独覆盖 |
| `border-radius` 斜杠椭圆角 | ⚠️ | R | B9：`10px / 20px` **能编译也能画**，但两个后端都用单半径 SDF，所以**取水平/垂直两者的平均**作为等价半径。要精确的椭圆角得给每角再加 4 个顶点属性并改写两个 SDF —— 已知近似，写在 §18.15 |
| 四边不同色 + 圆角 | ⚠️ | R | 四条边各自裁一次整圈描边；**角部归上/下边**（浏览器沿 45° 斜线切分） |
| `border-image` + `-source` / `-slice` / `-width` / `-repeat`（`stretch`/`repeat`/`round`/`space`） | ✅ | R | **已完成（B6）**：九宫格切片，复用 `blit_image(dst, src)`（§18.9） |
| `border-image-outset` | ❌ | N | 报错：边框图不向盒子外扩 |
| `box-shadow`（单条） | ✅ | R | CPU SDF + GPU `mode=2` 各一份实现 |
| `box-shadow`（多重 / `inset` / `spread`） | ✅ | R | 最多**两层**（多写的报错，不静默丢弃）；`inset` 用反向 SDF（GPU `mode=7`）；**B9 起支持 `spread`**（第四个长度，轮廓整体外扩/内缩） |
| `text-shadow` | ✅ | R | 一层（多层的语义是叠色）；`blur` 用 5 次采样近似（0~16px） |
| `outline` + `-width/-style/-color/-offset` | ✅ | R | 画在边框盒**外侧**、**不占布局**（focus 环标配）；`outline-offset` 可为负（压进边框里）。宽度接受 `px`、`0`（= 去掉 focus 环）与 `thin`/`medium`/`thick`（1/3/5 px，简写与 `outline-width` 长写都认）。CSS 语义照抄：只写 `outline-width` 不画（`outline-style` 初值 `none`），要画就写 `outline: 2px solid <色>` |
| `box-decoration-break` | ❌ | N | 跨行断开的盒子装饰，本库没有"跨行盒子"概念 |

---

## 8. 颜色与不透明度

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `color` | ✅ | R | |
| `opacity` | ✅ | O | 0~1，默认 1；**有子节点时走离屏图层 = 真 group opacity**（重叠处不会更深），单节点时逐图元乘 alpha（与合成等价，省一层缓冲）。见 §18.10 |
| `mix-blend-mode` | ✅ | O | `normal`/`multiply`/`screen`/`overlay`/`darken`/`lighten`/`difference`；其余**构建期报错**。见 §18.10 |
| `background-blend-mode` | ❌ | O | 要"多层背景图之间的混合"，与元素混合不是一回事 |
| `isolation` | ❌ | O | 同上（为混合服务的） |
| `color-scheme` / `accent-color` / `caret-color` | ❌ | N | 与"主题由 CSS 自己写"重复 |

---

## 9. 字体与文本

### 9.1 字体

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `font-family` | ✅ | G | **B8 起是真列表**（`A, B, monospace`）：按顺序找第一个装得上的家族，逐字回退到后面的家族。blob v33 |
| `font-size` | ✅ | L | `px` / `%`（相对父级）/ `smaller` / `larger` |
| `font-weight` | ✅ | G | PC/Mobile：按 `@font-face` 声明的字面挑**真字重**（挑不到才合成加粗）；MCU：合成 |
| `font-style` | ✅ | G | 同上：PC/Mobile 用真斜体字面，MCU 合成倾斜 |
| `font`（简写） | ✅ | L | B9 落地（`font: bold 12px/1.5 serif`） |
| `font-variant-ligatures` / `font-feature-settings` | ⚠️ | G | 只认 `liga` / `clig` / `kern` 三个开关（其余特性**构建期报错**） |
| `font-variant-caps: small-caps` / `font-variant-numeric: tabular-nums` | ✅ | G | B9：HarfBuzz 的 `smcp` / `tnum` 特性；**没有整形能力（`QL_FEAT_CSS_TEXT_SHAPE=0`）时构建期报错**，不静默降级。样式里只占 1 字节（`variant_flags`） |
| `font-stretch` / `font-size-adjust` / `font-optical-sizing` | ❌ | X | 变量字体轴，B8 不做 |
| `@font-face` | ✅ | G | `font-family` / `src: url()` / `font-weight` / `font-style` 四样；`unicode-range` / `local()` 构建期报错 |

### 9.2 文本

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `text-align` | ✅ | L | `left` / `center` / `right` / **`justify`**（B9）；也作用于行内子节点，换行后每行各自对齐 |
| `text-align: justify` | ✅ | B | B9：除**最后一行**外把剩余空间平摊到字形间隙；只有一行时等同左对齐；`text-overflow: ellipsis` 的行不拉伸 |
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
| `word-wrap` | ✅ | L | `overflow-wrap` 的旧名字，同一套解析 |
| `hyphens` | ❌ | X | 要连字词典（各语言各一份） |
| `text-shadow` | ✅ | R | 复用 `box-shadow` 的 SDF 路径（字形级） |
| `text-justify` / `text-align-last` | ❌ | X | 只做 `text-align: justify`（最后一行的行为固定为"不拉伸"，与浏览器默认一致） |
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
| `transform: rotate()` | ✅ | F | M9.4：离屏图层 + 仿射重采样（§18.11）。**依赖 `QL_FEAT_CSS_FILTER_LAYER`**，裁掉时构建期报错 |
| `transform: skew()` / `skewX()` / `skewY()` | ✅ | F | 与 `rotate` 同一份仿射路径（错切矩阵） |
| `transform: matrix()` | ✅ | F | 六参；与 `rotate` 一样进矩阵池 |
| `transform: matrix3d()` | ❌ | S | 归 3D，不排期 |
| `transform: perspective()` / `rotate3d()` 等 3D | ❌ | S | 要真 3D 管线（透视除法 + 深度排序）。PC 档也**不排期**：`scaleX` 已能近似"翻面"，而 3D 需要一整套矩阵 + 分层合成 |
| `transform-origin` | ✅ | R | px / 千分比 / `left|center|right|top|bottom`；默认元素中心 |
| `transform-style` / `backface-visibility` | ❌ | S | 3D 专用，同上 |

---

## 13. 过渡与动画

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `transition`（简写） | ⚠️ | T | 可过渡：`background-color` `opacity` `color` `border-color` `border-radius` `transform` `clip-path`。逗号分组支持，但**各段必须共用同一组时长/缓动**（库里只有一份 `trans_ms`/`trans_ease`），不同时长报错 |
| `transition-property` | ✅ | T | `all` 或上面那批，可列多个（位图） |
| `transition-duration` | ✅ | T | |
| `transition-timing-function` | ✅ | T | 具名缓动 + `steps(n)` + `cubic-bezier(a,b,c,d)`（§18.16） |
| `transition-delay` | ✅ | T | 支持长写与简写（`transition: opacity .3s ease .2s`），见 §18.16 |
| `transition-behavior` / `-property: display` | ❌ | P | 离散属性过渡，与本库"帧间只改绘制值"冲突 |
| `animation`（简写）+ 8 个子属性 | ✅ | T | `name/duration/timing-function/iteration-count/direction/fill-mode/delay/play-state` **全齐** |
| `animation-composition` | ❌ | N | 多个动画叠加的合成规则，用得极少 |
| `@keyframes` | ✅ | B | 编译期摊平成静态表；运行期是纯函数 `u = f(now - start)` |
| `animation-play-state: paused` | ✅ | T | 只是停止推进，不产生持续重绘 |

---

## 14. 滤镜、遮罩、混合

| 属性 | 状态 | 原因 | 备注 |
|---|---|---|---|
| `backdrop-filter: blur() saturate()` | ✅ | R | 毛玻璃；读"已画好的内容"就地模糊，有独立小着色器；`saturate(<N>%)` 把糊完的均值色推艳（`0`/`100%` = 不变），两个函数任意顺序、可只写一个；结果裁进 `border-radius`；自己带 `transform: translate` 时采样区跟着走；下层动画实时跟随 |
| `filter: blur(<N>px)` | ✅ | O | **离屏图层**（§18.10）：元素先合成到临时图再整体糊。9 抽头可分离高斯，抽头间距 = N/2，两条后端同一公式。MCU 档恒关 |
| `filter: brightness/contrast/saturate/grayscale/sepia/invert/hue-rotate/opacity` | ✅ | O | 颜色矩阵族，同一图层里可链式（最多 6 个）；`1.2` 与 `120%` 等价 |
| `filter: drop-shadow(<dx> <dy> [blur] [color])` | ✅ | O | 影子取 **alpha 形状**（跟着圆角/透明洞走）；颜色默认 `currentColor`；`drop-shadow` **不接受** `spread`（CSS 也没有） |
| `filter: url(#svg)` / `backdrop-filter` 的其它函数 | ❌ | X | 前者要内联 SVG 滤镜图；后者只有 `blur()` + `saturate()` 两个（`brightness`/`contrast`/`hue-rotate` 等明确报错，不静默忽略） |
| `clip-path: circle()` / `ellipse()` | ✅ | R | 子树级**椭圆**遮罩（圆心恒在元素中心）；GPU 顶点属性带 `(cx,cy,rx,ry)`，CPU 同一公式 |
| `clip-path: inset()` | ✅ | R | **与裁剪矩形求交**，两后端精确、零额外成本（B9） |
| `clip-path: polygon()` | ✅ | F | B9：顶点进 doc 池（同形状去重）→ 绘制期解析覆盖率（奇偶内点 + 到最近边距离），CPU/GPU 同公式、零图层 |
| `clip-path: path()` / `url()` | ❌ | X | 要 SVG 路径解析器；明确报错 |
| `clip-path: circle(… at x y)` / `inset(… round R)` / `polygon(… evenodd)` | ❌ | F | 明确报错并给替代做法（圆心恒居中；圆角矩形裁剪请用 `border-radius` + `overflow: hidden`） |
| `mask` / `mask-image` | ✅ | O | `url(<name>)` / `none`；只用图片 alpha，拉伸到边框盒（= `mask-size: 100% 100%` + `mask-mode: alpha`）。见 §18.10 |
| `mask-position` / `-repeat` / `-composite` / `mask-border` | ❌ | O | 语义分支多，收益低 |
| `mix-blend-mode` | ✅ | O | 与 `filter` 共用同一套离屏图层；GPU 侧要把目标表面抓成纹理再算 |

---

## 15. 多栏、分页、打印（B7 后半 ✅ 已完成，MCU 归"大核"包）

| 属性 | 状态 | 档位 | 备注 |
|---|---|---|---|
| `column-count` / `column-width` / `columns` / `column-gap` | ✅ | 大核 | blob v32；`columns` 简写按"最长一个整数当栏数、带单位的当栏宽"拆 |
| `column-rule` / `-width` / `-style` / `-color` | ✅ | 大核 | 复用 `border` 的虚线绘制（`paint_dash_run`），画在缝的正中间 |
| `column-span: all` / `column-fill` / `break-*` | ❌ | — | `S`：跨列要打断"盒内分裂"的不变量；`break-inside` 在 GUI 里几乎用不到 |
| `page-break-*` / `orphans` / `widows` | — | — | `N`：本库不打印 |
| `@page` / `@media print` | — | — | `N`：同上（`@media print` 目前**整块跳过**，不报错） |

**实现收窄成"贪心分栏"**（详见 §18.13）：按顺序把子节点塞进当前栏，塞不下就换下一栏，
**不把一个子节点劈到两栏**。所以本项并没有真的"把一份内容排两遍"：

- 容器给了 `height` → 直接用它当栏高，一遍排完（只多一次"量高度"的哑排）；
- 容器高度 auto → 高度已经量出来了，栏高用**二分**在"纯算术"层面求最小可容纳值
  （不重排），总代价是一次排版 + 十次左右"读 `border_box.h` 的循环"。

**为什么不做真·分栏**：真分栏要把每个子节点的绘制切成两段（断点可能在文字行中间），
本库的绘制/命中/滚动全都建立在"一个节点一个盒"上。收益（长文自动分栏）在嵌入式 UI 里
很小，而"卡片列表分两三栏"才是常见需求 —— 这一项用贪心就够了。

教程见 [23_columns](../../tutorial/23_columns.md)，例程 `example/34_columns`。

---

## 16. 表格（B7 前半 ✅ 已完成，MCU 归"大核"包）

| 属性 | 状态 | 档位 | 备注 |
|---|---|---|---|
| `display: table` / `table-row` / `table-cell` / `table-caption` / `table-row-group` | ✅ | 大核 | `<table>` `<tr>` `<td>` `<th>` `<thead>` `<tbody>` `<tfoot>` `<caption>` 一并可用（blob v31） |
| `table-layout: fixed` | ✅ | 大核 | **首行显式 `width`（或 `<col>`）决定列宽，其余等分**；只排一遍 |
| `table-layout: auto`（默认） | ✅ | 大核 | 量每列的 min/max-content 后按可用宽度插值：余量按 max-content 比例摊，够了就压到 min-content |
| `border-collapse` / `border-spacing` | ✅ | 大核 | `collapse` 用"朝左上挪半条边框 + 尺寸补回"实现，相邻边框重叠成一条 |
| `rowspan` / `colspan` | ✅ | 大核 | 每列一个"还剩几行被占"的计数器（24 B），不看行数 |
| `vertical-align: top/middle/bottom`（单元格内） | ✅ | 大核 | 非表格元素上仍然只认 `baseline/sub/super` |
| `caption-side: bottom` | ❌ | — | 标题恒在上方 |
| `<colgroup>` / `<col>` 列宽声明 | ✅ | 大核 | `display: table-column(-group)`；`fixed` 下优先于首行，`auto` 下是那一列的**下界**；`<col span>` 展开成多列 |
| `empty-cells` / `table-row` 的背景 | ❌ | — | 行不是盒子（浏览器也一样） |

**为什么不做了改成"要做"**：数据表格是**后台管理界面**的刚需，
而"用嵌套 flex 拼表格"拼不出列宽对齐 —— 这是真实的表达力缺口，不是省事。
PC/Mobile 档没有任何理由不做；MCU 档归"大核"包（`QL_FEAT_CSS_TABLE` 默认 0，
关掉时 `<table>` 与 `display: table*` **构建期报错**）。

实现细节与坑见 §18.12；教程见 [22_table](../../tutorial/22_table.md)。

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
| 只能"常量"的字段（例外：`border-radius` 现在吃 `%`，见 §7） | `font-size` / `border-width` / `letter-spacing` / `text-indent` / `margin` / `padding`：只接受构建期能折成 px 的 `calc()`（如 `calc(10px + 6px)`），带 `%`/`em`/`vw` 的报错 —— 这些字段存的是 `int16` 数值，没有放表达式下标的地方 |
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

剩下 5.5% 主要是 `float`（不做）、`list-style`（没有生成内容）。
> **2026-11-11 更新**：这一栏原来还列着 `cursor`、`@font-face` 的 `src`、`overflow-x/y` 分轴、
> `align-content` —— 后来都补上了（逐条状态见 §19.1 的实测表），真正还缺的只有 `float` 与 `list-style`。

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
| `border-radius` | 1 / 2 / 4 个值；px / em / rem / calc() / `%`（相对盒子宽、高） | 斜杠椭圆角**按两个半径的平均近似**（两后端都是单半径 SDF，见 §18.15）；未布局的节点（`display:none`）保留负值标记，`ql_ui_radius_max*` 按 0 处理 |
| `box-shadow` | 最多两层，含 `inset` / `spread` | 三层以上报错（不静默丢弃） |
| `opacity` | 逐图元降透明 | 子元素会一起淡，但**同一子树内部互相覆盖处会略透**（真 group 要离屏图层，见 §20.1） |
| `transform` | `translate`（**只吃 px**）/ `scale` / `rotate` / `skew` / `matrix` —— 全是**仿射**（2×2 + 平移） | `translate()` 的 `%` 报错；3D（`perspective()`/`rotate3d()`）不排期 |
| `transform: rotate/skew/matrix` | 走"离屏合成后重采样"（M9.4，`QL_FEAT_CSS_FILTER_LAYER`） | 布局盒不动、命中按旋转后的形状算；裁掉该包时构建期报错 |
| `transform-origin` | px / 千分比 / 关键字 | 没有三值写法（`left top 10px`） |
| flex 项的**主轴尺寸** | 用 `box-sizing: border-box` 时精确 | **content-box 时少算 padding + border**（`force_size` 把主轴尺寸按边框盒解释）—— flex 子项请写 `box-sizing: border-box`，这是真实页面的常规写法 |
| `@media` | 只有宽度断点 | `orientation` / `prefers-*` / `em` 单位报错；`print` 整块跳过 |
| `transition-timing-function` | 5 个具名 + `steps(n)` + `cubic-bezier(a,b,c,d)` | 没有 `linear()` |
| `font-family` | 真列表（`A, B, monospace`，逐字回退，blob v33） | 字体文件仍要由宿主提供或在构建期打包；MCU 档只有内建位图字体 |
| `font-weight` / `font-style` | PC/Mobile：按 `@font-face` 挑**真字重 / 真斜体**，挑不到才合成 | MCU 档只合成（不加载第二份字体文件） |
| `text-decoration` | `underline` / `overline` / `line-through` | 没有 `wavy` 线型 |
| `vertical-align` | `baseline` / `sub` / `super` | 没有 `middle` / 长度值 |
| `text-align` | `left` / `center` / `right` / `justify` | B9 起有 `justify`（末行不拉伸）；行内子节点一起对齐 |
| `overflow` | 单值管两轴，也支持 `overflow-x` / `overflow-y` 分轴 | 只有 `auto` 的那一轴能滚、能画滚动条 |
| `flex-wrap` | `nowrap` / `wrap` | 没有 `wrap-reverse` |
| `display` | `block` / `inline` / `inline-block` / `flex` / `inline-flex` / `grid` / `table` 全家 / `none` | 没有 `contents` / `list-item` |
| `%` 长度 | 根元素按视口；其余按包含块（`margin`/`padding` 用**内容宽**） | `margin`/`padding` 的 `%` 在布局期折 px（读写走 `margin_of()`/`padding_of()`）；固有尺寸计算里按 0 算 |
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

1. **`opacity` 在"有子节点"时走离屏图层，单节点时不走**。真 CSS 永远先合成整棵子树；
   本库对**单节点**保留"把 alpha 乘进每个绘制入口"的老做法 —— 因为一个节点内部没有
   "图元互相覆盖"这回事，两者逐像素等价，而省掉一层缓冲。一旦有子节点就必然走图层
   （否则重叠区会乘两次 alpha、比别处更深）。判据见 `style_needs_layer()`，实现见 §18.10。
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
| `CSS_TABLE` | `QL_FEAT_CSS_TABLE` | **B7 ✅**：`display: table` 全家 + `<table>` 标签 + `table-layout: auto/fixed` + `colspan/rowspan` + `border-collapse` + `<colgroup>/<col>` | ✅ | **默认关** | BASE | **实测 5,332 B**（见 §18.12 末） |
| `CSS_COLUMNS` | `QL_FEAT_CSS_COLUMNS` | **B7 ✅**：`column-count` / `column-width` / `columns` / `column-gap` / `column-rule*`（贪心分栏 + 等高平衡） | ✅ | **默认关** | BASE | **实测 640 B**（见 §18.13 末） |
| `CSS_FONT_STACK` | `QL_FEAT_CSS_FONT_STACK` | **B8 ✅**：`@font-face` + 自由 `font-family` 列表 + 逐字回退 + 真字重/真斜体 | ✅ | **默认关** | BASE + `QLGL_ENABLE_FREETYPE` | 与 FreeType 一起算；MCU 档根本没有 FT，开关只能关 |
| `CSS_TEXT_SHAPE` | `QL_FEAT_CSS_TEXT_SHAPE` | **B8 ✅**：HarfBuzz 整形（连字 / 字距 / 复杂文字），`font-variant-ligatures` / `font-feature-settings`（liga/clig/kern） | ✅ | **默认关** | BASE（+ FreeType 才有真正的整形；运行期 `dlopen` libharfbuzz） | MCU 上 **+0 B**（实现依赖 FT/download，硬件上没有） |
| `CSS_BIDI` | `QL_FEAT_CSS_BIDI` | **B8 ✅**：`direction` / `unicode-bidi` + RTL 排布（fribidi 或内建 UAX#9 子集） | ✅ | **默认关** | BASE（运行期 `dlopen` libfribidi） | MCU 上 **+0 B**（同上） |
| `CSS_BORDER_IMAGE` | `QL_FEAT_CSS_BORDER_IMAGE` | **B6 ✅**：`border-image` 九宫格 | ✅ | **默认开**（可裁） | BASE + `QL_FEAT_UI_IMAGE` | **实测 2,176 B**（−1.6%） |
| `CSS_FILTER_LAYER` | `QL_FEAT_CSS_FILTER_LAYER` | **M9.3 ✅**：离屏图层 = `filter` 全家 + `mix-blend-mode` + `mask` + 真 group opacity；**M9.4 ✅** 的 `transform: rotate/skew/matrix` 也挂在这个包上（它要靠离屏合成 + 重采样） | ✅ | **默认关** | BASE（+ `QL_FEAT_UI_IMAGE` 才有 `mask`） | **实测 Flash 15,344 B / RAM 676 B**（M9.3）+ **3,488 B**（M9.4，见 §18.11 末） |
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

- [CSS 进度规划](CSS3进度规划.html) —— 已完成 / 待做、批次计划（§0.1、§22）
- [CSS 开发日志](CSS3开发日志.html) —— 每一批落地时的做法、踩坑、代价（§18.4–§18.17）
- [HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html) —— 标签侧的同口径清单
- [QLUI 阅读指南](QLUI阅读指南.html) —— 样式 / 布局 / 绘制的实现细节与踩坑
- [Web 兼容设计](../../design/Web兼容设计.html) —— 分层与"为什么不做 JS / Grid"
- [开发规划](../../design/开发规划.html) —— 批次排期与验收
