# HTML5 标签覆盖与 MCU 裁剪

这份文档回答两个问题：

1. **HTML5 的标签，本库到底支持到哪一步？** —— 全量清单，一个不漏，逐条给状态。
2. **MCU 上怎么办？** —— 编译期按能力包裁剪，但**枚举与 blob 不许跟着变**。

一句话承诺：

> **解析层 114/114**（不再有"不认识的标签"）；**渲染层按能力包**（PC/移动默认全开）；
> **MCU 编译期裁包**，裁剪只影响代码路径与错误文案，**不影响 `QLUITag` 枚举值、不影响 blob**。

## 章节分布（三份文档）

这份"标签覆盖与裁剪"拆成三份，**章节编号保持不变**（外部文档按 § 号引用）：

| 文档 | 装什么 | 章节 |
|---|---|---|
| **本文 = 清单** | 114 个元素逐条状态、能力包 / 裁剪、构建期约束 | §1–§4、§6、§8 |
| [HTML5 标签进度规划](HTML5标签进度规划.html) | 分期与工作量 | §7 |
| [HTML5 标签开发日志](HTML5标签开发日志.html) | `EMBED` 占位 13 个的现状与落地记录 | §5 |

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
| ❌ 不做 | 连占位都不做，解析期报错（本表里已经没有了 —— 见 [开发日志 §5](HTML5标签开发日志.html)） |

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
| 表格 | `<caption>` | `TABLE` | ✅ 可用 |
| 表格 | `<col>` | `TABLE` | ✅ 可用（`display: table-column`，**只提供列宽**，不产生盒子；`span` 展开成多列） |
| 表格 | `<colgroup>` | `TABLE` | ✅ 可用（`display: table-column-group`，同上） |
| 表格 | `<table>` | `TABLE` | ✅ 可用 |
| 表格 | `<tbody>` | `TABLE` | ✅ 可用 |
| 表格 | `<td>` | `TABLE` | ✅ 可用 |
| 表格 | `<tfoot>` | `TABLE` | ✅ 可用 |
| 表格 | `<th>` | `TABLE` | ✅ 可用 |
| 表格 | `<thead>` | `TABLE` | ✅ 可用 |
| 表格 | `<tr>` | `TABLE` | ✅ 可用 |
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
| `TEXT` | `QL_FEAT_UI_TEXT` | 6（`pre sub sup ruby rt rp`） | 4/6 ✅ | ⚙️ 可裁 | 已做：`white-space: pre/pre-wrap/pre-line`、`vertical-align: sub/super`、两级字体族；未做：`ruby`/`rt`/`rp` 行内注音（要"注音排版"，PC/Mobile 档排期） |
| `TABLE` | `QL_FEAT_CSS_TABLE`（与 CSS 表格包同一个开关） | 10（`table` 全家） | ✅ **B7 已完成（2026-11-11）** | ⚙️ 默认关 | 表格布局（`table-layout: auto/fixed`、`colspan/rowspan`、`border-collapse`、`<colgroup>/<col>` 列宽）。实现见 [CSS 开发日志 §18.12](CSS3开发日志.html)，教程见[表格](../../tutorial/22_table.html) |
| `IMAGE` ✅ | `QL_FEAT_UI_IMAGE`<br>+ `QL_FEAT_IMG_BMP/QOI/PNG/GIF`（各自一个开关）<br>+ `QL_FEAT_IMG_PLATFORM` / `QL_FEAT_IMG_LIBJPEG`（JPEG） | 5（`img picture source map area`） | ✅ | ✅（用 `raw-only`：只吃构建期转好的 `RAW`，零解码器、零图片 RAM） | 开关正交、组合都合法；`full`/`lean`/`raw-only` 只是预设。构建期转码是零成本路径，任何平台可选。设计见[图像管线设计](../../design/图像管线设计.html) |
| `CONTROL` | `QL_FEAT_UI_CONTROL` | 12（`input select option optgroup` + `progress meter output datalist textarea details summary dialog`） | ✅ | ❌ | **需要专用运行期状态或覆盖层**：输入框池（光标/选区）、下拉池 + 覆盖面板、进度池、浮层栈、展开状态；`textarea` 复用输入框池 |
| `EMBED` ✅ | `QL_FEAT_UI_EMBED` | 13（`iframe embed object audio video track canvas script noscript slot template svg math`） | ✅ | ✅（**占位是零成本的**，保留也几乎不花 Flash） | **只做占位盒 / 不产生盒子**，见 [开发日志 §5](HTML5标签开发日志.html) |

`CORE` 里那 62 个是"不需要任何额外运行期机制、只靠样式+布局就能画"的那批
（含滚动容器）。**判据是"要不要专用机制"，不是"有没有状态"**：

- `<input>` / `<select>` 是状态最重的两个（光标、选区、覆盖面板），但正因为如此
  它们必须能被裁 —— 所以归 `CONTROL`；
- `<output>` / `<datalist>` 其实零成本（前者等价 `span`，后者 `display:none`），
  仍归 `CONTROL` 是因为**用户视角它们是表单控件**，一起裁更省心。

---

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

---

## 4. 裁包之后会发生什么

> **现状**：机制与第一个包（`META`）已经落地，开关在 `qlconfig.h`：
> `QL_FEAT_UI_META`（默认 1）与 `QL_CFG_UI_LENIENT_TAGS`（默认 0）。
> 其余包（`TEXT` / `TABLE` / `IMAGE` / `CONTROL` / `EMBED`）随实现追加，
> 在那之前它们的标签还不在枚举里，遇到就是"不认识的标签"报错。

解析器先按名字认标签（`tag_index()`，全量表），再看这个标签所属的包开没开：

| 模式 | 行为 |
|---|---|
| `strict`（默认） | **明确报错**，文案里点名缺哪个包：<br>`<table>: 这个构建关掉了表格（QL_FEAT_CSS_TABLE=0）` |
| `lenient` | 降级成 `div`（块级）/ `span`（行内），并计入 `doc->degraded_tags`，`ql_ui_load_text` 之后可查 |

两条都**不静默**：strict 失败在编译期（CI 能拦住），lenient 有计数与名单（运行时能打日志）。
这就是本库"明确失败优于半个正确"的落地方式 —— 降级是**显式声明的**，不是悄悄发生。

`lenient` 的典型用途：MCU 上贴外部 HTML 片段、只想要"能看就行"。

---

---

## 6. 构建期就该知道结果

> **现状**：下面的 `--features` 参数**还没实现**（`qltext check` 现在只报"能否编译"）。
> 这一节是设计意图 —— 包的名字在这里是**用户可见的报错单位**，所以名字得起对。

`qltext` 拿得到目标平台的能力包，所以不用等到跑起来才发现被降级：

```bash
$ qltext check --html app.html --css app.css --features core,image
app.html: 42 个节点，使用标签 9 种
  <table>   ×2  需要 QL_FEAT_CSS_TABLE  → 会被降级成 div
  <img>     ×3  ✅
  <ruby>    ×1  需要 QL_FEAT_UI_TEXT   → 会被降级成 span
结论：3 处降级。要么开包，要么改界面。
```

同一套开关也用在 `qltext cc --features ...`（生成 C 时直接按目标平台裁）。

---

---

## 8. 与文档其它地方的口径

- 现在：**已支持 111 / 114**（含 13 个占位），未支持 3（见 §1 的统计行）。
- 表格全家（10 个标签）在 B7 已经落地（`QL_FEAT_CSS_TABLE`，MCU 默认关）；
  现在只剩 `ruby`/`rt`/`rp`（注音排版，PC 排期）。
  判据见 [CSS 覆盖表 §0 / §2.2](CSS3覆盖与裁剪.html)：**"MCU 装不下"不再是"不做"的理由**。
- 教程的"[与 Web 的差别](../../tutorial/10_vs_web.html)"列的是**用户会踩到的差异**，不重复这份清单。
- 各标签的 UA 默认样式见[阅读指南 §5.4.7](QLUI阅读指南.html)；
  属性白名单见 [API 手册 §3.2](QLUI API手册.html)。
