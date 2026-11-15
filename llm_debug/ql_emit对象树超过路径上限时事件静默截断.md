# `ql_emit()` 对象树超过路径上限时传播被静默截断

## 问题

`ql_emit()` 只把目标到根的前 `QL_CFG_EVENT_PATH_MAX` 个节点写入固定路径数组；到达上限后，循环静默结束，没有检查仍有未收集的父节点，也没有拒绝派发或提示。捕获和冒泡阶段因此只遍历这段被截断的路径。

默认 `QL_CFG_EVENT_PATH_MAX` 为 8，而对象树挂接逻辑没有相应的深度上限检查。

## 影响

当对象深度超过配置上限时，事件不会到达更高层的祖先监听器；捕获阶段也会跳过这些祖先，调用方无法从返回值区分“完整传播”与“路径被截断”。

## 依据

- `include/qlgl/qlconfig.h` 将 `QL_CFG_EVENT_PATH_MAX` 默认设为 8。
- `src/core/qlevent.c` 的 `ql_emit()` 遍历条件是 `depth < QL_CFG_EVENT_PATH_MAX`，达到上限后直接继续派发。
- `src/core/qlobject.c` 的 `ql_object_set_parent()` 没有验证树深度不超过该配置。

## 验证状态

静态检查确认对象树可超过事件路径容量；现有事件测试未覆盖超深对象树，本次未做动态复现。
