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
| 分组 | `<pre>` | `TEXT` | ⏳ 计划 |
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
| 行内文本 | `<sub>` | `TEXT` | ⏳ 计划 |
| 行内文本 | `<sup>` | `TEXT` | ⏳ 计划 |
| 行内文本 | `<time>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<u>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<var>` | `CORE` | ✅ 已支持 |
| 行内文本 | `<wbr>` | `CORE` | ✅ 已支持 |
| 编辑标记 | `<del>` | `CORE` | ✅ 已支持 |
| 编辑标记 | `<ins>` | `CORE` | ✅ 已支持 |
| 嵌入 | `<area>` | `IMAGE` | ⏳ 计划 |
| 嵌入 | `<embed>` | `EMBED` | ⏳ 计划 |
| 嵌入 | `<iframe>` | `EMBED` | ⏳ 计划 |
| 嵌入 | `<img>` | `IMAGE` | ⏳ 计划 |
| 嵌入 | `<map>` | `IMAGE` | ⏳ 计划 |
| 嵌入 | `<object>` | `EMBED` | ⏳ 计划 |
| 嵌入 | `<picture>` | `IMAGE` | ⏳ 计划 |
| 嵌入 | `<source>` | `IMAGE` | ⏳ 计划 |
| 媒体 | `<audio>` | `EMBED` | ⏳ 计划 |
| 媒体 | `<track>` | `EMBED` | ⏳ 计划 |
| 媒体 | `<video>` | `EMBED` | ⏳ 计划 |
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
| 表单 | `<datalist>` | `WIDGET` | ✅ 已支持 |
| 表单 | `<fieldset>` | `CORE` | ✅ 已支持 |
| 表单 | `<form>` | `CORE` | ✅ 已支持 |
| 表单 | `<input>` | `CORE` | ✅ 已支持 |
| 表单 | `<label>` | `CORE` | ✅ 已支持 |
| 表单 | `<legend>` | `CORE` | ✅ 已支持 |
| 表单 | `<meter>` | `WIDGET` | ✅ 已支持 |
| 表单 | `<optgroup>` | `CORE` | ✅ 已支持 |
| 表单 | `<option>` | `CORE` | ✅ 已支持 |
| 表单 | `<output>` | `WIDGET` | ✅ 已支持 |
| 表单 | `<progress>` | `WIDGET` | ✅ 已支持 |
| 表单 | `<select>` | `CORE` | ✅ 已支持 |
| 表单 | `<textarea>` | `WIDGET` | ⏳ 计划 |
| 交互 | `<details>` | `WIDGET` | ✅ 已支持 |
| 交互 | `<dialog>` | `WIDGET` | ⏳ 计划 |
| 交互 | `<summary>` | `WIDGET` | ✅ 已支持 |
| 脚本 | `<canvas>` | `EMBED` | ⏳ 计划 |
| 脚本 | `<noscript>` | `EMBED` | ⏳ 计划 |
| 脚本 | `<script>` | `EMBED` | ⏳ 计划 |
| Web Components | `<slot>` | `EMBED` | ⏳ 计划 |
| Web Components | `<template>` | `EMBED` | ⏳ 计划 |
| SVG / MathML | `<math>` | `EMBED` | ⏳ 计划 |
| SVG / MathML | `<svg>` | `EMBED` | ⏳ 计划 |

统计：**✅ 78 个**（P0 加 31 个纯语义标签、P1 加 6 个元数据标签、
P2a 加 4 个控件、P2b 加 `details`/`summary`）、**⏳ 36 个**、**❌ 0 个**。

---

## 2. 能力包

一个"包"= 一组标签 + 支撑它们的那部分引擎。包是**编译期开关**，也是**构建期报告的单位**。

| 包 | 宏 | 含标签（个数） | PC/移动默认 | MCU 默认 | 依赖 |
|---|---|---|---|---|---|
| `CORE` | 恒开 | 66 | ✅ | ✅ | 无（永远有） |
| `META` ✅ | `QL_FEAT_UI_META` | 6（`base head link meta style title`） | ✅ | ✅ | 无 —— 解析期整段跳过，不产生盒子 |
| `TEXT` | `QL_FEAT_UI_TEXT` | 6（`pre sub sup ruby rt rp`） | ✅ | ❌ | 文本内 `\n`、`white-space: pre`、等宽字体族、行内注音 |
| `TABLE` | `QL_FEAT_UI_TABLE` | 10（`table` 全家） | ✅ | ❌ | 表格布局算法（列宽分配、跨行跨列、边框合并） |
| `IMAGE` | `QL_FEAT_UI_IMAGE` | 5（`img picture source map area`） | ✅ | ❌ | 有界解码器（PNG/BMP/QOI）+ 缩放 + 热区 |
| `WIDGET` | `QL_FEAT_UI_WIDGET` | 8（`progress meter output datalist textarea details summary dialog`） | ✅ | ❌ | 复用已有池/覆盖层机制 + `
`（`textarea`） |
| `EMBED` | `QL_FEAT_UI_EMBED` | 13（`iframe embed object audio video track canvas script noscript slot template svg math`） | ✅ | ❌ | **只做占位盒 / 忽略**，见 §5 |

`CORE` 里那 66 个就是现在能画的那批（含 `<input>` / `<select>` / 滚动容器）。

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
> 其余包（`TEXT` / `TABLE` / `IMAGE` / `WIDGET` / `EMBED`）随实现追加，
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

## 5. 永远只做占位的 13 个（`EMBED`）

它们不是"还没做"，是**按红线不做真渲染**：

| 标签 | 为什么不做 | 占位怎么做 |
|---|---|---|
| `script` `noscript` `template` `slot` | 与"没有 DOM / 没有 JS"红线冲突 | `script` 的内容**整段跳过**（不解析、不进文本池）；其余按 `display:none` |
| `iframe` `embed` `object` | 外来内容无法有界（网络、第三方渲染器） | 一个带边框的空盒子 + 文字 `未支持` |
| `audio` `video` `track` | 要解码器 + 音频输出 + 音视频同步 | 空盒子（可以画个播放图标字符） |
| `canvas` | 要 JS 或"C 代码往上画"的新 API 层 | 空盒子占位；真的想画 → 直接用 `QLCanvas`，别绕 HTML |
| `svg` `math` | 各自需要独立渲染器（等于写第二个 QLGL） | 空盒子占位 |

**这就是"HTML5 全支持"的准确含义**：114 个标签**全能编译通过**，
其中 66 个现在真画、48 个按包补齐后真画或占位、0 个报"不认识的标签"。

---

## 6. 构建期就该知道结果

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
| P2a ✅ 已完成 | `WIDGET` 一半：`progress` `meter`（value/max → 完成度池）、`output`、`datalist` | 76 | 1 天 |
| P2b ✅ 已完成 | `details` `summary`：收起时把子节点指向共享隐藏样式 | 78 | 0.5 天 |
| P2c | `dialog`：真正的浮层（居中 + 遮罩 + `Esc` 关闭） | 79 | 1–2 天 |
| P3 | `IMAGE` 包：PNG/BMP/QOI 有界解码 + `object-fit` + `map`/`area` 热区 | 84 | 3–4 天 |
| P4 | `TEXT` 包：文本内 `\n` + `white-space: pre` + 等宽字体 + `sub`/`sup` + `ruby` | 90 | 5–6 天 |
| P5 | `WIDGET` 补齐：`textarea`（多行编辑） | 91 | 2–3 天 |
| P6 | `TABLE` 包：表格布局算法 | 101 | 5–8 天（**风险最高**） |
| P7 | `EMBED` 包：13 个占位盒 + `script` 内容跳过 | 114 | 1–2 天 |
| P8 | `<input>` 的其余 `type`（`checkbox` `radio` `range` `number` `search`） | — | 3–5 天 |

总计约 **24–34 人日**。P6 单独标风险：它是唯一一处"要从零写一套布局算法"的地方，
其余都是在已有机制上加分支。

---

## 8. 与文档其它地方的口径

- 现在（P0 之后）：**已支持 66 / 114**，未支持 48。
- 教程的"[与 Web 的差别](10_vs_web.html)"列的是**用户会踩到的差异**，不重复这份清单。
- 各标签的 UA 默认样式见[阅读指南 §5.4.7](QLUI阅读指南.html)；
  属性白名单见 [API 手册 §3.2](QLUI API手册.html)。
