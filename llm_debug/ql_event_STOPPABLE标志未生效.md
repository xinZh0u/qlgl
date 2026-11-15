# `QL_EVENT_FLAG_STOPPABLE` 未限制事件传播停止

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlevent.c:280-284` `stop_requested()` 要求登记 STOPPABLE；回归 `test_object_event`

## 问题位置

- `include/qlgl/qlevent.h`：`QL_EVENT_FLAG_STOPPABLE`
- `src/core/qlevent.c`：`ql_event_stop_propagation()`、`dispatch()`、`ql_emit()`

## 问题说明

事件标志将 `QL_EVENT_FLAG_STOPPABLE` 定义为“允许 `stop_propagation` 生效”。但 `ql_event_stop_propagation()` 对任何事件都设置 `event->stop_propagation = 1`，而 `dispatch()` 和 `ql_emit()` 无条件根据该字段停止后续回调/传播，没有检查事件描述中的 `flags`。

因此，对未设置 `QL_EVENT_FLAG_STOPPABLE` 的事件调用 `ql_event_stop_propagation()`，仍会截断捕获、目标或冒泡阶段。当前 `test_object_event` 只在设置了该标志的事件上测试停止行为，未覆盖未设置标志的情况。

## 影响

事件描述中未允许停止传播的事件仍可被任意监听器截断，实际行为与公开标志语义不符。

## 检查状态

已确认 `QL_EVENT_FLAG_STOPPABLE` 在派发实现中没有被读取；没有修改项目代码。
