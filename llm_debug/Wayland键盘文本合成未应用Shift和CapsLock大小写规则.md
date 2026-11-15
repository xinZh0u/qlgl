# Wayland 键盘文本合成未应用 Shift 和 CapsLock 大小写规则

> **核对 2026-11-15**：✅ 已修 —— `src/port/qlwindow_wayland.h:289-312` 字母用 `Shift ^ CapsLock`，符号按 Shift 映射

## 问题

Wayland 键盘回调把 evdev 键码映射成 US 基础字符后，只在 Shift 按下时将 `a..z` 转成大写。数字与标点不应用 Shift 映射（如 Shift+1 仍产生 `1`），字母也完全忽略 CapsLock 状态。

## 影响

使用原生 Wayland 后端时，常见的 Shift+数字/标点组合与 CapsLock 输入不符合键盘预期，且与 X11/XIM 路径的字符输入行为不一致。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_kb_key()` 先由 `ql_evdev_key()` 获取 US 基础字符。
- 该函数只对 `a..z` 检查 `QL_MOD_SHIFT` 并转换大小写，没有处理 Shift 标点表或 `QL_MOD_CAPS`。
- `src/port/qlinput_evdev.h` 的静态映射仅提供未按 Shift 的数字与标点。

## 验证状态

静态追踪确认修饰键文本转换不完整；当前环境没有 Wayland 合成器，相关集成测试跳过，未能进行键盘事件动态验证。
