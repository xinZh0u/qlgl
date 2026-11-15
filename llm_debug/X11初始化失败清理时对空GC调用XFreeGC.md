# X11 初始化失败清理时对空 GC 调用 XFreeGC

> **核对 2026-11-15**：✅ 已修 —— `src/port/qldisp_x11.c:355` / `:483` 判空后才 `XFreeGC`

## 问题

`ql_disp_x11_create()` 的 `fail` 清理路径无条件调用 `XFreeGC(dpy, x->gc)`。对象通过 `calloc` 初始化，若在创建 GC 之前的任一步骤失败（例如 `XCreateImage`、帧缓冲分配或窗口创建失败），`x->gc` 仍为 `NULL`，清理会向 X 服务器发送无效的 GC 释放请求。

## 影响

资源分配失败本应返回 `NULL`，但清理阶段会额外触发 `BadGC` X 错误；在默认 X 错误处理器下可能输出错误，或终止进程，掩盖原始失败原因。

## 依据

- `src/port/qldisp_x11.c` 的 `x->gc` 只在窗口创建后通过 `XCreateGC()` 赋值。
- `fail` 标签处仅对 `XDestroyIC`、`XCloseIM`、`XDestroyWindow` 等做了空值判断，对 `XFreeGC(dpy, x->gc)` 未做判断。

## 验证状态

静态追踪确认 `x->gc == NULL` 的失败路径会抵达无条件释放；未在 X11 服务器上注入创建失败进行动态复现。
