# Wayland 键盘忽略 repeat_info 导致按住按键不重复

## 问题

Wayland 的 `wl_keyboard` 不会自动发送传统意义上的重复按键事件；客户端需根据 `repeat_info` 给出的速率/延迟自行生成重复。当前 `qlwl_kb_repeat()` 丢弃这两个参数，`qlwl_kb_key()` 也没有启动重复计时器。

## 影响

原生 Wayland 后端按住 Backspace、方向键等可重复按键时只收到一次按下事件，文本删除、光标移动等操作无法连续重复；行为与 X11 后端不一致。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_kb_repeat()` 对 `rate` 与 `delay` 仅执行 `QL_UNUSED`。
- 同文件 `qlwl_kb_key()` 每次收到按下事件只派发一次，没有使用 repeat 信息安排后续按键。

## 验证状态

静态追踪确认重复参数未使用且无客户端重复逻辑；当前环境没有 Wayland 合成器，相关集成测试跳过，未能进行长按测试。
