# Wayland 未知触摸 ID 的 move 和 up 会分配空槽并派发伪事件

## 问题

`qlwl_touch_slot()` 同时承担“查找已激活 ID”和“为新 down 分配空槽”两种职责：找不到 ID 时会返回第一个空槽。`qlwl_touch_motion()` 与 `qlwl_touch_up()` 也调用它，因此对未跟踪的 ID 不会返回忽略状态。

例如超出 `QLWL_MAX_TOUCH` 而被忽略的手指，在已有触点释放出槽位后继续收到 motion/up，就可能被误当作已跟踪触点；up 还会读取该槽保留的旧坐标并派发事件。

## 影响

被忽略或未知 ID 的触摸事件可能变成没有对应 down 的伪 motion/up，干扰界面层的拖动、点击或多指手势状态。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_touch_slot()` 在找不到活动 ID 时返回空槽。
- `qlwl_touch_down()` 需要分配空槽；`qlwl_touch_motion()` 和 `qlwl_touch_up()` 却复用同一函数作为查找。
- 后两者未验证返回槽的 `active` 和 `id` 是否与事件 ID 匹配。

## 验证状态

静态追踪确认超出跟踪容量的触点可在槽位释放后被误识别；当前环境没有 Wayland 合成器，相关集成测试跳过，未能进行多指时序复现。
