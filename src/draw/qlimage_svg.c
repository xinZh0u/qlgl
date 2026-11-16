/**
 * @file    qlimage_svg.c
 * @brief   运行期 SVG 解码：公共预处理 + 后端分发（PlutoSVG / ThorVG）。
 *
 * 定位：**图标级的静态 SVG**。公共部分负责：
 *   - 外来数据的有界检查（深度/元素数/外链/脚本/预算）；
 *   - 从 `<svg width/height/viewBox>` 解析固有尺寸；
 *   - 计算目标像素尺寸（按比例、单边上限、DPR 由调用方给）；
 *   - 把后端的非预乘 RGBA 转成 `out_format`。
 *
 * 后端只做"把 SVG 画进 W×H 的 RGBA 缓冲"：
 *   - PlutoSVG（纯 C，默认）；
 *   - ThorVG（C++ 内核 + C API，可选，`QLGL_BUILD_THORVG=ON`）。
 * 运行时用 `ql_image_svg_set_backend("thorvg")` 切换，便于同一份资产对比。
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#if QL_FEAT_IMG_SVG

#if QL_FEAT_IMG_SVG_PLUTOSVG
#  include <plutosvg.h>
#endif

/* ================================================================== */
/* 外来数据预扫描                                                      */
/* ================================================================== */

static void svg_set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static const uint8_t *svg_find(const uint8_t *d, size_t n, const char *needle)
{
    const size_t m = strlen(needle);
    if (m == 0 || n < m)
        return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(d + i, needle, m) == 0)
            return d + i;
    return NULL;
}

/* 写在 SVG 文本里的危险引用：一律拒绝（不做"尽力而为"的过滤） */
static int svg_precheck_refs(const uint8_t *d, size_t n, char *err, size_t cap)
{
    static const char *const bad[] = {
        "<script", "javascript:", "href=\"http", "href='http",
        "href=\"//", "href='//", "xlink:href=\"http", "xlink:href='http",
        "url(http", "url('http", "url(\"http", "@import",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (svg_find(d, n, bad[i]) != NULL) {
            svg_set_err(err, cap, "SVG 含有被禁止的外部引用/脚本");
            return 0;
        }
    }
    return 1;
}

/*
 * 粗扫标签结构：只看 `<` / `>`，不解析属性（真正的解析交给后端）。
 * 目的只有两个：限制嵌套深度与元素总数。注释/CDATA/处理指令先跳过。
 */
static int svg_precheck_structure(const uint8_t *d, size_t n, char *err,
                                  size_t cap)
{
    size_t i = 0;
    int depth = 0;
    int elements = 0;

    while (i < n) {
        const uint8_t *lt = (const uint8_t *)memchr(d + i, '<', n - i);
        if (lt == NULL)
            break;
        const size_t p = (size_t)(lt - d);

        if (p + 4 <= n && d[p + 1] == '!' && d[p + 2] == '-' && d[p + 3] == '-') {
            const uint8_t *e = svg_find(d + p + 4, n - p - 4, "-->");
            if (e == NULL)
                break;
            i = (size_t)(e - d) + 3u;
            continue;
        }
        if (p + 9 <= n && memcmp(d + p + 1, "![CDATA[", 8) == 0) {
            const uint8_t *e = svg_find(d + p + 9, n - p - 9, "]]>");
            if (e == NULL)
                break;
            i = (size_t)(e - d) + 3u;
            continue;
        }
        if (p + 2 <= n && d[p + 1] == '?') {
            const uint8_t *e = svg_find(d + p + 2, n - p - 2, "?>");
            if (e == NULL)
                break;
            i = (size_t)(e - d) + 2u;
            continue;
        }
        if (p + 2 <= n && d[p + 1] == '!') {          /* DOCTYPE / 声明 */
            size_t q = p;
            int bracket = 0;
            while (q < n) {
                if (d[q] == '[')
                    bracket++;
                else if (d[q] == ']' && bracket > 0)
                    bracket--;
                else if (d[q] == '>' && bracket == 0)
                    break;
                q++;
            }
            if (q >= n)
                break;
            i = q + 1u;
            continue;
        }
        if (p + 2 <= n && d[p + 1] == '/') {          /* 结束标签 */
            if (depth > 0)
                depth--;
            i = p + 2;
            continue;
        }

        const uint8_t *gt = (const uint8_t *)memchr(d + p, '>', n - p);
        if (gt == NULL)
            break;
        const size_t q = (size_t)(gt - d);

        size_t k = q;
        while (k > p + 1 && (d[k - 1] == ' ' || d[k - 1] == '\t' ||
                             d[k - 1] == '\r' || d[k - 1] == '\n'))
            k--;
        const int self_closing = (k > p + 1 && d[k - 1] == '/');

        elements++;
        if (elements > QL_CFG_IMAGE_SVG_MAX_ELEMENTS) {
            svg_set_err(err, cap, "SVG 元素个数超过上限");
            return 0;
        }
        if (!self_closing) {
            depth++;
            if (depth > QL_CFG_IMAGE_SVG_MAX_DEPTH) {
                svg_set_err(err, cap, "SVG 嵌套过深");
                return 0;
            }
        }
        i = q + 1u;
    }
    return 1;
}

/* ================================================================== */
/* 固有尺寸（width/height/viewBox，独立于后端）                        */
/* ================================================================== */

/* 在 `<svg ...>` 标签文本里找属性值（前面必须是空白/`<`，避免匹配 data-width） */
static const char *svg_attr(const char *tag, size_t len, const char *name,
                            size_t *vlen)
{
    const size_t n = strlen(name);
    for (size_t i = 0; i + n < len; i++) {
        if (i > 0) {
            const unsigned char c = (unsigned char)tag[i - 1];
            if (isalnum(c) || c == '-' || c == '_' || c == ':')
                continue;
        }
        if (strncmp(tag + i, name, n) != 0)
            continue;
        size_t j = i + n;
        while (j < len && isspace((unsigned char)tag[j]))
            j++;
        if (j >= len || tag[j] != '=')
            continue;
        j++;
        while (j < len && isspace((unsigned char)tag[j]))
            j++;
        if (j >= len)
            return NULL;
        const char q = tag[j];
        if (q != '"' && q != '\'')
            return NULL;
        j++;
        const char *v = tag + j;
        while (j < len && tag[j] != q)
            j++;
        if (j >= len)
            return NULL;
        *vlen = (size_t)(tag + j - v);
        return v;
    }
    return NULL;
}

/* SVG 长度 → px；`%` / 未知单位按"没写"处理（返回 0） */
static double svg_len_px(const char *s, size_t n)
{
    char buf[32];
    if (n == 0 || n >= sizeof(buf))
        return 0.0;
    memcpy(buf, s, n);
    buf[n] = '\0';

    char *end = NULL;
    const double v = strtod(buf, &end);
    if (end == buf || !(v > 0.0))
        return 0.0;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end == '\0' || strcmp(end, "px") == 0)
        return v;
    if (strcmp(end, "pt") == 0)  return v * 96.0 / 72.0;
    if (strcmp(end, "pc") == 0)  return v * 16.0;
    if (strcmp(end, "in") == 0)  return v * 96.0;
    if (strcmp(end, "cm") == 0)  return v * 96.0 / 2.54;
    if (strcmp(end, "mm") == 0)  return v * 96.0 / 25.4;
    if (strcmp(end, "em") == 0 || strcmp(end, "rem") == 0) return v * 16.0;
    if (strcmp(end, "ex") == 0)  return v * 8.0;
    return 0.0;
}

/* viewBox="minx miny w h"（逗号/空白分隔都认） */
static void svg_parse_viewbox(const char *s, size_t n, double *w, double *h)
{
    char buf[128];
    if (n == 0 || n >= sizeof(buf))
        return;
    memcpy(buf, s, n);
    buf[n] = '\0';

    double v[4] = { 0, 0, 0, 0 };
    const char *p = buf;
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '\0')
            return;
        v[i] = strtod(p, &end);
        if (end == p)
            return;
        p = end;
    }
    if (v[2] > 0.0 && v[3] > 0.0) {
        *w = v[2];
        *h = v[3];
    }
}

/*
 * 固有尺寸：width/height 优先，缺一边用 viewBox 的宽高比补齐；
 * 两者都没有就用 viewBox；再没有按 SVG 规范的默认 300×150。
 */
static void svg_intrinsic_size(const uint8_t *d, size_t n, int *out_w, int *out_h)
{
    const char *text = (const char *)d;
    const char *tag = NULL;
    size_t tag_len = 0;

    for (size_t i = 0; i + 4 < n; i++) {
        if (text[i] != '<' || text[i + 1] != 's' || text[i + 2] != 'v' ||
            text[i + 3] != 'g')
            continue;
        const unsigned char c = (unsigned char)text[i + 4];
        if (!(isspace(c) || c == '>' || c == '/'))
            continue;
        const char *end = memchr(text + i, '>', n - i);
        if (end == NULL)
            break;
        tag = text + i;
        tag_len = (size_t)(end - tag);
        break;
    }

    double w = 0.0, h = 0.0, vw = 0.0, vh = 0.0;
    if (tag != NULL) {
        size_t vn = 0;
        const char *v = svg_attr(tag, tag_len, "width", &vn);
        if (v != NULL)
            w = svg_len_px(v, vn);
        v = svg_attr(tag, tag_len, "height", &vn);
        if (v != NULL)
            h = svg_len_px(v, vn);
        v = svg_attr(tag, tag_len, "viewBox", &vn);
        if (v != NULL)
            svg_parse_viewbox(v, vn, &vw, &vh);
    }

    if (w > 0.0 && h <= 0.0 && vw > 0.0 && vh > 0.0)
        h = w * vh / vw;
    if (h > 0.0 && w <= 0.0 && vw > 0.0 && vh > 0.0)
        w = h * vw / vh;
    if (!(w > 0.0) || !(h > 0.0)) {
        if (vw > 0.0 && vh > 0.0) {
            w = vw;
            h = vh;
        } else {
            w = 300.0;
            h = 150.0;
        }
    }

    *out_w = (int)(w + 0.5);
    *out_h = (int)(h + 0.5);
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/* ================================================================== */
/* 后端接口                                                           */
/* ================================================================== */

typedef struct QLSvgRenderer {
    const char *name;
    /*
     * 把 SVG 渲染成 W×H 的**非预乘 RGBA8888**（rgba 已按 W*H*4 分配）。
     * 失败时写 err 并返回 QL_ERR_INVAL / QL_ERR_NOMEM / QL_ERR_UNSUPPORTED。
     */
    QLErr (*render)(const uint8_t *data, size_t len, int w, int h,
                    uint32_t current_color, uint8_t *rgba,
                    char *err, size_t err_cap);
} QLSvgRenderer;

static const QLSvgRenderer *s_svg_backend;   /* NULL = 默认 */

#if QL_FEAT_IMG_SVG_PLUTOSVG
static QLErr render_plutosvg(const uint8_t *data, size_t len, int w, int h,
                             uint32_t current_color, uint8_t *rgba,
                             char *err, size_t err_cap)
{
    if (len > (size_t)0x7FFFFFFF)
        return QL_ERR_INVAL;

    plutosvg_document_t *doc = plutosvg_document_load_from_data(
        (const char *)data, (int)len, -1.0f, -1.0f, NULL, NULL);
    if (doc == NULL) {
        svg_set_err(err, err_cap, "SVG 解析失败（畸形 XML 或不支持的结构）");
        return QL_ERR_INVAL;
    }

    plutovg_color_t color;
    color.r = (float)((current_color >> 16) & 0xFFu) / 255.0f;
    color.g = (float)((current_color >> 8) & 0xFFu) / 255.0f;
    color.b = (float)(current_color & 0xFFu) / 255.0f;
    color.a = 1.0f;

    plutovg_surface_t *surface = plutosvg_document_render_to_surface(
        doc, NULL, w, h, &color, NULL, NULL);
    if (surface == NULL) {
        svg_set_err(err, err_cap, "SVG 光栅化失败");
        plutosvg_document_destroy(doc);
        return QL_ERR_INVAL;
    }

    const int sw = plutovg_surface_get_width(surface);
    const int sh = plutovg_surface_get_height(surface);
    const int stride = plutovg_surface_get_stride(surface);
    const unsigned char *src = plutovg_surface_get_data(surface);
    if (sw != w || sh != h || src == NULL || stride != w * 4) {
        svg_set_err(err, err_cap, "SVG 光栅化结果尺寸不符");
        plutovg_surface_destroy(surface);
        plutosvg_document_destroy(doc);
        return QL_ERR_INVAL;
    }

    /* PlutoVG 的 surface 是预乘 ARGB；QLImage 要非预乘 RGBA */
    plutovg_convert_argb_to_rgba(rgba, src, w, h, stride);

    plutovg_surface_destroy(surface);
    plutosvg_document_destroy(doc);
    return QL_OK;
}

static const QLSvgRenderer k_backend_plutosvg = { "plutosvg", render_plutosvg };
#endif /* QL_FEAT_IMG_SVG_PLUTOSVG */

#if QL_FEAT_IMG_SVG_THORVG
/* 定义在 qlimage_svg_thorvg.cpp（C++），这里只声明 */
QLErr ql_svg_render_thorvg(const uint8_t *data, size_t len, int w, int h,
                           uint32_t current_color, uint8_t *rgba,
                           char *err, size_t err_cap);

static const QLSvgRenderer k_backend_thorvg = { "thorvg", ql_svg_render_thorvg };
#endif

static const QLSvgRenderer *svg_default_backend(void)
{
#if QL_FEAT_IMG_SVG_PLUTOSVG
    return &k_backend_plutosvg;
#elif QL_FEAT_IMG_SVG_THORVG
    return &k_backend_thorvg;
#else
    return NULL;
#endif
}

const char *ql_image_svg_backend_name(void)
{
    const QLSvgRenderer *b = (s_svg_backend != NULL) ? s_svg_backend
                                                     : svg_default_backend();
    return (b != NULL) ? b->name : "none";
}

QLErr ql_image_svg_set_backend(const char *name)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, "auto") == 0) {
        s_svg_backend = NULL;
        return QL_OK;
    }
#if QL_FEAT_IMG_SVG_PLUTOSVG
    if (strcmp(name, "plutosvg") == 0 || strcmp(name, "pluto") == 0) {
        s_svg_backend = &k_backend_plutosvg;
        return QL_OK;
    }
#endif
#if QL_FEAT_IMG_SVG_THORVG
    if (strcmp(name, "thorvg") == 0) {
        s_svg_backend = &k_backend_thorvg;
        return QL_OK;
    }
#endif
    return QL_ERR_UNSUPPORTED;
}

/* ================================================================== */
/* 目标尺寸                                                           */
/* ================================================================== */

static void svg_target_size(int iw, int ih, const QLImageDecodeReq *req, int *tw,
                            int *th, const char **clamped)
{
    int w = req->target_w;
    int h = req->target_h;

    if (w <= 0 && h <= 0) {
        w = iw;
        h = ih;
    } else if (w <= 0) {
        w = (int)((int64_t)h * iw / ih);
    } else if (h <= 0) {
        h = (int)((int64_t)w * ih / iw);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* 超上限时按比例缩，保留长宽比 */
    if (w > QL_CFG_IMAGE_SVG_MAX_DIM) {
        h = (int)((double)h * QL_CFG_IMAGE_SVG_MAX_DIM / (double)w + 0.5);
        w = QL_CFG_IMAGE_SVG_MAX_DIM;
        *clamped = "宽";
    }
    if (h > QL_CFG_IMAGE_SVG_MAX_DIM) {
        w = (int)((double)w * QL_CFG_IMAGE_SVG_MAX_DIM / (double)h + 0.5);
        h = QL_CFG_IMAGE_SVG_MAX_DIM;
        *clamped = (*clamped != NULL) ? "宽和高" : "高";
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *tw = w;
    *th = h;
}

/* ================================================================== */
/* 入口                                                               */
/* ================================================================== */

int ql_image_decode_svg(const QLImageDecodeReq *req, QLImage *out,
                        uint8_t out_format, char *err, size_t err_cap)
{
    if (req == NULL || req->data == NULL || req->len == 0 || out == NULL)
        return QL_ERR_INVAL;

    if (!svg_precheck_refs(req->data, req->len, err, err_cap) ||
        !svg_precheck_structure(req->data, req->len, err, err_cap))
        return QL_ERR_INVAL;

    int iw = 0, ih = 0;
    svg_intrinsic_size(req->data, req->len, &iw, &ih);

    int tw = 0, th = 0;
    const char *clamped = NULL;
    svg_target_size(iw, ih, req, &tw, &th, &clamped);
    if (clamped != NULL)
        svg_set_err(err, err_cap, "SVG 尺寸超过上限，已按上限栅格化");

    const uint64_t need = (uint64_t)(uint32_t)tw * (uint64_t)(uint32_t)th * 4u;
    if (need > (uint64_t)ql_image_decode_budget()) {
        svg_set_err(err, err_cap, "SVG 目标尺寸超过解码预算");
        return QL_ERR_NOMEM;
    }

    const QLSvgRenderer *be = (s_svg_backend != NULL) ? s_svg_backend
                                                      : svg_default_backend();
    if (be == NULL) {
        svg_set_err(err, err_cap, "本构建没有可用的 SVG 后端");
        return QL_ERR_UNSUPPORTED;
    }

    uint8_t *rgba = (uint8_t *)ql_mem_alloc((size_t)tw * (size_t)th * 4u);
    if (rgba == NULL) {
        svg_set_err(err, err_cap, "SVG 像素缓冲分配失败");
        return QL_ERR_NOMEM;
    }

    const QLErr rc = be->render(req->data, req->len, tw, th, req->current_color,
                                rgba, err, err_cap);
    if (rc != QL_OK) {
        ql_mem_free(rgba);
        return rc;
    }

    int result = QL_ERR_INVAL;
    const size_t rgba_len = (size_t)tw * (size_t)th * 4u;

    if (out_format == QL_IMAGE_RGBA8888) {
        out->px = rgba;
        out->len = (uint32_t)rgba_len;
        out->w = (uint16_t)tw;
        out->h = (uint16_t)th;
        out->stride = (uint16_t)(tw * 4);
        out->format = QL_IMAGE_RGBA8888;
        out->owned = 1;
        out->rev = 0;
        result = QL_OK;
    } else if (out_format == QL_IMAGE_A8) {
        /*
         * A8 在 UI 里是**遮罩**：取覆盖率（alpha），不是黑色图形的亮度。
         */
        const size_t a8_len = (size_t)tw * (size_t)th;
        uint8_t *a8 = (uint8_t *)ql_mem_alloc(a8_len);
        if (a8 == NULL) {
            svg_set_err(err, err_cap, "SVG 遮罩缓冲分配失败");
            ql_mem_free(rgba);
            return QL_ERR_NOMEM;
        }
        for (size_t i = 0; i < a8_len; i++)
            a8[i] = rgba[i * 4u + 3u];
        ql_mem_free(rgba);
        out->px = a8;
        out->len = (uint32_t)a8_len;
        out->w = (uint16_t)tw;
        out->h = (uint16_t)th;
        out->stride = (uint16_t)tw;
        out->format = QL_IMAGE_A8;
        out->owned = 1;
        out->rev = 0;
        result = QL_OK;
    } else {
        QLImage src;
        memset(&src, 0, sizeof(src));
        src.px = rgba;
        src.len = (uint32_t)rgba_len;
        src.w = (uint16_t)tw;
        src.h = (uint16_t)th;
        src.stride = (uint16_t)(tw * 4);
        src.format = QL_IMAGE_RGBA8888;
        src.owned = 1;
        result = ql_image_convert(&src, out_format, out);
        ql_mem_free(rgba);
    }
    return result;
}

#else /* !QL_FEAT_IMG_SVG */

QLErr ql_image_svg_set_backend(const char *name)
{
    (void)name;
    return QL_ERR_UNSUPPORTED;
}

const char *ql_image_svg_backend_name(void)
{
    return "none";
}

#endif /* QL_FEAT_IMG_SVG */
