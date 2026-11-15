# qlui blob 未校验文本和图像资源引用范围

> **核对 2026-11-15**：✅ 本次补修 —— 已校验的节点/文本/图像区间之外，补上**样式资源名下标**（`bg_image`/`border_image`/`mask_image`）与**图像条目 name 不能是 0xFFFF**（`src/ui/qlui_blob.c:796-820`）；回归 `test_ui_blob`（样式越界）与 `test_image`（资源名哨兵）

## 问题

`ql_ui_doc_load_mem()` 校验 blob 段布局与若干表，但没有验证节点、样式和图像条目中的文本池索引，也没有验证 `text_offsets[]` 指向文本池内部且字符串以 NUL 结尾。它也没有验证图像条目的 `off + len` 落在 `image_data_size` 范围内。

加载后，`build_internal()` 会直接用节点文本索引读取 `doc->text_offsets[src->text]`；背景/边框/遮罩图及图像资源路径也直接索引文本表。解码图像时还会把 `doc->image_data + src->off` 与 `src->len` 传给图像解码器。

## 影响

结构布局与校验和正确、但内部引用字段损坏的 `.qlui` 文件，仍可被加载并在构建/渲染时触发越界读取，或向图像解码器提供超出 blob 数据段的范围。

## 依据

- `src/ui/qlui_blob.c` 的 `ql_ui_doc_load_mem()` 仅对字体表及 `<area>` 引用做了部分范围检查，未遍历校验节点文本、样式资源文本索引、文本偏移/NUL 终止符和图像数据范围。
- `src/ui/qlui.c` 的 `build_internal()`、图像资源绑定路径直接使用这些索引和偏移。

## 验证状态

静态追踪确认 loader 接受这些未验证字段并在构建时解引用；现有 blob 测试通过，本次未构造损坏内部引用的 blob 动态验证。
