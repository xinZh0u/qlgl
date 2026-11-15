# Wayland 绑定版本未受服务端 advertised version 约束

> **核对 2026-11-15**：✅ 已修 —— `src/port/qlwindow_wayland.h:1074` `QLWL_BIND_VER(want)` 取 `min(advertised, wanted)`，所有 bind 都走它

## 问题

`qlwl_registry_global()` 忽略 registry 通知中的 `version`，对 `wl_compositor`、`wl_seat`、`wl_output` 分别固定绑定 v4、v5、v2。若合成器只公布较低版本，客户端仍请求更高版本，违反 `wl_registry.bind` 的版本上限要求，可能导致协议错误并断开连接。

## 影响

运行在只提供较旧 Wayland global 版本的合成器上时，窗口初始化可能失败或连接被 compositor 终止，而不是按可用版本降级运行。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_registry_global()` 将 `version` 标为未使用。
- 同函数对多个 global 使用固定版本号调用 `wl_registry_bind()`，没有与服务端公布版本取最小值。

## 验证状态

静态追踪确认绑定版本没有依据服务端版本协商；当前环境没有 Wayland 合成器，相关集成测试跳过，未能在低版本服务端动态验证。
