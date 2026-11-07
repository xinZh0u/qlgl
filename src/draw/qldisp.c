/**
 * @file    qldisp.c
 * @brief   显示驱动注册与内存帧缓冲实现。
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

static QLDispDriver *s_active;
static ql_color_t s_bg_color = QL_COLOR_RGB(0x20, 0x20, 0x20);

uint32_t ql_disp_format_bytes(uint8_t format)
{
    switch (format) {
    case QL_COLOR_RGB565:   return 2;
    case QL_COLOR_RGB888:   return 3;
    case QL_COLOR_ARGB8888: return 4;
    case QL_COLOR_L8:       return 1;
    default:                return 0;
    }
}

const char *ql_disp_format_name(uint8_t format)
{
    static const char *const names[QL_COLOR_FORMAT_COUNT] = {
        "RGB565", "RGB888", "ARGB8888", "L8",
    };
    return (format < QL_COLOR_FORMAT_COUNT) ? names[format] : "invalid";
}

uint32_t ql_disp_buffer_size(QLColorFormat format, ql_coord_t width, ql_coord_t height)
{
    uint32_t bytes = ql_disp_format_bytes((uint8_t)format);
    if (bytes == 0 || width <= 0 || height <= 0)
        return 0;
    return (uint32_t)width * bytes * (uint32_t)height;
}

ql_err_t ql_disp_init_fb(QLDispDriver *driver, void *framebuffer, uint32_t stride,
                         ql_coord_t width, ql_coord_t height, QLColorFormat format,
                         int (*flush)(QLDispDriver *self, QLRect area))
{
    if (driver == NULL || framebuffer == NULL || width <= 0 || height <= 0)
        return QL_ERR_INVAL;

    uint32_t bytes = ql_disp_format_bytes((uint8_t)format);
    if (bytes == 0)
        return QL_ERR_INVAL;

    uint32_t min_stride = (uint32_t)width * bytes;
    if (stride == 0)
        stride = min_stride;
    if (stride < min_stride)
        return QL_ERR_INVAL;

    memset(driver, 0, sizeof(*driver));
    driver->width = width;
    driver->height = height;
    driver->format = (uint8_t)format;
    driver->framebuffer = framebuffer;
    driver->stride = stride;
    driver->flush = flush;
    return QL_OK;
}

ql_err_t ql_disp_set(QLDispDriver *driver)
{
    if (driver == NULL)
        return QL_ERR_INVAL;
    if (driver->framebuffer == NULL || driver->width <= 0 || driver->height <= 0)
        return QL_ERR_INVAL;
    if (ql_disp_format_bytes(driver->format) == 0)
        return QL_ERR_INVAL;

    s_active = driver;
    QL_LOGI("display set: %dx%d %s", (int)driver->width, (int)driver->height,
            ql_disp_format_name(driver->format));
    return QL_OK;
}

QLDispDriver *ql_disp_get(void)
{
    return s_active;
}

void ql_disp_unset(QLDispDriver *driver)
{
    if (driver == NULL || s_active == driver)
        s_active = NULL;
}

void ql_disp_set_bg_color(ql_color_t color)
{
    s_bg_color = color;
}

ql_color_t ql_disp_bg_color(void)
{
    return s_bg_color;
}

/* ------------------------------ 内存驱动 ------------------------------ */

typedef struct {
    QLDispDriver base;
    uint8_t      *pixels;
    uint32_t      flush_count;
} QLDispMem;

static int mem_flush(QLDispDriver *self, QLRect area)
{
    QLDispMem *mem = (QLDispMem *)self;
    QL_UNUSED(area);
    mem->flush_count++;
    return 0;
}

QLDispDriver *ql_disp_mem_create(ql_coord_t width, ql_coord_t height, QLColorFormat format)
{
    if (width <= 0 || height <= 0)
        return NULL;

    uint32_t bytes = ql_disp_format_bytes((uint8_t)format);
    if (bytes == 0)
        return NULL;

    QLDispMem *mem = (QLDispMem *)ql_mem_calloc(1, sizeof(QLDispMem));
    if (mem == NULL)
        return NULL;

    uint32_t stride = (uint32_t)width * bytes * 4u;   /* 故意带冗余，顺带验证 stride 支持 */
    uint32_t size = stride * (uint32_t)height;
    mem->pixels = (uint8_t *)ql_mem_calloc(1, size);
    if (mem->pixels == NULL) {
        ql_mem_free(mem);
        return NULL;
    }

    if (ql_disp_init_fb(&mem->base, mem->pixels, stride, width, height, format,
                        mem_flush) != QL_OK) {
        ql_mem_free(mem->pixels);
        ql_mem_free(mem);
        return NULL;
    }

    if (ql_disp_set(&mem->base) != QL_OK) {
        ql_mem_free(mem->pixels);
        ql_mem_free(mem);
        return NULL;
    }

    return &mem->base;
}

void ql_disp_mem_destroy(QLDispDriver *driver)
{
    if (driver == NULL)
        return;

    if (s_active == driver)
        s_active = NULL;

    QLDispMem *mem = (QLDispMem *)driver;
    ql_mem_free(mem->pixels);
    ql_mem_free(mem);
}

void ql_disp_mem_clear(QLDispDriver *driver, ql_color_t color)
{
    if (driver == NULL)
        return;

    uint8_t r = QL_COLOR_GET_R(color);
    uint8_t g = QL_COLOR_GET_G(color);
    uint8_t b = QL_COLOR_GET_B(color);
    uint8_t *base = (uint8_t *)driver->framebuffer;

    for (ql_coord_t y = 0; y < driver->height; y++) {
        uint8_t *row = base + (uint32_t)y * driver->stride;
        for (ql_coord_t x = 0; x < driver->width; x++) {
            switch (driver->format) {
            case QL_COLOR_RGB565: {
                uint16_t px = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
                memcpy(row + (uint32_t)x * 2u, &px, 2);
                break;
            }
            case QL_COLOR_RGB888:
                row[x * 3 + 0] = r;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = b;
                break;
            case QL_COLOR_ARGB8888: {
                uint32_t px = 0xFF000000u | color;
                memcpy(row + (uint32_t)x * 4u, &px, 4);
                break;
            }
            default:
                row[x] = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                                    (uint32_t)b * 29u) >> 8);
                break;
            }
        }
    }
}

ql_color_t ql_disp_mem_pixel(QLDispDriver *driver, ql_coord_t x, ql_coord_t y)
{
    if (driver == NULL || x < 0 || y < 0 || x >= driver->width || y >= driver->height)
        return 0;

    const uint8_t *row = (const uint8_t *)driver->framebuffer + (uint32_t)y * driver->stride;

    switch (driver->format) {
    case QL_COLOR_RGB565: {
        uint16_t px;
        memcpy(&px, row + (uint32_t)x * 2u, 2);
        uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
        uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
        return QL_COLOR_RGB(r, g, b);
    }
    case QL_COLOR_RGB888:
        return QL_COLOR_RGB(row[x * 3 + 0], row[x * 3 + 1], row[x * 3 + 2]);
    case QL_COLOR_ARGB8888: {
        uint32_t px;
        memcpy(&px, row + (uint32_t)x * 4u, 4);
        return px & 0x00FFFFFFu;
    }
    default: {
        uint8_t v = row[x];
        return QL_COLOR_RGB(v, v, v);
    }
    }
}

uint32_t ql_disp_mem_flush_count(QLDispDriver *driver)
{
    if (driver == NULL)
        return 0;
    return ((QLDispMem *)driver)->flush_count;
}

ql_err_t ql_disp_mem_save_ppm(QLDispDriver *driver, const char *path)
{
    if (driver == NULL || path == NULL)
        return QL_ERR_INVAL;

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        QL_LOGE("ql_disp_mem_save_ppm: cannot open %s", path);
        return QL_ERR_INVAL;
    }

    fprintf(fp, "P6\n%d %d\n255\n", (int)driver->width, (int)driver->height);
    for (ql_coord_t y = 0; y < driver->height; y++) {
        for (ql_coord_t x = 0; x < driver->width; x++) {
            ql_color_t c = ql_disp_mem_pixel(driver, x, y);
            fputc((int)QL_COLOR_GET_R(c), fp);
            fputc((int)QL_COLOR_GET_G(c), fp);
            fputc((int)QL_COLOR_GET_B(c), fp);
        }
    }

    fclose(fp);
    return QL_OK;
}
