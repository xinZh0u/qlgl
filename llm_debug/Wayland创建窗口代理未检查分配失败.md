# Wayland 创建窗口代理未检查分配失败

## 问题

`qlwl_open()` 通过 Wayland 协议创建多个代理对象后，未检查其是否为 `NULL`，便立即注册 listener 或继续调用后续代理方法。例如 `wl_compositor_create_surface()` 的结果直接传给 `wl_surface_add_listener()`，`xdg_wm_base_get_xdg_surface()` 和 `xdg_surface_get_toplevel()` 也采用相同模式。

## 影响

客户端代理分配失败时，窗口创建不会沿现有失败返回路径清理，而可能在注册 listener 或调用协议函数时解引用空代理，导致进程崩溃。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_open()` 对 `wl_display_get_registry()` 结果也未检查便注册 registry listener。
- `wl_compositor_create_surface()`、`xdg_wm_base_get_xdg_surface()`、`xdg_surface_get_toplevel()` 的结果均在使用前未判空。
- 相比之下，同一模块对 `wl_egl_window_create()` 与部分可选代理有显式空值处理。

## 验证状态

静态追踪确认代理创建失败时会进入未判空的调用链；当前环境没有 Wayland 合成器，未能进行分配故障注入。
