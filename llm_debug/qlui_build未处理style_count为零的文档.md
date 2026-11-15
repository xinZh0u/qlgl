# qlui_build 未处理 style_count 为零的文档

## 问题

`build_internal()` 只检查文档节点数组，没有要求 `doc->styles` 非空或 `style_count > 0`。当 `style_count == 0` 时，它仍把可用样式数设为 `node_count`，随后在 `src->style` 越界分支取 `&doc->styles[0]`。若文档来自 `ql_ui_doc_load_mem()`，零样式数会被接受且 `doc->styles` 被设为 `NULL`，因此构建会解引用空指针。

## 影响

带节点但没有样式表的 `.qlui` blob 能通过加载阶段，随后传给 `ql_ui_build()` 时触发崩溃。

## 依据

- `src/ui/qlui_blob.c` 的 `ql_ui_doc_load_mem()` 未拒绝 `node_count > 0 && style_count == 0`。
- 同函数在 `style_count == 0` 时把 `doc->styles` 设为 `NULL`。
- `src/ui/qlui.c` 的 `build_internal()` 不检查 styles 指针，并在构造运行期节点时访问 `&doc->styles[0]`。

## 验证状态

静态追踪确认零样式 blob 可到达空指针解引用路径；现有 blob 测试通过，本次未构造零样式 blob 动态验证。
