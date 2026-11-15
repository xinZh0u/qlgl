# Wayland 剪贴板 source 取消后保留悬空代理指针

> **核对 2026-11-15**：✅ 已修 —— `src/port/qlwindow_wayland.h:848-849` cancelled 回调里清 `clip_source`

## 问题

Wayland 剪贴板的 `qlwl_clip_cancelled()` 收到 compositor 的 `cancelled` 事件后销毁 `wl_data_source`，但没有把 `w->clip_source` 清空。该指针之后可能在 `qlwl_clip_set()` 中再次销毁，或在 `qlwl_close()` 释放窗口时再次销毁。

## 影响

剪贴板所有权被其他客户端取代后，后续设置剪贴板内容或关闭窗口可能对已销毁的 Wayland proxy 再次调用销毁接口，触发 libwayland 协议/对象生命周期错误。

## 依据

- `src/port/qlwindow_wayland.h` 的 `qlwl_clip_cancelled()` 只调用 `wl_data_source_destroy(src)`。
- 同文件 `qlwl_clip_set()` 与 `qlwl_close()` 都会在 `w->clip_source != NULL` 时再次销毁该对象。

## 验证状态

静态追踪确认取消回调后保留了非空悬空代理指针；当前环境没有 Wayland 合成器，相关集成测试跳过，未能动态复现。
