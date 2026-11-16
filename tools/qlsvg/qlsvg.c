/**
 * @file    qlsvg.c
 * @brief   构建期 SVG → PNG 工具（MCU / 预渲染资源用）。
 *
 * 和运行期解码用的是**同一份** PlutoSVG + PlutoVG，所以"工具转出来的图"
 * 与桌面运行期渲染的结果一致，不会出现两套 SVG 语义。
 *
 * 用法：
 *     qlsvg icon.svg icon.png              # 按 SVG 固有尺寸
 *     qlsvg icon.svg icon.png -w 32 -h 32  # 指定尺寸（只给一边按比例）
 *     qlsvg icon.svg icon.png --alpha      # 导出覆盖率遮罩（给 qltext -a8 用）
 *
 * 只做静态 SVG：path/形状/渐变/transform/use；不做 text/filter/CSS/SMIL。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <plutosvg.h>

#define QLSVG_MAX_BYTES  (32u * 1024u * 1024u)
#define QLSVG_MAX_DIM    4096

static void usage(const char *argv0)
{
    fprintf(stderr,
            "用法: %s 输入.svg 输出.png [-w 宽] [-h 高] [--alpha]\n"
            "  -w/-h     输出像素尺寸（0 = SVG 固有尺寸；只给一边按比例）\n"
            "  --alpha   只导出覆盖率（白=不透明/黑=透明），适合 qltext -a8\n",
            argv0);
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > QLSVG_MAX_DIM)
        return 0;
    *out = (int)v;
    return 1;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int req_w = 0, req_h = 0;
    int alpha_only = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "-w") == 0 || strcmp(a, "--width") == 0) {
            if (i + 1 >= argc || !parse_int(argv[++i], &req_w)) {
                fprintf(stderr, "qlsvg: -w 需要 0..%d 的整数（0 = 固有尺寸）\n", QLSVG_MAX_DIM);
                return 2;
            }
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--height") == 0) {
            if (i + 1 >= argc || !parse_int(argv[++i], &req_h)) {
                fprintf(stderr, "qlsvg: -h 需要 0..%d 的整数（0 = 固有尺寸）\n", QLSVG_MAX_DIM);
                return 2;
            }
        } else if (strcmp(a, "--alpha") == 0) {
            alpha_only = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "qlsvg: 未知参数 %s\n", a);
            usage(argv[0]);
            return 2;
        } else if (in == NULL) {
            in = a;
        } else if (out == NULL) {
            out = a;
        } else {
            fprintf(stderr, "qlsvg: 多余的参数 %s\n", a);
            return 2;
        }
    }

    if (in == NULL || out == NULL) {
        usage(argv[0]);
        return 2;
    }

    FILE *f = fopen(in, "rb");
    if (f == NULL) {
        fprintf(stderr, "qlsvg: 打不开 \"%s\"\n", in);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 1;
    }
    const long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || (size_t)fsz > QLSVG_MAX_BYTES) {
        fprintf(stderr, "qlsvg: \"%s\" 大小非法（上限 %u 字节）\n", in,
                QLSVG_MAX_BYTES);
        fclose(f);
        return 1;
    }
    char *data = (char *)malloc((size_t)fsz);
    if (data == NULL || fread(data, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(data);
        fclose(f);
        fprintf(stderr, "qlsvg: 读不了 \"%s\"\n", in);
        return 1;
    }
    fclose(f);

    plutosvg_document_t *doc =
        plutosvg_document_load_from_data(data, (int)fsz, -1.0f, -1.0f, NULL, NULL);
    if (doc == NULL) {
        fprintf(stderr, "qlsvg: 解析失败（畸形 XML 或不支持的结构）\n");
        free(data);
        return 1;
    }

    float iw = plutosvg_document_get_width(doc);
    float ih = plutosvg_document_get_height(doc);
    if (!(iw > 0.0f) || !(ih > 0.0f)) {
        iw = 300.0f;
        ih = 150.0f;
    }

    int w = req_w, h = req_h;
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
    /* 超上限按比例缩，保持长宽比 */
    if (w > QLSVG_MAX_DIM) {
        h = (int)((double)h * QLSVG_MAX_DIM / (double)w + 0.5);
        w = QLSVG_MAX_DIM;
    }
    if (h > QLSVG_MAX_DIM) {
        w = (int)((double)w * QLSVG_MAX_DIM / (double)h + 0.5);
        h = QLSVG_MAX_DIM;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    plutovg_surface_t *surface = plutosvg_document_render_to_surface(
        doc, NULL, w, h, NULL, NULL, NULL);
    plutosvg_document_destroy(doc);
    free(data);
    if (surface == NULL) {
        fprintf(stderr, "qlsvg: 光栅化失败\n");
        return 1;
    }

    int ok = 0;
    if (!alpha_only) {
        ok = plutovg_surface_write_to_png(surface, out) ? 1 : 0;
    } else {
        /*
         * `--alpha`：把覆盖率写成灰度（白 = 不透明）。
         * 下游 `qltext cc --pixel-format a8` 按亮度取，正好得到遮罩；
         * 如果直接给透明背景 + 黑色图形，亮度全是 0，遮罩会反。
         */
        plutovg_surface_t *mask = plutovg_surface_create(w, h);
        if (mask != NULL) {
            unsigned char *src = plutovg_surface_get_data(surface);
            unsigned char *dst = plutovg_surface_get_data(mask);
            const int stride = plutovg_surface_get_stride(surface);
            for (int y = 0; y < h; y++) {
                const uint32_t *srow = (const uint32_t *)(src + (size_t)y * stride);
                uint32_t *drow = (uint32_t *)(dst + (size_t)y * stride);
                for (int x = 0; x < w; x++) {
                    const uint32_t a = (srow[x] >> 24) & 0xFFu;
                    drow[x] = 0xFF000000u | (a << 16) | (a << 8) | a;
                }
            }
            ok = plutovg_surface_write_to_png(mask, out) ? 1 : 0;
            plutovg_surface_destroy(mask);
        }
    }

    plutovg_surface_destroy(surface);
    if (!ok) {
        fprintf(stderr, "qlsvg: 写出 \"%s\" 失败\n", out);
        return 1;
    }
    printf("qlsvg: %s → %s（%dx%d%s）\n", in, out, w, h,
           alpha_only ? "，覆盖率遮罩" : "");
    return 0;
}
