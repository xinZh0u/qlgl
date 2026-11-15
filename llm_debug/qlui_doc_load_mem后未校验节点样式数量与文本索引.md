# qlui_doc_load_mem 后未校验节点样式数量与文本索引

## 问题

`ql_ui_doc_load_mem()` 没有要求有节点的文档至少包含一个样式，也没有验证 `QLUINode.style` / `QLUINode.text` 是否落在对应表内。虽然运行时初始化对部分非零样式索引做了回退处理，但零样式表会令 `doc->styles == NULL`，之后仍可能读取 `doc->styles[0]`；非法文本索引则在 `build_internal()` 中直接访问 `doc->text_offsets[src->text]`。

## 影响

结构段和校验和正确、但计数/索引不一致的 blob 可以通过加载；后续构建可能触发空指针解引用或越界读取。

## 依据

- `src/ui/qlui_blob.c` 的加载校验未拒绝 `node_count > 0 && style_count == 0`，也未检查节点的 style/text 索引。
- `src/ui/qlui.c` 的 `build_internal()` 未检查 styles 指针；节点文本初始化直接用 `src->text` 索引 `doc->text_offsets`。

## 验证状态

静态追踪确认无效计数/索引可从 blob 加载路径抵达构建解引用；本次未构造非法索引 blob 动态验证。
