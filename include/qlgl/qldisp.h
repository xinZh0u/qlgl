/**
 * @file    qldisp.h
 * @brief   显示驱动接口。
 *
 * v0.1 采用"帧缓冲直绘"：驱动提供一块连续像素内存，渲染器直接写入，
 * 之后调用 flush 通知驱动把某个矩形推到屏幕上。
 * 部分缓冲/双缓冲的实现只需自己管理 framebuffer 指针与 flush 时机。
 */
#ifndef QLGL_DISP_H
#define QLGL_DISP_H

#include "qlgl.h"
#include "qlgeom.h"

typedef enum {
    QL_COLOR_RGB565 = 0,     /* 每像素 2 字节 */
    QL_COLOR_RGB888,         /* 每像素 3 字节 */
    QL_COLOR_ARGB8888,       /* 每像素 4 字节，渲染器写入 alpha=0xFF */
    QL_COLOR_L8,             /* 每像素 1 字节灰度 */
    QL_COLOR_FORMAT_COUNT
} QLColorFormat;

/* 传它给后端表示"由后端自动选择像素格式" */
#define QL_COLOR_FORMAT_AUTO    ((QLColorFormat)0xFF)

typedef struct QLDispDriver QLDispDriver;

struct QLDispDriver {
    ql_coord_t width;
    ql_coord_t height;
    uint8_t    format;         /* QLColorFormat */
    uint8_t    rotation;       /* 预留：0/90/180/270 */
    uint16_t   reserved;
    void      *framebuffer;    /* 像素数据起点（左上角第 0 字节） */
    uint32_t   stride;         /* 字节/行 */
    /*
     * 这块面板相对 CSS px 的缩放（26.6 定点，QL_SCALE_ONE = 1.0）。
     * width/height 永远是**设备像素**；布局要的是 CSS px，用
     * ql_disp_css_width() 取。高 DPI 后端（X11 的 Xft.dpi / Wayland 的
     * wl_output.scale）会自己填，也可以由应用用 ql_disp_set_scale() 指定。
     */
    ql_coord_t scale_fx;
    /* 通知驱动某个矩形已更新；返回 0 表示成功。一帧内可能被调用多次 */
    int      (*flush)(QLDispDriver *self, QLRect area);
    /*
     * 可选：一帧所有脏区都推送完毕后调用一次。
     * 用于把"多次小提交"合并成"一次大提交"（Wayland 必须 attach+commit，
     * X11 可以只 XFlush 一次）。为 NULL 时忽略。
     */
    int      (*flush_done)(QLDispDriver *self);
    void      *user_data;
};

/* 内置帧缓冲驱动所需的字节数（含对齐） */
uint32_t ql_disp_buffer_size(QLColorFormat format, ql_coord_t width, ql_coord_t height);

/* 每像素字节数；未知格式返回 0 */
uint32_t ql_disp_format_bytes(uint8_t format);
const char *ql_disp_format_name(uint8_t format);

/* 改缩放（26.6 定点）；会自动同步到已绑定它的 QLCanvas */
void ql_disp_set_scale(QLDispDriver *driver, ql_coord_t scale_fx);

/* CSS px 下的屏幕尺寸（布局用这个，不要用 width/height） */
static inline ql_coord_t ql_disp_css_width(const QLDispDriver *d)
{
    ql_coord_t fx = (d->scale_fx > 0) ? d->scale_fx : QL_SCALE_ONE;
    return ql_unscale_len_ceil(d->width, fx);
}

static inline ql_coord_t ql_disp_css_height(const QLDispDriver *d)
{
    ql_coord_t fx = (d->scale_fx > 0) ? d->scale_fx : QL_SCALE_ONE;
    return ql_unscale_len_ceil(d->height, fx);
}

/* 设置/获取当前活动显示（同一时刻只有一个） */
ql_err_t       ql_disp_set(QLDispDriver *driver);
QLDispDriver  *ql_disp_get(void);
/* 仅当 driver 是当前活动显示时注销它；driver 为 NULL 时直接注销 */
void           ql_disp_unset(QLDispDriver *driver);
void           ql_disp_set_bg_color(ql_color_t color);
ql_color_t     ql_disp_bg_color(void);

/* 把一块像素内存包装成驱动（不做任何分配） */
ql_err_t ql_disp_init_fb(QLDispDriver *driver, void *framebuffer, uint32_t stride,
                         ql_coord_t width, ql_coord_t height, QLColorFormat format,
                         int (*flush)(QLDispDriver *self, QLRect area));

/* ----- 内存显示驱动（PC 调试 / 测试 / 示例用） ----- */
QLDispDriver *ql_disp_mem_create(ql_coord_t width, ql_coord_t height, QLColorFormat format);
void          ql_disp_mem_destroy(QLDispDriver *driver);
void          ql_disp_mem_clear(QLDispDriver *driver, ql_color_t color);
ql_color_t    ql_disp_mem_pixel(QLDispDriver *driver, ql_coord_t x, ql_coord_t y);
/* 输出 P6 二进制 PPM，可直接用图片查看器打开 */
ql_err_t      ql_disp_mem_save_ppm(QLDispDriver *driver, const char *path);
/* 累计的 flush 次数与最后一次 flush 的面积，测试用 */
uint32_t      ql_disp_mem_flush_count(QLDispDriver *driver);

#endif /* QLGL_DISP_H */
