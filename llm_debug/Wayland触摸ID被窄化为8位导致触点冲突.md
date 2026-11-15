# Wayland 触摸 ID 被窄化为 8 位导致触点冲突

## 问题

Wayland 的触摸 ID 是 `int32_t`，但 `qlwl_touch_push()` 在派发前将其强制转换为 `uint8_t`；负值还被统一改成 0。公开输入事件的 `touch_id` 也只有 8 位，UI 层据此匹配触点。

## 影响

不同的 Wayland 触点 ID 若低 8 位相同（例如 0 与 256），在 UI 层会被视为同一触点；负 ID 也可能与真实的 0 号触点冲突，导致多指手势的移动/抬起匹配到错误触点。

## 依据

- `src/port/qlwindow_wayland.h` 中 `wl_touch` listener 接收 `int32_t id`。
- `qlwl_touch_push()` 将 ID 转为 `uint8_t` 后交给 `ql_input_push_touch()`。
- `include/qlgl/qlinput.h` 的 `QLInputEvent.touch_id` 为 `uint8_t`，`qlui.c` 用该值查找触点槽。

## 验证状态

静态追踪确认 ID 窄化与 UI 按窄化值匹配；未在合成器上构造超过 8 位的触摸 ID 进行动态验证。
