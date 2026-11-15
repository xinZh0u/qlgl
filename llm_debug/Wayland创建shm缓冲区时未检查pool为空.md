# Wayland 创建 shm 缓冲区时未检查 pool 为空

> **核对 2026-11-15**：✅ 已修 —— `src/port/qldisp_wayland.c:99-105` pool 为空时 munmap/close 后失败

## 问题

`create_buffer()` 调用 `wl_shm_create_pool()` 后，没有检查返回值是否为 `NULL`，便将其传给 `wl_shm_pool_create_buffer()`。Wayland 客户端代理对象分配失败时，pool 创建可能返回空指针，后续调用会解引用无效代理。

## 影响

Wayland 客户端内存/代理分配失败时，窗口初始化或运行期缓冲区重建可能直接崩溃，而不是按既有错误路径返回失败。

## 依据

- `src/port/qldisp_wayland.c` 的 `create_buffer()` 在 `wl_shm_create_pool()` 与 `wl_shm_pool_create_buffer()` 之间没有空值检查。
- 返回的 `slot->buffer` 有空值检查，但 pool 本身没有。

## 验证状态

静态追踪确认空 pool 会被继续使用；未通过 Wayland 客户端分配故障注入进行动态复现。
