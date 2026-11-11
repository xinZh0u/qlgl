# HTML5 标签进度规划

排期看这里；逐条状态见 [标签覆盖清单](HTML5标签覆盖与裁剪.html)，落地记录见 [标签开发日志](HTML5标签开发日志.html)。章节编号沿用原文档。

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
