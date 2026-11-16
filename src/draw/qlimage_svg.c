/**
 * @file    qlimage_svg.c
 * @brief   运行期 SVG 解码：PlutoSVG 解析 + PlutoVG 光栅化。
 *
 * 定位很明确：**图标级的静态 SVG**。支持 path/矩形/圆/椭圆/线/多边形、
 * 渐变、transform、`use`；不支持 `<text>`（没开 FreeType）、滤镜、
 * CSS 样式表、clipPath/mask、SMIL/CSS 动画。复杂插画请构建期预渲染成位图。
 *
 * 外来数据仍然按"不信任"处理：
 *   - 字节数由 `ql_image_decode()` 先按 `QL_CFG_IMAGE_MAX_ENCODED_BYTES` 拒绝；
 *   - 这里再做一遍结构预扫描：嵌套深度 / 元素个数上限；
 *   - 拒绝脚本、外链（http/https/协议相对）、`@import`；
 *   - 输出尺寸夹到 `QL_CFG_IMAGE_SVG_MAX_DIM`，并按解码预算检查 surface。
 *
 * 线程模型：PlutoSVG 每次解码独立建文档、独立 surface，可以并发调用；
 * 但本库的图像解码本身按"UI 线程一次"设计，这里不额外加锁。
 */
#include <qlgl/qlgl.h>

#if QL_FEAT_IMG_SVG

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <plutosvg.h>

#include "qlimage_svg.h"

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
 * 粗扫标签结构：只看 `<` / `>`，不解析属性（真正的解析交给 PlutoSVG）。
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

/* 目标尺寸：0 = 用固有尺寸；只给一边按比例补另一边；最后夹上限 */
static void svg_target_size(float iw, float ih, const QLImageDecodeReq *req,
                            int *tw, int *th, const char **clamped)
{
    int w = req->target_w;
    int h = req->target_h;

    if (w <= 0 && h <= 0) {
        w = (int)(iw + 0.5f);
        h = (int)(ih + 0.5f);
    } else if (w <= 0) {
        w = (int)((float)h * iw / ih + 0.5f);
    } else if (h <= 0) {
        h = (int)((float)w * ih / iw + 0.5f);
    }

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* 超上限时**按比例**缩，不能把宽高各自截断（那会改变图标的长宽比） */
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

int ql_image_decode_svg(const QLImageDecodeReq *req, QLImage *out,
                        uint8_t out_format, char *err, size_t err_cap)
{
    if (req == NULL || req->data == NULL || req->len == 0 || out == NULL)
        return QL_ERR_INVAL;
    if (req->len > (size_t)0x7FFFFFFF)
        return QL_ERR_INVAL;

    if (!svg_precheck_refs(req->data, req->len, err, err_cap) ||
        !svg_precheck_structure(req->data, req->len, err, err_cap))
        return QL_ERR_INVAL;

    plutosvg_document_t *doc = plutosvg_document_load_from_data(
        (const char *)req->data, (int)req->len, -1.0f, -1.0f, NULL, NULL);
    if (doc == NULL) {
        svg_set_err(err, err_cap, "SVG 解析失败（畸形 XML 或不支持的结构）");
        return QL_ERR_INVAL;
    }

    float iw = plutosvg_document_get_width(doc);
    float ih = plutosvg_document_get_height(doc);
    if (!(iw > 0.0f) || !(ih > 0.0f)) {       /* 没有 width/height/viewBox */
        iw = 300.0f;
        ih = 150.0f;
    }

    int tw = 0, th = 0;
    const char *clamped = NULL;
    svg_target_size(iw, ih, req, &tw, &th, &clamped);
    if (clamped != NULL)
        svg_set_err(err, err_cap, "SVG 尺寸超过上限，已按上限栅格化");

    /* surface 是 4 B/px；先按最贵的形态过一遍预算 */
    const uint64_t need = (uint64_t)(uint32_t)tw * (uint64_t)(uint32_t)th * 4u;
    if (need > (uint64_t)ql_image_decode_budget()) {
        svg_set_err(err, err_cap, "SVG 目标尺寸超过解码预算");
        plutosvg_document_destroy(doc);
        return QL_ERR_NOMEM;
    }

    plutovg_surface_t *surface = plutosvg_document_render_to_surface(
        doc, NULL, tw, th, NULL, NULL, NULL);
    if (surface == NULL) {
        svg_set_err(err, err_cap, "SVG 光栅化失败");
        plutosvg_document_destroy(doc);
        return QL_ERR_INVAL;
    }

    const int sw = plutovg_surface_get_width(surface);
    const int sh = plutovg_surface_get_height(surface);
    const int sstride = plutovg_surface_get_stride(surface);
    const unsigned char *sdata = plutovg_surface_get_data(surface);

    int rc = QL_ERR_INVAL;
    if (sw <= 0 || sh <= 0 || sdata == NULL || sstride != sw * 4 ||
        sw > 0xFFFF || sh > 0xFFFF) {
        svg_set_err(err, err_cap, "SVG 光栅化结果非法");
        goto done;
    }

    const size_t rgba_len = (size_t)sw * (size_t)sh * 4u;
    uint8_t *rgba = (uint8_t *)ql_mem_alloc(rgba_len);
    if (rgba == NULL) {
        svg_set_err(err, err_cap, "SVG 像素缓冲分配失败");
        rc = QL_ERR_NOMEM;
        goto done;
    }

    /* PlutoVG 的 surface 是预乘 ARGB；QLImage 要非预乘 RGBA */
    plutovg_convert_argb_to_rgba(rgba, sdata, sw, sh, sstride);

    if (out_format == QL_IMAGE_RGBA8888) {
        out->px = rgba;
        out->len = (uint32_t)rgba_len;
        out->w = (uint16_t)sw;
        out->h = (uint16_t)sh;
        out->stride = (uint16_t)(sw * 4);
        out->format = QL_IMAGE_RGBA8888;
        out->owned = 1;
        out->rev = 0;
        rc = QL_OK;
    } else if (out_format == QL_IMAGE_A8) {
        /*
         * A8 在 UI 里是**遮罩**（`mask-image` / 单色图标染色）：
         * 取 SVG 的覆盖率（alpha），而不是黑底图形的亮度 —— 否则一个
         * `fill="#000"` + 透明背景的图标会得到全 0（黑 = 0），完全反了。
         */
        const size_t a8_len = (size_t)sw * (size_t)sh;
        uint8_t *a8 = (uint8_t *)ql_mem_alloc(a8_len);
        if (a8 == NULL) {
            svg_set_err(err, err_cap, "SVG 遮罩缓冲分配失败");
            ql_mem_free(rgba);
            rc = QL_ERR_NOMEM;
            goto done;
        }
        for (size_t i = 0; i < a8_len; i++)
            a8[i] = rgba[i * 4u + 3u];
        ql_mem_free(rgba);
        out->px = a8;
        out->len = (uint32_t)a8_len;
        out->w = (uint16_t)sw;
        out->h = (uint16_t)sh;
        out->stride = (uint16_t)sw;
        out->format = QL_IMAGE_A8;
        out->owned = 1;
        out->rev = 0;
        rc = QL_OK;
    } else {
        QLImage src;
        memset(&src, 0, sizeof(src));
        src.px = rgba;
        src.len = (uint32_t)rgba_len;
        src.w = (uint16_t)sw;
        src.h = (uint16_t)sh;
        src.stride = (uint16_t)(sw * 4);
        src.format = QL_IMAGE_RGBA8888;
        src.owned = 1;
        rc = ql_image_convert(&src, out_format, out);
        ql_mem_free(rgba);
    }

done:
    plutovg_surface_destroy(surface);
    plutosvg_document_destroy(doc);
    return rc;
}

#endif /* QL_FEAT_IMG_SVG */
