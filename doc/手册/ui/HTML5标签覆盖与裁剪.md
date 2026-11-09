# HTML5 标签覆盖与 MCU 裁剪

这份文档回答两个问题：

1. **HTML5 的标签，本库到底支持到哪一步？** —— 全量清单，一个不漏，逐条给状态。
2. **MCU 上怎么办？** —— 编译期按能力包裁剪，但**枚举与 blob 不许跟着变**。

一句话承诺：

> **解析层 114/114**（不再有"不认识的标签"）；**渲染层按能力包**（PC/移动默认全开）；
> **MCU 编译期裁包**，裁剪只影响代码路径与错误文案，**不影响 `QLUITag` 枚举值、不影响 blob**。

---

## 1. 全量清单（114 个）

口径：HTML Living Standard 的元素索引，**不含 obsolete 元素**（`marquee` `center`
`font` `big` `strike` `tt` `frame` `nobr` `rb` `xmp` 等二十来个），也不含实验中的
`portal` / `fencedframe`。obsolete 那批按"明确失败"处理：写了就报错，不做宽容降级。

现状三档：

| 记号 | 含义 |
|---|---|
| ✅ 已支持 | 能解析、有 UA 默认样式、能画 |
| ⏳ 计划 | 标签能解析（或即将能），但渲染要么需要新能力（能力包），要么只做占位盒 |
| ✅ 已支持（解析跳过） | 能解析，但**不进树**：元数据本来就不产生盒子 |
| ❌ 不做 | 连占位都不做，解析期报错（本表里已经没有了 —— 见 §5） |

| 分类 | 标签 | 能力包 | 现状 |
|---|---|---|---|
| 根 / 文档外壳 | `<html>` | `CORE` | ✅ 已支持 |
| 文档元数据 | `<base>` | `META` | ✅ 已支持（解析跳过） |
| 文档元数据 | `<head>` | `META` | ✅ 已支持（解析跳过） |
| 文档元数据 | `<link>` | `META` | ✅ 已支持（解析跳过） |
| 文档元数据 | `<meta>` | `META` | ✅ 已支持（解析跳过） |
| 文档元数据 | `<style>` | `META` | ✅ 已支持（解析跳过） |
| 文档元数据 | `<title>` | `META` | ✅ 已支持（解析跳过） |
| 分段 | `<body>` | `CORE` | ✅ 已支持 |
| 分段 | `<address>` | `CORE` | ✅ 已支持 |
| 分段 | `<article>` | `CORE` | ✅ 已支持 |
| 分段 | `<aside>` | `CORE` | ✅ 已支持 |
| 分段 | `<footer>` | `CORE` | ✅ 已支持 |
| 分段 | `<header>` | `CORE` | ✅ 已支持 |
| 分段 | `<h1>` | `CORE` | ✅ 已支持 |
| 分段 | `<h2>` | `CORE` | ✅ 已支持 |
| 分段 | `<h3>` | `CORE` | ✅ 已支持 |
| 分段 | `<h4>` | `CORE` | ✅ 已支持 |
| 分段 | `<h5>` | `CORE` | ✅ 已支持 |
| 分段 | `<h6>` | `CORE` | ✅ 已支持 |
| 分段 | `<hgroup>` | `CORE` | ✅ 已支持 |
| 分段 | `<main>` | `CORE` | ✅ 已支持 |
| 分段 | `<nav>` | `CORE` | ✅ 已支持 |
| 分段 | `<search>` | `CORE` | ✅ 已支持 |
| 分段 | `<section>` | `CORE` | ✅ 已支持 |
| 分组 | `<blockquote>` | `CORE` | ✅ 已支持 |
| 分组 | `<dd>` | `CORE` | ✅ 已支持 |
| 分组 | `<div>` | `CORE` | ✅ 已支持 |
| 分组 | `<dl>` | `CORE` | ✅ 已支持 |
| 分组 | `<dt>` | `CORE` | ✅ 已支持 |
| 分组 | `<figcaption>` | `CORE` | ✅ 已支持 |
| 分组 | `<figure>` | `CORE` | ✅ 已支持 |
| 分组 | `<hr>` | `CORE` | ✅ 已支持 |
| 分组 | `<li>` | `CORE` | ✅ 已支持 |
| 分组 | `<menu>` | `CORE` | ✅ 已支持 |
| 分组 | `<ol>` | `CORE` | ✅ 已支持 |
| 分组 | `<p>` | `CORE` | ✅ 已支持 |
| 分组 | `<pre>` | `TEXT` | ✅ 已实现 |
| 分组 | `<ul>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<a>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<abbr>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<b>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<bdi>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<bdo>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<br>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<cite>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<code>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<data>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<dfn>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<em>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<i>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<kbd>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<mark>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<q>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<rp>` | `TEXT` | ⏳ 计划 |
| 行内文本 | `<rt>` | `TEXT` | ⏳ 计划 |
| 行内文本 | `<ruby>` | `TEXT` | ⏳ 计划 |
| 行内文本 | `<s>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<samp>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<small>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<span>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<strong>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<sub>` | `TEXT` | ✅ 已实现 |
| 行内文本 | `<sup>` | `TEXT` | ✅ 已实现 |
| 行内文本 | `<time>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<u>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<var>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<wbr>` | `CORE` | ✅ 已支持 |
| 编辑标记 | `<del>` | `CORE` | ✅ 已支持 |
| 编辑标记 | `<ins>` | `CORE` | ✅ 已支持 |
| 嵌入 | `<area>` | `IMAGE` | ✅ 已支持（P3e：热区，不产生盒子） |
| 嵌入 | `<embed>` | `EMBED` | ✅ 占位 |
| 嵌入 | `<iframe>` | `EMBED` | ✅ 占位 |
| 嵌入 | `<img>` | `IMAGE` | ✅ 已支持（P3a） |
| 嵌入 | `<map>` | `IMAGE` | ✅ 已支持（P3e：热区表，`display:none`） |
| 嵌入 | `<object>` | `EMBED` | ✅ 占位 |
| 嵌入 | `<picture>` | `IMAGE` | ✅ 已支持（P3e：透明容器，`inline-block`） |
| 嵌入 | `<source>` | `IMAGE` | ✅ 已支持（P3e：空元素，`display:none`；不做候选选择） |
| 媒体 | `<audio>` | `EMBED` | ✅ 占位 |
| 媒体 | `<track>` | `EMBED` | ✅ 占位 |
| 媒体 | `<video>` | `EMBED` | ✅ 占位 |
| 表格 | `<caption>` | `TABLE` | ⏳ 计划 |
| 表格 | `<col>` | `TABLE` | ⏳ 计划 |
| 表格 | `<colgroup>` | `TABLE` | ⏳ 计划 |
| 表格 | `<table>` | `TABLE` | ⏳ 计划 |
| 表格 | `<tbody>` | `TABLE` | ⏳ 计划 |
| 表格 | `<td>` | `TABLE` | ⏳ 计划 |
| 表格 | `<tfoot>` | `TABLE` | ⏳ 计划 |
| 表格 | `<th>` | `TABLE` | ⏳ 计划 |
| 表格 | `<thead>` | `TABLE` | ⏳ 计划 |
| 表格 | `<tr>` | `TABLE` | ⏳ 计划 |
| 表单 | `<button>` | `CORE` | ✅ 已支持 |
| 表单 | `<datalist>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<fieldset>` | `CORE` | ✅ 已支持 |
| 表单 | `<form>` | `CORE` | ✅ 已支持 |
| 表单 | `<input>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<label>` | `CORE` | ✅ 已支持 |
| 表单 | `<legend>` | `CORE` | ✅ 已支持 |
| 表单 | `<meter>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<optgroup>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<option>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<output>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<progress>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<select>` | `CONTROL` | ✅ 已支持 |
| 表单 | `<textarea>` | `CONTROL` | ✅ 已实现 |
| 交互 | `<details>` | `CONTROL` | ✅ 已支持 |
| 交互 | `<dialog>` | `CONTROL` | ✅ 已支持 |
| 交互 | `<summary>` | `CONTROL` | ✅ 已支持 |
| 脚本 | `<canvas>` | `EMBED` | ✅ 占位 |
| 脚本 | `<noscript>` | `EMBED` | ✅ 占位 |
| 脚本 | `<script>` | `EMBED` | ✅ 占位 |
| Web Components | `<slot>` | `EMBED` | ✅ 占位 |
| Web Components | `<template>` | `EMBED` | ✅ 占位 |
| SVG / MathML | `<math>` | `EMBED` | ✅ 占位 |
| SVG / MathML | `<svg>` | `EMBED` | ✅ 占位 |

统计：**✅ 101 个**（P0 加 31 个纯语义标签、P1 加 6 个元数据标签、
P2a 加 4 个控件、P2b/P2c 加 `details`/`summary`/`dialog`、P3a 加 `img`、
P3e 加 `picture`/`source`/`map`/`area`、P7 加 13 个占位标签、
P4a/P4b 加 `pre`/`sub`/`sup`、P5 加 `textarea`）、
**⏳ 13 个**、**❌ 0 个**。

其中 101 个里 **13 个是占位**（`EMBED` 包，只画提示盒或不产生盒子），
真正按 HTML 语义渲染的是 **88 个**。

---

## 2. 能力包

一个"包"= 一组标签 + 支撑它们的那部分引擎。包是**编译期开关**，也是**构建期报告的单位**。

> **现状（老实说）**：除 `QL_FEAT_UI_META`、`QL_FEAT_UI_IMAGE` 与
> `QL_FEAT_IMG_*` 之外，表里的宏目前**只有名字、还没进代码**（`grep` 不到）。
> 它们是"裁剪时的口径"，不是现在能 `-D` 的东西。已落地的开关见
> [图像管线设计 §2](../../design/图像管线设计.html) 与 `include/qlgl/qlconfig.h`。

| 包 | 宏 | 含标签（个数） | PC/移动默认 | MCU 默认 | 依赖 |
|---|---|---|---|---|---|
| `CORE` | 恒开 | 62 | ✅ | ✅ | 无（永远有） |
| `META` ✅ | `QL_FEAT_UI_META` | 6（`base head link meta style title`） | ✅ | ✅ | 无 —— 解析期整段跳过，不产生盒子 |
| `TEXT` | `QL_FEAT_UI_TEXT` | 6（`pre sub sup ruby rt rp`） | 4/6 ✅ | ❌ | 已做：`white-space: pre/pre-wrap/pre-line`、`vertical-align: sub/super`、两级字体族；未做：`ruby`/`rt`/`rp` 行内注音 |
| `TABLE` | `QL_FEAT_UI_TABLE` | 10（`table` 全家） | ✅ | ❌ | 表格布局算法（列宽分配、跨行跨列、边框合并） |
| `IMAGE` ✅ | `QL_FEAT_UI_IMAGE`<br>+ `QL_FEAT_IMG_BMP/QOI/PNG/GIF`（各自一个开关）<br>+ `QL_FEAT_IMG_PLATFORM` / `QL_FEAT_IMG_LIBJPEG`（JPEG） | 5（`img picture source map area`） | ✅ | ✅（用 `raw-only`：只吃构建期转好的 `RAW`，零解码器、零图片 RAM） | 开关正交、组合都合法；`full`/`lean`/`raw-only` 只是预设。构建期转码是零成本路径，任何平台可选。设计见[图像管线设计](../../design/图像管线设计.html) |
| `CONTROL` | `QL_FEAT_UI_CONTROL` | 12（`input select option optgroup` + `progress meter output datalist textarea details summary dialog`） | ✅ | ❌ | **需要专用运行期状态或覆盖层**：输入框池（光标/选区）、下拉池 + 覆盖面板、进度池、浮层栈、展开状态；`textarea` 复用输入框池 |
| `EMBED` ✅ | `QL_FEAT_UI_EMBED` | 13（`iframe embed object audio video track canvas script noscript slot template svg math`） | ✅ | ✅（**占位是零成本的**，保留也几乎不花 Flash） | **只做占位盒 / 不产生盒子**，见 §5 |

`CORE` 里那 62 个是"不需要任何额外运行期机制、只靠样式+布局就能画"的那批
（含滚动容器）。**判据是"要不要专用机制"，不是"有没有状态"**：

- `<input>` / `<select>` 是状态最重的两个（光标、选区、覆盖面板），但正因为如此
  它们必须能被裁 —— 所以归 `CONTROL`；
- `<output>` / `<datalist>` 其实零成本（前者等价 `span`，后者 `display:none`），
  仍归 `CONTROL` 是因为**用户视角它们是表单控件**，一起裁更省心。

---

## 3. 一条硬约束：枚举不可条件化

`QLUITag` 的数值会**写进 blob**（`QLUINode.tag` 是 `uint16` 索引），而且三张表
（`QLUITag` / `k_tags[]` / `s_tag_names[]`，外加 `qltext` 的 `TAG_NAMES[]`）必须同序。

所以：

| 允许 | 禁止 |
|---|---|
| 在**末尾追加**枚举项 | 因为"MCU 不用"而删掉/挪动枚举项 |
| 用宏裁掉**函数体、UA 默认分支、绘制路径** | 用宏裁掉**枚举项本身**（值会错位，blob 跨平台就废了） |
| 用宏裁掉**报错文案** | 让同一份 `.qlui` 在两个平台上解释出不同标签 |

结论：**枚举与名字表永远全量**。代价是 `.rodata` 里多约 1.2 KB 字符串
（114 条名字 + 指针表），换回来的是"一份 blob 到处能读、调试名永远对得上"。

> 如果某天真的差这 1 KB，再加一个 `QL_CFG_UI_TAG_NAMES_MIN`（名字表退化成
> `ql_ui_tag_name()` 返回 `"?"`），那是**独立的一步**，和裁包无关。

---

## 4. 裁包之后会发生什么

> **现状**：机制与第一个包（`META`）已经落地，开关在 `qlconfig.h`：
> `QL_FEAT_UI_META`（默认 1）与 `QL_CFG_UI_LENIENT_TAGS`（默认 0）。
> 其余包（`TEXT` / `TABLE` / `IMAGE` / `CONTROL` / `EMBED`）随实现追加，
> 在那之前它们的标签还不在枚举里，遇到就是"不认识的标签"报错。

解析器先按名字认标签（`tag_index()`，全量表），再看这个标签所属的包开没开：

| 模式 | 行为 |
|---|---|
| `strict`（默认） | **明确报错**，文案里点名缺哪个包：<br>`<table>: 本构建没有启用 QL_FEAT_UI_TABLE（表格布局）` |
| `lenient` | 降级成 `div`（块级）/ `span`（行内），并计入 `doc->degraded_tags`，`ql_ui_load_text` 之后可查 |

两条都**不静默**：strict 失败在编译期（CI 能拦住），lenient 有计数与名单（运行时能打日志）。
这就是本库"明确失败优于半个正确"的落地方式 —— 降级是**显式声明的**，不是悄悄发生。

`lenient` 的典型用途：MCU 上贴外部 HTML 片段、只想要"能看就行"。

---

## 5. 现在是占位的 13 个（`EMBED`）✅

### 5.1 先说清楚"占位"是什么意思

**占位是默认档，不是终点。** 这一包的原则是：

> **能接外部 C 库就接，真的接不了才留占位；不同平台接不同的库；
> PC / 移动默认开，MCU 默认关，但硬要在 MCU 上保留也允许。**

所以下面 13 行现在全是 ✅ —— ✅ 的意思是"**认识且不炸**"（整页 HTML 贴进来能编译过），
不是"已经做到头了"。每行都标了它属于哪一档：

| 档 | 含义 | 标签 |
|---|---|---|
| **A** | 自己就能做，不需要外部库，也不动架构前提 | `iframe` `embed` `object` `canvas` `track` |
| **B** | 需要外部库，但接进来是加法 | `svg` `audio` `video` |
| **C** | 库存在，但接了要推翻"构建期定型 / 无运行期重排 / 脏区驱动" | `script` `slot` `template` |
| **D** | 没有可用的 C 库，自己写等于重做一个布局引擎 | `math` |

**全部后端的接口设计、开关结构、工作量与排序见 [EMBED 后端设计](../../design/EMBED后端设计.html)。
目前代码里这 13 个只有占位实现，一个后端都没接（未排期）。**

### 5.2 现状逐条

| 标签 | 档 | 为什么现在是占位 / 能不能接后端 | 内容怎么处理 | 盒子 |
|---|---|---|---|---|
| `script` | **C** | 引擎能接（QuickJS / JerryScript / Duktape），但 `el.style.x=` / `appendChild` / `rAF` 分别要求运行期重算样式、运行期建节点、持续全帧重绘 —— **三条都是反向红线**。不做 | **整段跳过**（按 raw text 扫到 `</script>`，内容里的 `<` `</div>` 都是普通字符） | `display:none` |
| `template` | **C** | 惰性内容，没有 JS 就没有任何东西会去克隆它。可以另做"构建期宏展开"，但那是 QLGL 自己的语言特性，不是 HTML 语义 | **整段跳过**（标签配平扫描） | `display:none` |
| `slot` | **C** | 没有 shadow DOM 就没有可插的树。同上 | 解析但隐藏 | `display:none` |
| `svg` | **B** | **可接，ROI 最高**：plutosvg（MIT，自带光栅器）。两条路 —— 构建期转 RAW（MCU 唯一档）或运行期后端（PC/移动），和 JPEG 后端同形 | **整段跳过**（内容不是 HTML：`<path>`/`<circle>` 真去解析就报错） | 占位盒 `120×60` inline-block |
| `audio` | **B** | **可接**：`dr_wav`/`dr_mp3`/`dr_flac` + **miniaudio**（ALSA/Pulse/CoreAudio/WASAPI/AAudio/OpenSL 一套通吃）。真正的工作量是 `<audio controls>` 控件，不是解码 | 解析但隐藏 | 占位盒 `300×32` |
| `video` | **B** | **可接，成本最大**：起步用 pl_mpeg（MPEG-1，单文件 MIT，无依赖）；ffmpeg 作 PC 全格式后端，但要接受 5–15 MB 和 **LGPL 链接要求**。还需要逐帧 blit 的双缓冲 | 解析但隐藏 | 占位盒 `300×150` |
| `iframe` | **A** | **不需要任何库**：构建期把 `src` 文档编进来，运行期当子视口渲染（裁剪 + 滚动都现成）。唯一新机制是**事件坐标路由** | 解析但隐藏 | 占位盒 `300×150` block |
| `embed` `object` | **A** | **纯类型分派器**，几十行：`src` 是图片走 `<img>` 那条完整路径，是 HTML 走 `iframe`，其余占位 | 解析但隐藏 | 占位盒 `300×150` block |
| `canvas` | **A** | HTML 语义要 JS（永远空白）→ 死路。但**换成 C API 渲染回调就完全成立**：`ql_ui_canvas_bind()` 让应用代码在渲染期往盒子里画图表/仪表/波形 | 解析但隐藏 | 占位盒 `300×150` block |
| `track` | **A** | **依附** audio/video：WebVTT 解析约 200 行 + 字幕叠画。没有 media 就没有意义 | 空元素 | `display:none` |
| `math` | **D** | C 世界没有可用的 MathML 引擎；自己写 ≥ `TABLE` 包的工作量。**旁路**：构建期用 TeX/KaTeX 渲染成 SVG/位图，走 `svg` 或图像管线 | **整段跳过**（`<mi>`/`<mo>` 不是 HTML 标签） | 占位盒 `120×60` inline-block |
| `noscript` | — | 纯语义。既然本库永久无脚本，按规范内容**应该显示**；现在是隐藏（实用主义），后续改成开关 `QL_CFG_UI_NOSCRIPT_SHOW` | 解析但隐藏 | `display:none` |

**占位盒长什么样**：1px 实线边框 + 居中的**标签名**（`iframe` / `video` / `svg`…）。
用标签名而不是"未支持"两个字，是因为内建位图字体只有 ASCII（中文会变成 `???`），
而且标签名本身就是排查线索。盒子小于 32×12 时不画字（糊成一团没意义）。
想彻底去掉提示就写 CSS：`iframe { border-width: 0; width: 0; height: 0 }`。

**两处与浏览器不同的取舍**（都写在这里，不藏着）：

1. **`<noscript>` 的内容不显示。** 严格按规范，QLGL 没有 JS ⇒ 脚本"总是不可用" ⇒
   应该显示 noscript 的内容。但这类内容在网页上通常是"请开启 JavaScript"式的降级
   提示，在 QLGL 里显示出来只会是噪音。要显示就自己写 `noscript { display: block }`。
   后续会把这个取舍变成开关 `QL_CFG_UI_NOSCRIPT_SHOW`（默认仍是隐藏，见
   [EMBED 后端设计 §6.8](../../design/EMBED后端设计.html)），**目前该宏还不存在**。
2. **`<canvas>` / `<iframe>` / `<object>` / `<video>` 的兜底内容不显示。**
   浏览器在"不支持"时显示它们；这里用占位盒代替，避免一页里出现两套"做不到"的说明。

**`<svg>` 必须跳内容，不能只 `display:none`** —— 这是实现时踩到的坑：
`<svg>` 里的 `<path>` / `<circle>` 都不是 HTML 标签，真解析会报"不支持的标签"，
整页编译不过。而"整页 HTML 贴进来能编译"正是这一包存在的全部理由。

**顺带修的一条容错**：空元素（`br` / `img` / `input` / `embed` / `track` …）的
结束标签，浏览器是**忽略**（规范里叫 parse error 而非语法错误），这里也照做 ——
否则 `<embed></embed>` 这种看着很正常的写法会让整页编译不过，而错在哪用户看不出来。
非空元素的结束标签不匹配仍然**明确报错**。

---

## 6. 构建期就该知道结果

> **现状**：下面的 `--features` 参数**还没实现**（`qltext check` 现在只报"能否编译"）。
> 这一节是设计意图 —— 包的名字在这里是**用户可见的报错单位**，所以名字得起对。

`qltext` 拿得到目标平台的能力包，所以不用等到跑起来才发现被降级：

```bash
$ qltext check --html app.html --css app.css --features core,image
app.html: 42 个节点，使用标签 9 种
  <table>   ×2  需要 QL_FEAT_UI_TABLE  → 会被降级成 div
  <img>     ×3  ✅
  <ruby>    ×1  需要 QL_FEAT_UI_TEXT   → 会被降级成 span
结论：3 处降级。要么开包，要么改界面。
```

同一套开关也用在 `qltext cc --features ...`（生成 C 时直接按目标平台裁）。

---

## 7. 分期与工作量

按"先解锁数量、再补能力"排，每期都能独立验收、独立提交：

| 期 | 内容 | 新增可画标签 | 估时 |
|---|---|---|---|
| P0 ✅ 已完成 | 31 个纯语义标签（`a` `code` `del` `ins` `dl` `form` `fieldset`…） | 66 | 1 天 |
| P1 ✅ 已完成 | `META` 包：元数据标签一律跳过（`<style>` 内容整段不解析）+ 包机制骨架 | 72 | 0.5 天 |
| P2a ✅ 已完成 | `CONTROL` 一半：`progress` `meter`（value/max → 完成度池）、`output`、`datalist` | 76 | 1 天 |
| P2b ✅ 已完成 | `details` `summary`：收起时把子节点指向共享隐藏样式 | 78 | 0.5 天 |
| P2c ✅ 已完成 | `dialog`：真正的浮层（居中 + 遮罩 + `Esc` / 点外面关闭） | 79 | 1 天 |
| P3a ✅ 已完成 | `QLImage` + `blit_image`（**CPU + GPU**）+ `<img>` + doc 资源段（blob v13）+ 固有尺寸 | 80 | 3–4 天 |
| P3f ✅ 已完成 | `qltext cc --root`：构建期把图片解码成目标像素格式并嵌入 doc | 80 | 1.5–2 天 |
| P3b ✅ 已完成 | BMP + QOI 解码器（构建期与运行时共用同一份） | 80 | 1 天 |
| P3d ✅ 已完成 | `object-fit` 五态 + 图片跟随 `border-radius` 裁剪 | 80 | 1 天 |
| P3e ✅ 已完成 | `map`/`area` 热区（rect/circle/poly/default，构建期绑定 `<img>`）+ `picture`/`source` | 84 | 0.5 天 |
| P3c ✅ 已完成 | PNG 解码（`qlinflate.c` + 容器：位深 1/2/4/8/16 × 颜色类型 0/2/3/4/6 × filter 0–4）+ 对拍 + fuzz | 84 | 3–4 天 |
| P3g ✅ 已完成 | 平台后端接口 + libjpeg-turbo 参考实现（JPEG）+ **GIF 首帧（自有 LZW）** | 84 | 1 天 |
| P4 | `TEXT` 包：文本内 `\n` + `white-space: pre` + 等宽字体 + `sub`/`sup` + `ruby` | 90 | 5–6 天 |
| P5 | `CONTROL` 补齐：`textarea`（多行编辑） | 91 | 2–3 天 |
| P6 | `TABLE` 包：表格布局算法 | 101 | 5–8 天（**风险最高**） |
| P7 ✅ 已完成 | `EMBED` 包：13 个占位盒 + `script`/`svg`/`math`/`template` 内容跳过 | 114 | 1–2 天 |
| P4a ✅ 已完成 | `TEXT`：`white-space: pre / pre-wrap / pre-line`（空白折叠推迟到建表期）+ `<pre>` | 111 | 1–2 天 |
| P4b ✅ 已完成 | `TEXT`：`<sub>` / `<sup>`（`vertical-align: sub/super`）+ `font-size: smaller/larger` | 113 | 1 天 |
| P4c ✅ 已完成 | `TEXT`：等宽字体族两级（`font-family: monospace` → `ql_font_mono`） | 113 | 0.5 天 |
| P5 ✅ 已完成 | `CONTROL`：`<textarea>` 多行编辑（独立 arena、视觉行光标、sticky 列） | 114* | 1–2 天 |
| P8 | `<input>` 的其余 `type`（`checkbox` `radio` `range` `number` `search`） | — | 3–5 天 |

总计约 **24–34 人日**。P6 单独标风险：它是唯一一处"要从零写一套布局算法"的地方，
其余都是在已有机制上加分支。

---

## 8. 与文档其它地方的口径

- 现在：**已支持 101 / 114**（含 13 个占位），未支持 13（见 §1 的统计行）。
- 教程的"[与 Web 的差别](../../tutorial/10_vs_web.html)"列的是**用户会踩到的差异**，不重复这份清单。
- 各标签的 UA 默认样式见[阅读指南 §5.4.7](QLUI阅读指南.html)；
  属性白名单见 [API 手册 §3.2](QLUI API手册.html)。
