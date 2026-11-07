/**
 * @file    qldisp_x11.h
 * @brief   Linux X11 窗口显示后端。
 *
 * 渲染器直接写进 XImage 的像素存储，flush 时用 XPutImage 推脏矩形到窗口。
 * 使用方式：
 *
 *     QLDispDriver *d = ql_disp_x11_create("QLGL Demo", 320, 240,
 *                                          QL_COLOR_FORMAT_AUTO);
 *     if (d == NULL) { 无 X 环境，降级到内存驱动 }
 *     ql_render_set_root(root);
 *     while (ql_disp_x11_poll(d)) {
 *         ... 修改控件 ...
 *         ql_render();
 *     }
 *     ql_disp_x11_destroy(d);
 *
 * 编译期由 CMake 定义 QLGL_ENABLE_X11；没有 X11 时本节内容不参与编译。
 */
#ifndef QLGL_DISP_X11_H
#define QLGL_DISP_X11_H

#include "qlgl.h"
#include "qldisp.h"

#if defined(QLGL_ENABLE_X11)

/*
 * 创建窗口。title 可为 NULL（默认 "QLGL"）。
 * 返回的驱动已经通过 ql_disp_set 设为活动显示。
 * 失败返回 NULL 并打错误日志（最常见原因是没有 DISPLAY）。
 */
QLDispDriver *ql_disp_x11_create(const char *title, ql_coord_t width, ql_coord_t height,
                                 QLColorFormat format);

/*
 * 处理挂起的窗口事件并返回是否继续：
 *   1  窗口仍在，可以继续渲染
 *   0  窗口被关闭（点了关闭按钮 / 按 Esc / DestroyNotify）
 * Expose 事件会自动把整屏标记为脏。
 */
int ql_disp_x11_poll(QLDispDriver *driver);

void ql_disp_x11_destroy(QLDispDriver *driver);

/* 窗口名（调试用），未创建返回 "" */
const char *ql_disp_x11_title(QLDispDriver *driver);

/* 已经处理过的 Expose 次数，测试/调试用 */
uint32_t ql_disp_x11_expose_count(QLDispDriver *driver);

#endif /* QLGL_ENABLE_X11 */

#endif /* QLGL_DISP_X11_H */
