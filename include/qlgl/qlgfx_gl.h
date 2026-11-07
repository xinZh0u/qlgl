/**
 * @file    qlgfx_gl.h
 * @brief   OpenGL ES 3.0（EGL）GPU 后端。
 *
 * 设计要点：
 *   - 单管线单 draw call：SDF 圆角矩形与字形都走同一个 shader，用 use_tex 分支；
 *   - 顶点先攒在 CPU 侧数组，一帧一次上传，零额外分配；
 *   - 圆角/边框用 SDF 解析计算，不需要纹理图集，天然抗锯齿；
 *   - 字形来自 4bpp 位图字体展开成的 R8 图集；
 *   - 支持"离屏 FBO"模式给测试用（不弹窗），与窗口模式共用同一套绘制代码。
 *
 * 与 CPU 后端的关系：两者都实现 QLDraw2D，输出必须一致（golden image 测试）。
 */
#ifndef QLGL_GFX_GL_H
#define QLGL_GFX_GL_H

#include "qlgl.h"
#include "qldraw2d.h"

#if defined(QLGL_ENABLE_GL)

typedef struct QLGfxGL QLGfxGL;

enum {
    QL_GFX_GL_WINDOW    = 0,   /* 建窗口，可直接看到界面 */
    QL_GFX_GL_OFFSCREEN = 1u << 0,  /* pbuffer 离屏，测试/导出用 */
};

/* 失败返回 NULL（无 EGL / 无 X11 / 无 GLES3 都会失败并打印原因） */
QLGfxGL *ql_gfx_gl_create(const char *title, ql_coord_t width, ql_coord_t height,
                          uint32_t flags);
void     ql_gfx_gl_destroy(QLGfxGL *gfx);

/* 窗口模式：处理事件，返回 1=继续 0=窗口已关闭 */
int      ql_gfx_gl_poll(QLGfxGL *gfx);

/* 每帧：begin → 用 ql_gfx_gl_draw() 得到的接口绘制 → end */
void     ql_gfx_gl_begin(QLGfxGL *gfx, ql_color_t clear_color);
void     ql_gfx_gl_end(QLGfxGL *gfx);

QLDraw2D *ql_gfx_gl_draw(QLGfxGL *gfx);

/* 1 = 等垂直同步（默认），0 = 不限帧（基准测试用） */
void     ql_gfx_gl_set_vsync(QLGfxGL *gfx, int on);

/* 阻塞到 GPU 画完（基准测试用；正常渲染不要调用） */
void     ql_gfx_gl_finish(QLGfxGL *gfx);

/* 统计 */
uint32_t ql_gfx_gl_frame_count(QLGfxGL *gfx);
uint32_t ql_gfx_gl_draw_calls(QLGfxGL *gfx);
uint32_t ql_gfx_gl_vertex_count(QLGfxGL *gfx);
/* 上一帧的 begin→end 耗时（微秒；CPU 侧测量） */
uint32_t ql_gfx_gl_frame_us(QLGfxGL *gfx);
double   ql_gfx_gl_gpu_mhz(QLGfxGL *gfx);   /* 粗略信息，可能为 0 */

/* 读回像素（RGBA8888，自上而下）。离屏模式必可读；窗口模式读的是后缓冲 */
ql_err_t ql_gfx_gl_read_pixels(QLGfxGL *gfx, uint8_t *rgba, uint32_t stride_bytes);

#endif /* QLGL_ENABLE_GL */

#endif /* QLGL_GFX_GL_H */
