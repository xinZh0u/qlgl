# Wayland 运行期缓冲区重建失败后仍使用空缓冲区

> **核对 2026-11-15**：✅ 已修 —— `src/port/qldisp_wayland.c:157-172` 先建 `fresh[]` 全部成功再换旧，失败保留旧缓冲区

## 问题

窗口尺寸或输出缩放变化时，`ql_disp_wayland_poll()` 调用 `buffers_rebuild()`，但忽略其返回值。`buffers_rebuild()` 会先销毁旧 shm 缓冲区，再分配新的帧缓冲及两个 shm 缓冲区；中途分配失败时可能留下一个或多个 `buffer/data == NULL` 的槽位。

随后 `wayland_flush()` 只依据 `busy` 选择槽位，没有检查 `slot->data` 是否有效，就对其执行 `memcpy()`。

## 影响

运行期 resize/scale 重建遇到内存或 Wayland 资源分配失败时，后续绘制可能向空指针拷贝而崩溃；同时旧缓冲区已先销毁，无法继续显示旧画面。

## 依据

- `src/port/qldisp_wayland.c` 的 `buffers_rebuild()` 先调用 `buffers_release()`，随后可能因帧缓冲分配或 `create_buffer()` 失败返回 0。
- `ql_disp_wayland_poll()` 两处调用 `buffers_rebuild()` 均未处理失败返回值。
- `wayland_flush()` 选择空闲槽位后直接访问 `slot->data`，没有有效性检查。

## 验证状态

静态追踪确认失败路径与后续空指针使用；当前环境没有 Wayland 合成器，未能通过故障注入动态复现。
