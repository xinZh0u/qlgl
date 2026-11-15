# 动画完成回调重启同一 key 时新动画被清除

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlanim.c:178-191` / `:274-281` 先清槽再回调；回归 `test_anim`（done 里重启同 key）

## 问题

`ql_anim_tick()` 完成动画时，调用 `done` 回调之后才把当前槽位清零：

1. 当前槽仍登记着同一个 `key`；
2. `done` 中对同一个 key 调用 `ql_anim_start()` 会找到并重用该槽，成功写入新动画；
3. 回调返回后，旧的 `ql_anim_tick()` 路径继续将 `key->used` 设为 0 并清零该槽。

因此新动画会被旧动画的收尾逻辑立即抹掉。

## 影响

使用完成回调串联同一动画 key 的连续动画时，后续动画不会运行，且 key 的 `used` 状态错误。

## 依据

- `include/qlgl/qlanim.h` 将 `done` 定义为动画结束回调。
- `src/core/qlanim.c` 在完成分支中先调用 `s->done(s->user)`，之后再修改 key 并 `memset(s, 0, sizeof(*s))`。
- `ql_anim_start()` 会按 key 查找并重用正在使用的槽位。

## 验证状态

由回调时序静态确认；现有 `test_anim` 通过，但未覆盖完成回调中重启同一 key。
