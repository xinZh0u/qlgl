/**
 * @file    test_golden.c
 * @brief   golden 图套件：每个 CSS 用例渲染一张参考图，和 test/golden/ 里的基准逐像素比。
 *
 * 为什么要有它：特性面（flex / position / 渐变 / 阴影 / 过渡 / 关键帧 / 遮罩 / 缩放 /
 * 毛玻璃…）已经大到"改一处 shorthand 顺序就可能悄悄改到另一页"，
 * 靠人眼看不出来。M3.5 要在 C 里复刻一套解析语义，没有自动回归根本做不下去。
 *
 * 三条用法：
 *   ./test_golden                      比基准（默认容差，见下）
 *   QLGL_GOLDEN_UPDATE=1 ./test_golden 重建全部基准（**只在确认画面是对的之后**）
 *   QLGL_GOLDEN_EXACT=1 ./test_golden  逐像素严格相等（本机自测用）
 *
 * 容差为什么不是 0：这些页里几乎都有文字，字形是 FreeType 光栅化的结果，
 * 换一个 FreeType/字体版本就会有 1~2 个灰度级的差别。所以默认判据是
 * "单通道差 > 12 的像素不超过 0.5%"。版面/颜色/几何的回归远大于这个量级。
 *
 * 除了图像，还比一份**逐字节**的 `.qlui` blob：
 *   - 图像那层有容差（字形受 FreeType 版本影响），只能抓"看得见的"回归；
 *   - blob 这层是精确的：任何一个 CSS 值改了 1 个色阶、间距差 1px、圆角从 12 变 11，
 *     都会让字节不同。而且它是**机器无关**的（全是整数，不受字体影响）。
 *   - 这条正是 M3.5 第 3 步（C 版解析器）的验收标准：
 *     "构建期 qltext 产出的 blob 与提交的基准逐字节相同"。
 *
 * 顺带把**全部** flex 用例的 CPU vs GPU 一致性也自动覆盖了（以前只手挑 3 页）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#include "ui_flex_row.h"
#include "ui_flex_grow.h"
#include "ui_flex_justify.h"
#include "ui_flex_align.h"
#include "ui_flex_wrap.h"
#include "ui_flex_column.h"
#include "ui_flex_posabs.h"
#include "ui_flex_posfix.h"
#include "ui_flex_posrel.h"
#include "ui_flex_zidx.h"
#include "ui_flex_ovf.h"
#include "ui_flex_mixed.h"
#include "ui_flex_frac.h"
#include "ui_flex_paint.h"
#include "ui_flex_dpr.h"
#include "ui_flex_trans.h"
#include "ui_flex_kf.h"
#include "ui_flex_grad.h"
#include "ui_flex_clip.h"
#include "ui_flex_scale.h"
#include "ui_flex_flip.h"
#include "ui_flex_flipdemo.h"
#include "ui_flex_showcase.h"
#include "ui_flex_blur.h"
#include "ui_flex_anim.h"
#include "ui_flex_anim10.h"
#include "ui_flex_bench100.h"
#include "ui_flex_tags.h"
#include "ui_flex_b4.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

/* 基准目录由 CMake 传进来（源码树里的 test/golden） */
#ifndef QL_GL_GOLDEN_DIR
#define QL_GL_GOLDEN_DIR "test/golden"
#endif

/* 布局用的"足够大的"视口：每页的 #root 都有自己的 width/height，实际尺寸从根节点取 */
#define VIEW_W 900
#define VIEW_H 700

typedef struct {
    const char    *name;
    const QLUIDoc *doc;
    /*
     * 这一页允许的 CPU vs GPU 差异（比例）。绝大多数页是整数运算、逐像素相同，
     * 所以卡在 2%；`blur` 是唯一例外——CPU 走全分辨率盒式、GPU 走半分辨率 9 抽头，
     * 硬边摊开的斜坡宽度本来就不一样（实测 3.1%）。
     */
    double         gpu_tol;
} GoldenCase;

static const GoldenCase k_cases[] = {
    { "row", &ui_flex_row_doc, 0.02 },
    { "grow", &ui_flex_grow_doc, 0.02 },
    { "justify", &ui_flex_justify_doc, 0.02 },
    { "align", &ui_flex_align_doc, 0.02 },
    { "wrap", &ui_flex_wrap_doc, 0.02 },
    { "column", &ui_flex_column_doc, 0.02 },
    { "posabs", &ui_flex_posabs_doc, 0.02 },
    { "posfix", &ui_flex_posfix_doc, 0.02 },
    { "posrel", &ui_flex_posrel_doc, 0.02 },
    { "zidx", &ui_flex_zidx_doc, 0.02 },
    { "ovf", &ui_flex_ovf_doc, 0.02 },
    { "mixed", &ui_flex_mixed_doc, 0.02 },
    { "frac", &ui_flex_frac_doc, 0.02 },
    { "paint", &ui_flex_paint_doc, 0.02 },
    { "dpr", &ui_flex_dpr_doc, 0.02 },
    { "trans", &ui_flex_trans_doc, 0.02 },
    { "kf", &ui_flex_kf_doc, 0.02 },
    { "grad", &ui_flex_grad_doc, 0.02 },
    { "clip", &ui_flex_clip_doc, 0.02 },
    { "scale", &ui_flex_scale_doc, 0.02 },
    { "flip", &ui_flex_flip_doc, 0.02 },
    { "flipdemo", &ui_flex_flipdemo_doc, 0.02 },
    { "showcase", &ui_flex_showcase_doc, 0.02 },
    { "blur", &ui_flex_blur_doc, 0.06 },
    { "anim", &ui_flex_anim_doc, 0.02 },
    { "anim10", &ui_flex_anim10_doc, 0.02 },
    { "bench100", &ui_flex_bench100_doc, 0.02 },
    { "tags", &ui_flex_tags_doc, 0.02 },
    { "b4", &ui_flex_b4_doc, 0.02 },
};

#define CASE_COUNT ((int)(sizeof(k_cases) / sizeof(k_cases[0])))

/* ---------- 基准图格式：极简 RLE（无外部依赖） ----------
 *
 * UI 图基本都是大片纯色，逐像素存下来 27 页要 8 MB，压一下就 100 KB 以内。
 * 格式（小端）：
 *     "QLG1"  u16 w  u16 h  然后是一串 run：[u8 count][u8 r][u8 g][u8 b]
 *     count 1..255 表示"这个颜色连续 count 个像素"（跨行连续，无 padding）。
 * 这样既不需要 zlib，也不会有"换行填充"导致两个平台字节不同的问题。
 */
#define GOLDEN_MAGIC "QLG1"

static int golden_save(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 0;
    uint8_t hdr[8];
    memcpy(hdr, GOLDEN_MAGIC, 4);
    hdr[4] = (uint8_t)(w & 0xFF);
    hdr[5] = (uint8_t)((w >> 8) & 0xFF);
    hdr[6] = (uint8_t)(h & 0xFF);
    hdr[7] = (uint8_t)((h >> 8) & 0xFF);
    int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    const long total = (long)w * (long)h;
    long i = 0;
    while (ok && i < total) {
        const uint8_t *a = rgb + (size_t)i * 3u;
        long run = 1;
        while (i + run < total && run < 255 &&
               memcmp(rgb + (size_t)(i + run) * 3u, a, 3) == 0)
            run++;
        uint8_t rec[4] = { (uint8_t)run, a[0], a[1], a[2] };
        ok = fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
        i += run;
    }
    fclose(f);
    return ok;
}

/* 返回：1 读到，0 打不开，-1 格式/尺寸/内容不符 */
static int golden_load(const char *path, uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, GOLDEN_MAGIC, 4) != 0) {
        fclose(f);
        return -1;
    }
    const int fw = hdr[4] | (hdr[5] << 8);
    const int fh = hdr[6] | (hdr[7] << 8);
    if (fw != w || fh != h) {
        fclose(f);
        return -1;
    }
    const long total = (long)w * (long)h;
    long i = 0;
    int rc = 1;
    while (i < total) {
        uint8_t rec[4];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) {
            rc = -1;
            break;
        }
        if (i + rec[0] > total) {
            rc = -1;
            break;
        }
        for (int k = 0; k < rec[0]; k++) {
            uint8_t *p = rgb + (size_t)(i + k) * 3u;
            p[0] = rec[1];
            p[1] = rec[2];
            p[2] = rec[3];
        }
        i += rec[0];
    }
    fclose(f);
    return rc;
}

/* ---------- PPM(P6)：只用于人工查看（QLGL_GOLDEN_PPM=1） ---------- */

static int save_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const size_t n = fwrite(rgb, 1, (size_t)w * (size_t)h * 3u, f);
    fclose(f);
    return n == (size_t)w * (size_t)h * 3u;
}

/*
 * 基准图只保留"真正有内容的那块"。
 * 这些页的 #root 往往撑满整个视口（900 宽），但内容只占左上角一小块；
 * 整幅存下来 27 页要 8 MB，裁掉空白后不到 1 MB，而且回归信号一点没少
 * （裁掉的地方在两个版本里都是同一个纯色）。
 */
static QLRect content_rect(const QLUIRoot *ui, QLRect root)
{
    QLRect acc = ql_rect_make(0, 0, 0, 0);
    int any = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUINodeRT *n = &ui->nodes[i];
        if (n->style != NULL && n->style->display == QL_UI_DISPLAY_NONE)
            continue;
        QLRect b = ql_rect_union(ql_ui_node_rect(n), n->paint_box);
        if (ql_rect_is_empty(b))
            continue;
        acc = any ? ql_rect_union(acc, b) : b;
        any = 1;
    }
    if (!any)
        return root;

    /* 向外取整到整像素，再夹进根节点之内 */
    QLRect r = ql_rect_make((acc.x / 4) * 4 - 4, (acc.y / 4) * 4 - 4, acc.w + 16,
                            acc.h + 16);
    r = ql_rect_intersect(r, root);
    return ql_rect_is_empty(r) ? root : r;
}

/* 把 mem driver 的一块区域取成紧凑 RGB888（PPM 用，也方便与 GPU 的 RGBA 对齐比较） */
static void fetch_rgb(QLDispDriver *disp, QLRect crop, uint8_t *out)
{
    for (ql_coord_t y = 0; y < crop.h; y++) {
        for (ql_coord_t x = 0; x < crop.w; x++) {
            ql_color_t c = ql_disp_mem_pixel(disp, crop.x + x, crop.y + y);
            uint8_t *p = out + ((size_t)y * (size_t)crop.w + (size_t)x) * 3u;
            p[0] = QL_COLOR_GET_R(c);
            p[1] = QL_COLOR_GET_G(c);
            p[2] = QL_COLOR_GET_B(c);
        }
    }
}

/* 单通道差 > tol 的像素数占总数的比例 */
static double diff_ratio(const uint8_t *a, const uint8_t *b, int w, int h, int tol,
                         int *out_max)
{
    long bad = 0;
    int mx = 0;
    for (int i = 0; i < w * h; i++) {
        int m = 0;
        for (int k = 0; k < 3; k++) {
            int d = abs((int)a[i * 3 + k] - (int)b[i * 3 + k]);
            if (d > m)
                m = d;
        }
        if (m > mx)
            mx = m;
        if (m > tol)
            bad++;
    }
    if (out_max != NULL)
        *out_max = mx;
    return (double)bad / (double)(w * h);
}

QL_TEST_BEGIN("test_golden")

    const int update = (getenv("QLGL_GOLDEN_UPDATE") != NULL);
    const int exact = (getenv("QLGL_GOLDEN_EXACT") != NULL);
    int compared = 0, created = 0, missing = 0, blob_ok = 0;

    for (int ci = 0; ci < CASE_COUNT; ci++) {
        const GoldenCase *gc = &k_cases[ci];

        QLUIRoot *ui = ql_ui_build(gc->doc);
        QL_CHECK(ui != NULL);
        if (ui == NULL)
            continue;

        ql_ui_layout(ui, VIEW_W, VIEW_H);
        QLRect root = ql_ui_node_rect(&ui->nodes[0]);
        if (root.w <= 0 || root.h <= 0) {
            printf("  [FAIL] %s：根节点尺寸 %dx%d\n", gc->name, (int)root.w,
                   (int)root.h);
            ql_test_failures++;
            ql_ui_free(ui);
            continue;
        }

        /* ---------- 第一层：doc 数据的逐字节比对（精确、机器无关） ---------- */
        {
            const size_t need = ql_ui_doc_encode(gc->doc, NULL, 0);
            QL_CHECK(need > 0);
            if (need > 0) {
                uint8_t *blob = (uint8_t *)ql_mem_alloc(need);
                QL_CHECK(blob != NULL);
                if (blob != NULL) {
                    QL_CHECK_EQ(ql_ui_doc_encode(gc->doc, blob, need), need);

                    char bpath[512];
                    snprintf(bpath, sizeof(bpath), "%s/%s.qlui", QL_GL_GOLDEN_DIR,
                             gc->name);
                    if (update) {
                        FILE *bf = fopen(bpath, "wb");
                        QL_CHECK(bf != NULL);
                        if (bf != NULL) {
                            QL_CHECK_EQ(fwrite(blob, 1, need, bf), need);
                            fclose(bf);
                        }
                    } else {
                        FILE *bf = fopen(bpath, "rb");
                        if (bf == NULL) {
                            printf("  [缺基准] %s.qlui\n", gc->name);
                            missing++;
                        } else {
                            uint8_t *ref = (uint8_t *)ql_mem_alloc(need);
                            QL_CHECK(ref != NULL);
                            size_t got = 0;
                            if (ref != NULL)
                                got = fread(ref, 1, need, bf);
                            fclose(bf);
                            if (ref != NULL) {
                                if (got != need) {
                                    printf("  [FAIL] %s：blob 长度 %zu（基准 %zu）\n",
                                           gc->name, got, need);
                                    ql_test_failures++;
                                    ql_test_checks++;
                                } else if (memcmp(ref, blob, need) != 0) {
                                    /* 报第一个不同的字节偏移，够定位到字段了 */
                                    size_t k = 0;
                                    while (k < need && ref[k] == blob[k])
                                        k++;
                                    printf("  [FAIL] %s：blob 第 %zu 字节不同"
                                           "（基准 %02X，现在 %02X）\n",
                                           gc->name, k, ref[k], blob[k]);
                                    ql_test_failures++;
                                    ql_test_checks++;
                                } else {
                                    blob_ok++;
                                }
                                ql_mem_free(ref);
                            }
                        }
                    }
                    ql_mem_free(blob);
                }
            }
        }

        QLRect crop = content_rect(ui, root);
        const int w = (int)crop.w;
        const int h = (int)crop.h;

        QLDispDriver *disp =
            ql_disp_mem_create((ql_coord_t)w, (ql_coord_t)h, QL_COLOR_ARGB8888);
        QL_CHECK(disp != NULL);
        QLCanvas canvas;
        QL_CHECK_EQ(ql_canvas_from_disp(&canvas, disp), QL_OK);
        ql_disp_mem_clear(disp, 0x000000);
        ql_ui_render_canvas(ui, &canvas);

        uint8_t *rgb = (uint8_t *)ql_mem_alloc((size_t)w * (size_t)h * 3u);
        QL_CHECK(rgb != NULL);
        if (rgb == NULL) {
            ql_disp_mem_destroy(disp);
            ql_ui_free(ui);
            continue;
        }
        fetch_rgb(disp, crop, rgb);

        char path[512];
        snprintf(path, sizeof(path), "%s/%s.qlg", QL_GL_GOLDEN_DIR, gc->name);

        /* 想看具体画面时写出 PPM（不参与比对，也不进版本库的判据） */
        if (getenv("QLGL_GOLDEN_PPM") != NULL) {
            char pp[512];
            snprintf(pp, sizeof(pp), "/tmp/golden_%s.ppm", gc->name);
            (void)save_ppm(pp, rgb, w, h);
        }

        if (update) {
            if (golden_save(path, rgb, w, h)) {
                created++;
            } else {
                printf("  [FAIL] %s：写不了 %s\n", gc->name, path);
                ql_test_failures++;
            }
        } else {
            uint8_t *ref = (uint8_t *)ql_mem_alloc((size_t)w * (size_t)h * 3u);
            QL_CHECK(ref != NULL);
            if (ref != NULL) {
                int got = golden_load(path, ref, w, h);
                if (got == 0) {
                    printf("  [缺基准] %s（跑 QLGL_GOLDEN_UPDATE=1 生成）\n",
                           gc->name);
                    missing++;
                } else if (got < 0) {
                    printf("  [FAIL] %s：%s 格式或尺寸不符（%dx%d）\n", gc->name, path,
                           w, h);
                    ql_test_failures++;
                } else {
                    int mx = 0;
                    double r = exact ? diff_ratio(ref, rgb, w, h, 0, &mx)
                                     : diff_ratio(ref, rgb, w, h, 12, &mx);
                    compared++;
                    if (r > 0.005) {
                        printf("  [FAIL] %s：%.2f%% 像素超差（最大单通道差 %d）\n",
                               gc->name, r * 100.0, mx);
                        ql_test_failures++;
                        ql_test_checks++;
                    }
                }
                ql_mem_free(ref);
            }
        }

#if defined(QLGL_ENABLE_GL)
        /* 顺带：CPU vs GPU（全部用例）。没有 EGL 时整段 SKIP */
        {
            QLGfxGL *gfx = ql_gfx_gl_create(gc->name, w, h, QL_GFX_GL_OFFSCREEN);
            if (gfx == NULL) {
                /* 只在第一个用例上报一次，避免刷屏 */
                if (ci == 0)
                    QL_TEST_GL_SKIP("CPU vs GPU：无法创建 GL 上下文");
            } else {
                ql_gfx_gl_begin(gfx, 0x000000);
                ql_ui_render(ui, ql_gfx_gl_draw(gfx));
                ql_gfx_gl_end(gfx);
                ql_gfx_gl_finish(gfx);

                uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)w * (size_t)h * 4u);
                QL_CHECK(rgba != NULL);
                if (rgba != NULL &&
                    ql_gfx_gl_read_pixels(gfx, rgba, (uint32_t)w * 4u) == QL_OK) {
                    long bad = 0;
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            const size_t px = (size_t)y * (size_t)w + (size_t)x;
                            int m = 0;
                            for (size_t k = 0; k < 3u; k++) {
                                int d = abs((int)rgb[px * 3u + k] -
                                            (int)rgba[px * 4u + k]);
                                if (d > m)
                                    m = d;
                            }
                            if (m > 32)
                                bad++;
                        }
                    }
                    double r = (double)bad / (double)(w * h);
                    printf("  CPU vs GPU  %-10s %.2f%%\n", gc->name, r * 100.0);
                    if (r > gc->gpu_tol) {
                        printf("  [FAIL] %s：CPU vs GPU %.2f%% 超差（上限 %.1f%%）\n",
                               gc->name, r * 100.0, gc->gpu_tol * 100.0);
                        ql_test_failures++;
                        ql_test_checks++;
                    }
                }
                ql_mem_free(rgba);
                ql_gfx_gl_destroy(gfx);
            }
        }
#endif

        ql_mem_free(rgb);
        ql_disp_mem_destroy(disp);
        ql_ui_free(ui);
    }

    printf("  %d 页：blob 一致 %d，图像比对 %d，新建 %d，缺基准 %d\n", CASE_COUNT,
           blob_ok, compared, created, missing);
    /* 缺基准不算失败（首次拉代码还没生成时），但要醒目 */
    QL_CHECK(missing == 0 || update);

QL_TEST_END()
