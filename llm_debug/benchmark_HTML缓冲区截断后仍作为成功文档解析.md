# benchmark HTML 缓冲区截断后仍作为成功文档解析

> **核对 2026-11-15**：✅ 已修 —— `benchmark/bench_scenes.h:59/67/72` 截断/容量不足都置 `h->ok = 0`

## 问题

`BenchHtml` 的 `bh_add()` 在剩余容量不足时直接返回，不会把 `h->ok` 标为失败；而 `vsnprintf()` 返回值大于可用容量时也未检查，仍将完整所需长度加到 `h->n`。因此 HTML 可能被截断，但 `bh_finish()` 仍把它当作有效输入交给解析器。

## 影响

当维度规模超出预留容量时，benchmark 可能静默编译并运行不完整场景，测量结果低于命令行请求规模，或因截断位置不同而产生不一致的结果。

## 依据

- `benchmark/bench_scenes.h` 的 `bh_add()` 在 `h->n + 1 >= h->cap` 时直接返回。
- `vsnprintf()` 返回值 `w` 未与剩余容量比较，截断时 `h->n += (size_t)w` 仍会前进。
- `bh_finish()` 仅以 `h->ok` 决定是否调用 `ql_ui_load_text()`，容量不足不会令其失败。

## 验证状态

静态追踪确认截断路径未传播失败状态；未以超大 `--scale` 动态复现。
