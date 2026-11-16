/**
 * @file    qlimage_svg_thorvg.cpp
 * @brief   SVG 的 ThorVG 后端（可选，C++；通过 C API 调用）。
 *
 * 与 PlutoSVG 后端行为对齐：
 *   - 输出**非预乘 RGBA8888**；
 *   - `currentColor` 在加载前替换成具体颜色（ThorVG 的 SVG loader 只认
 *     `<color>`/默认黑色，没有"外部 currentColor"入口）；
 *   - 只做内存数据解码，不读文件/网络（外部引用在公共预检查里已经拒绝）。
 *
 * 线程模型：ThorVG 引擎是全局的，这里用一把互斥锁把"初始化 + 一次渲染"
 * 串起来；SVG 解码本来也不是热路径。
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* qlgl 的公开头文件里有 C11 的 `_Static_assert`；C++ 里换成 static_assert */
#define _Static_assert(cond, msg) static_assert(cond, msg)
#include <qlgl/qlgl.h>
#undef _Static_assert

#include <thorvg_capi.h>

namespace {

void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_engine_ready = false;

/* 调用方必须已经持有 g_mutex */
bool ensure_engine(char *err, size_t cap)
{
    if (g_engine_ready)
        return true;
    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        set_err(err, cap, "ThorVG 引擎初始化失败");
        return false;
    }
    g_engine_ready = true;
    return true;
}


/* 大小写不敏感地判断 p 处是否是 "currentColor"（12 字节） */
bool is_current_color(const uint8_t *p)
{
    static const char k[] = "currentcolor";
    for (int i = 0; i < 12; i++) {
        unsigned char c = p[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)k[i])
            return false;
    }
    return true;
}

/* 把源里的 currentColor 换成 #RRGGBB；没有则返回 nullptr */
char *replace_current_color(const uint8_t *data, size_t len, uint32_t rgb,
                            size_t *out_len)
{
    bool found = false;
    for (size_t i = 0; i + 12 <= len; i++) {
        if (is_current_color(data + i)) {
            found = true;
            break;
        }
    }
    if (!found)
        return nullptr;

    char hex[8];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned)(rgb & 0xFFFFFFu));

    char *out = (char *)malloc(len + 1u);
    if (out == NULL)
        return nullptr;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (i + 12 <= len && is_current_color(data + i)) {
            memcpy(out + o, hex, 7u);
            o += 7u;
            i += 12u;
        } else {
            out[o++] = (char)data[i++];
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

} // namespace

extern "C" QLErr ql_svg_render_thorvg(const uint8_t *data, size_t len, int w, int h,
                                      uint32_t current_color, uint8_t *rgba,
                                      char *err, size_t err_cap)
{
    if (data == NULL || len == 0 || rgba == NULL || w <= 0 || h <= 0)
        return QL_ERR_INVAL;
    if (len > 0xFFFFFFFFu)
        return QL_ERR_INVAL;

    char *replaced = replace_current_color(data, len, current_color, &len);
    const char *svg = (replaced != NULL) ? replaced : (const char *)data;

    pthread_mutex_lock(&g_mutex);

    QLErr rc = QL_ERR_INVAL;
    if (!ensure_engine(err, err_cap)) {
        pthread_mutex_unlock(&g_mutex);
        free(replaced);
        return QL_ERR_INVAL;
    }

    Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_NONE);
    if (canvas == NULL) {
        set_err(err, err_cap, "ThorVG 画布创建失败");
        goto done;
    }

    if (tvg_swcanvas_set_target(canvas, reinterpret_cast<uint32_t *>(rgba),
                                (uint32_t)w, (uint32_t)w, (uint32_t)h,
                                TVG_COLORSPACE_ARGB8888S) != TVG_RESULT_SUCCESS) {
        set_err(err, err_cap, "ThorVG 设置输出缓冲失败");
        tvg_canvas_destroy(canvas);
        goto done;
    }

    {
        Tvg_Paint pic = tvg_picture_new();
        if (pic == NULL) {
            set_err(err, err_cap, "ThorVG 图片对象创建失败");
            tvg_canvas_destroy(canvas);
            goto done;
        }

        if (tvg_picture_load_data(pic, svg, (uint32_t)len, "svg", NULL, true) !=
            TVG_RESULT_SUCCESS) {
            set_err(err, err_cap, "ThorVG 解析 SVG 失败");
            tvg_paint_unref(pic, true);
            tvg_canvas_destroy(canvas);
            goto done;
        }
        if (tvg_picture_set_size(pic, (float)w, (float)h) != TVG_RESULT_SUCCESS) {
            set_err(err, err_cap, "ThorVG 设置图片尺寸失败");
            tvg_paint_unref(pic, true);
            tvg_canvas_destroy(canvas);
            goto done;
        }
        if (tvg_canvas_add(canvas, pic) != TVG_RESULT_SUCCESS) {
            set_err(err, err_cap, "ThorVG 把图片加入画布失败");
            tvg_paint_unref(pic, true);
            tvg_canvas_destroy(canvas);
            goto done;
        }
        if (tvg_canvas_draw(canvas, true) != TVG_RESULT_SUCCESS ||
            tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS) {
            set_err(err, err_cap, "ThorVG 渲染失败");
            tvg_canvas_remove(canvas, pic);   /* remove 会释放 picture */
            tvg_canvas_destroy(canvas);
            goto done;
        }

        /*
         * ThorVG 的 ARGB8888S 是**未预乘** 0xAARRGGBB；QLImage 要内存序
         * R,G,B,A（小端 uint32 = 0xAABBGGRR）。做一次通道交换。
         */
        uint32_t *p = reinterpret_cast<uint32_t *>(rgba);
        const size_t npx = (size_t)w * (size_t)h;
        for (size_t i = 0; i < npx; i++) {
            const uint32_t v = p[i];
            p[i] = ((v >> 16) & 0xFFu) | (v & 0xFF00u) |
                   ((v & 0xFFu) << 16) | (v & 0xFF000000u);
        }

        /* remove 会 unref 并释放 picture；之后才能销毁画布 */
        tvg_canvas_remove(canvas, pic);
        tvg_canvas_destroy(canvas);
        rc = QL_OK;
    }

done:
    pthread_mutex_unlock(&g_mutex);
    free(replaced);
    return rc;
}

extern "C" QLErr ql_svg_load_font_thorvg(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return QL_ERR_INVAL;
    pthread_mutex_lock(&g_mutex);
    const bool ok = ensure_engine(NULL, 0) &&
                    tvg_font_load(path) == TVG_RESULT_SUCCESS;
    pthread_mutex_unlock(&g_mutex);
    return ok ? QL_OK : QL_ERR_INVAL;
}
