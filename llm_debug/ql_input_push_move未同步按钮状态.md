# `ql_input_push_move()` 不会更新查询到的按钮状态

## 问题位置

- `src/core/qlinput.c`：`track_state()`
- `src/core/qlinput.c`：`ql_input_push_move()`

## 问题说明

`ql_input_push_move(x, y, buttons)` 把调用方提供的按钮位图写入事件的 `ev.buttons`，但入队状态更新函数 `track_state()` 对 `QL_INPUT_POINTER_MOVE` 只更新坐标，没有同步 `s_buttons`。

例如 `ql_input_push_move(10, 20, QL_BUTTON_BIT_LEFT)` 后，派发的移动事件报告左键按住，但 `ql_input_buttons()` 仍返回旧值（通常为 0）。这与公开头文件“由 push 系列在入队时更新”以及移动事件本身携带当前按钮位图的语义不一致。

## 影响

依赖 `ql_input_buttons()` 获取当前按钮状态的调用方，在移动事件提供了新的按钮快照时会得到过期状态。当前 UI 输入路由主要读取事件本身，未因此在现有测试中失败。

## 检查状态

由状态更新路径静态确认；现有 `test_input_queue` 未覆盖移动事件携带非零按钮位图的查询结果。没有修改项目代码。
