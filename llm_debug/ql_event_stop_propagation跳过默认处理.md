# 目标监听器停止传播会连带跳过默认处理

> **核对 2026-11-15**：✅ 已修 —— `src/core/qlevent.c:299-315` 监听器循环 break 与默认处理解耦；回归 `test_object_event`

## 问题位置

- `src/core/qlevent.c`：`dispatch()`

## 问题说明

`dispatch()` 在监听器回调后发现 `event->stop_propagation` 已设置时立即返回。若当前对象就是事件目标，这个提前返回会跳过同一函数末尾的类默认事件处理。

`prevent_default` 是独立字段，且默认处理条件专门检查 `!event->prevent_default`；因此目标监听器只调用 `ql_event_stop_propagation()` 也会意外抑制默认行为，混淆了“停止后续传播”和“阻止默认处理”两种语义。

## 影响

目标监听器停止向祖先传播时，目标类的默认处理也不会运行，即使事件带有 `QL_EVENT_FLAG_DEFAULT` 且没有调用 `ql_event_prevent_default()`。

## 检查状态

由控制流静态确认；现有事件测试覆盖了祖先节点停止传播，但没有覆盖目标监听器停止传播时的默认处理。没有修改项目代码。
