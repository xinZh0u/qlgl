# 事件派发期间 `ql_disconnect_all()` 立即回收槽位

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlevent.c:159-174` 派发中只标记、`conn_gc()` 延后；回归 `test_object_event`（重连不串轮）

## 问题

`dispatch()` 在调用监听器前缓存当前连接的 `next` 索引，以容忍回调修改连接链；但 `ql_disconnect_all()` 不检查 `s_emit_depth`，会在派发回调中立即将整条连接链的槽位归还空闲池。

回调若随后重新连接监听器，空闲池可能立刻复用派发循环已缓存的 `next` 槽位。循环继续时会把新连接当成原链上的后继连接处理，导致它意外参与当前事件派发；重新连接/断开还可能造成重复派发或迭代失控。

## 影响

在事件回调中调用 `ql_disconnect_all()` 并新增监听器时，当前事件可能错误调用新监听器，事件回调修改连接集合的行为不可靠。

## 依据

- `src/core/qlevent.c` 的 `dispatch()` 在执行 `conn->cb(event)` 前保存 `next = conn->next`。
- `ql_disconnect_all()` 无视 `s_emit_depth`，立即对每个槽调用 `pool_push()`。
- `ql_connect()` 可从刚释放的 free list 立即复用槽位。
- 相比之下，单连接的 `ql_disconnect()` 会在派发期间只标记删除，并延后 GC。

## 验证状态

静态追踪确认槽位复用与缓存索引冲突；现有事件测试未覆盖派发中“断开全部后重新连接”的场景，本次未做动态复现。
