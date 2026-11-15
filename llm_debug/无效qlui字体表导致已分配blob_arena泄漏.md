# 无效 qlui 字体表导致已分配 blob arena 泄漏

> **核对 2026-11-15**：✅ 已修 —— `src/ui/qlui_blob.c:821-828` 校验失败统一 `fail:` 释放 doc + arena

## 问题

`ql_ui_doc_load_mem()` 先分配并复制完整 blob 到 `arena`，之后才校验字体表。若字体表不一致、`@font-face` 文本索引越界、字体列表区间越界或字体名称索引越界，错误分支只释放 `doc`，没有释放 `arena`。

此时 `doc->arena` 尚未赋值（仅在全部校验通过后设置），所以调用 `ql_ui_doc_free()` 也无法补回这块内存。

## 影响

每次加载带有上述无效字体表的 blob 都会泄漏一份与 blob 大小相当的内存；重复加载错误文件可持续耗尽进程内存。

## 依据

- `src/ui/qlui_blob.c` 的 `ql_ui_doc_load_mem()` 在设置 `doc->arena = arena` 前执行字体表检查。
- 字体表相关的四类失败分支均调用 `ql_mem_free(doc)` 后直接返回，没有释放局部变量 `arena`。
- 同一函数后续 `<area>` 校验失败分支已同时释放 `doc` 与 `arena`，显示该资源需要在错误路径显式释放。

## 验证状态

静态追踪确认错误清理遗漏；现有 blob 测试通过，本次未通过故意损坏字体表的 blob 动态验证。
