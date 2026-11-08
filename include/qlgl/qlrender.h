/**
 * @file    qlrender.h
 * @brief   渲染器：维护脏矩形，遍历对象树把控件画到当前显示上。
 *
 * 用法：
 *     qlgl_init();
 *     QLDispDriver *disp = ql_disp_mem_create(320, 240, QL_COLOR_RGB565);
 *     QLWidget *root = ql_widget_new_ex(0, 0, 320, 240);
 *     ql_render_set_root((QLObject *)root);
 *     ql_render();               // 首帧：全屏脏
 *     修改控件属性 -> 自动打脏 -> ql_render();  // 只重绘脏区
 */
#ifndef QLGL_RENDER_H
#define QLGL_RENDER_H

#include "qlgl.h"
#include "qlwidget.h"
#include "qldisp.h"

/* 设置/获取渲染根节点（根可以不是控件）；传 NULL 解绑，销毁树之前必须先解绑 */
ql_err_t   ql_render_set_root(QLObject *root);
QLObject  *ql_render_root(void);

/* 把整屏标记为脏（首帧、显示切换后调用） */
void ql_render_invalidate_all(void);
void ql_render_invalidate_rect(QLRect area);
/* 按控件的绝对矩形打脏 */
void ql_render_invalidate(QLObject *obj);

/* 当前排队的脏矩形数量（测试/调试用） */
uint32_t ql_render_dirty_count(void);
void     ql_render_dirty_rect(uint32_t index, QLRect *out);

/*
 * 渲染所有脏区：
 *   1. 用显示背景色清空每个脏区
 *   2. 按对象树顺序（父先子后）绘制与脏区相交的控件
 *   3. 调用驱动的 flush
 * 返回实际绘制的脏区数量；无显示或未设根时返回 QL_ERR_NOT_INIT。
 */
int ql_render(void);

/*
 * 把控件树画到任意 QLDraw2D（CPU 或 GPU），用于"自己管理窗口/上下文"的场景：
 *
 *     ql_gfx_gl_begin(gfx, bg);                       // 自己的清屏
 *     ql_render_draw2d(ql_gfx_gl_draw(gfx), w, h);     // 只画脏区
 *     ql_gfx_gl_end(gfx);
 *
 * 与 ql_render() 的区别：**不清背景、不推屏、不清空脏区队列**（脏区由 ql_render
 * 或 ql_render_invalidate_* 管理）。s_all_dirty 时会画满 surface_w × surface_h。
 * 返回本次画了几个区域。
 */
int ql_render_draw2d(QLDraw2D *d2d, ql_coord_t surface_w, ql_coord_t surface_h);

#endif /* QLGL_RENDER_H */
