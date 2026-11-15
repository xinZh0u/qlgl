/**
 * @file    main.c
 * @brief   液态玻璃圆盘：复刻 codepen.io/Mikhail-Bespalov/pen/MYwrMNy。
 *
 *   ./build/example/example_38_liquid_glass --image 照片.jpg            # 打开窗口
 *   ./build/example/example_38_liquid_glass --image 照片.jpg --size 900x600
 *   ./build/example/example_38_liquid_glass --image 照片.jpg --dump /tmp/g.ppm
 *
 * 背景不再是程序生成的图：`--image <path>` 把任意图片（PNG / BMP / QOI，构建里
 * 带 JPEG 后端时也支持 JPEG）解码后注册成资源 `photo`。
 *
 * **高分辨率壁纸**（4K、手机竖屏 2700×4800 等）也直接支持：解码时临时放开像素
 * 预算，然后**按窗口需要的尺寸降采样**（box filter），只保留约 2× 窗口宽的像素。
 * 图片显示得比窗口高，**滚轮上下滚动**它，玻璃固定在中间 —— 同一块玻璃会折过
 * 照片的不同部分，折射/模糊对不对一眼就能看出来。
 *
 * 参考稿的核心是把 `backdrop-filter` 交给一个 SVG 滤镜（`feGaussianBlur` 轻度模糊 +
 * `feDisplacementMap` 按法线图位移）。本库把这两步做成两个 CSS 函数：
 *
 *   `blur(<N>px)`      高斯模糊。**半径要小**：模糊只是"看不清细节"，
 *                      一开大，玻璃后面就成了一团均值色，越糊越像塑料。
 *   `refract(<N>px)`   按参考稿那张法线图做位移：位移随半径增长、轮廓处最大，
 *                      方向**朝圆心**（内部画面被压进边缘一圈）。300px 的盘
 *                      取 ≈143px（参考稿 0.476×盒子尺寸）；没有它，玻璃就只是
 *                      一块贴在背景上的半透明圆片。
 *
 * 另外两样是"像玻璃"的廉价部分：8% 白底 + 一圈柔和的亮边（box-shadow：
 * 1px 实心 + 3px 弥散，而不是参考稿那圈硬 2px）+ 一层很淡的投影。
 * 它们都是纯绘制，不额外增加模糊次数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 720
#define H 460

/* 4K / 竖屏壁纸的整幅 RGBA 可能到 50MB+：只在这段时间放开预算 */
#define PHOTO_DECODE_BUDGET (256u * 1024u * 1024u)

static QLUINodeRT *s_hint;
static QLUINodeRT *s_wrap;      /* #photo-wrap：滚轮滚它 */
static int         s_count;

/* 背景：降采样后的 RGBA（owned，退出时释放） */
static QLImage     s_photo;

/* `ql_ui_set_text()` 不复制字符串：缓冲必须一直有效 */
static char s_buf[64];

/* 整个文件读进内存；失败返回 NULL（图片可能很大，不能用固定缓冲） */
static uint8_t *read_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

/* 目标分辨率：宽度最多 2×窗口（夹在 1280~2048），总像素不超过 6M */
static void photo_target(const QLImage *src, int win_w, int *out_w, int *out_h)
{
    int tw = src->w, th = src->h;
    int max_w = win_w * 2;
    if (max_w < 1280)
        max_w = 1280;
    if (max_w > 2048)
        max_w = 2048;
    const int64_t max_px = 6ll * 1024 * 1024;

    if (tw > max_w) {
        th = (int)((int64_t)src->h * max_w / src->w);
        tw = max_w;
    }
    if ((int64_t)tw * th > max_px) {
        th = (int)(max_px / tw);
        if (th < 1)
            th = 1;
        tw = (int)((int64_t)src->w * th / src->h);
        if (tw < 1)
            tw = 1;
    }
    *out_w = tw;
    *out_h = th;
}

/* 面积平均（box filter）降采样 RGBA8888 → 新分配一块更小的缓冲 */
static uint8_t *downscale_rgba(const QLImage *src, int tw, int th)
{
    uint8_t *dst = (uint8_t *)ql_mem_alloc((size_t)tw * (size_t)th * 4u);
    if (dst == NULL)
        return NULL;
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((int64_t)y * src->h / th);
        int sy1 = (int)((int64_t)(y + 1) * src->h / th);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((int64_t)x * src->w / tw);
            int sx1 = (int)((int64_t)(x + 1) * src->w / tw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            uint32_t acc[4] = { 0, 0, 0, 0 };
            uint32_t n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row =
                    src->px + (size_t)sy * src->stride + (size_t)sx0 * 4u;
                for (int sx = sx0; sx < sx1; sx++, row += 4) {
                    acc[0] += row[0];
                    acc[1] += row[1];
                    acc[2] += row[2];
                    acc[3] += row[3];
                    n++;
                }
            }
            uint8_t *o = dst + ((size_t)y * (size_t)tw + (size_t)x) * 4u;
            o[0] = (uint8_t)(acc[0] / n);
            o[1] = (uint8_t)(acc[1] / n);
            o[2] = (uint8_t)(acc[2] / n);
            o[3] = (uint8_t)(acc[3] / n);
        }
    }
    return dst;
}

/*
 * 读入背景图：解码（按需放开预算）→ 降采样到显示需要的尺寸 → 释放原始像素。
 *
 * 失败最常见的原因：文件超过 32MB，或像素数超过 `QL_CFG_IMAGE_MAX_PIXELS`
 * （默认 4096×4096）。
 */
static int load_photo(const char *path, int win_w)
{
    size_t flen = 0;
    uint8_t *file = read_all(path, &flen);
    if (file == NULL) {
        printf("读不了图片：%s\n", path);
        return 0;
    }

    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = file;
    req.len = flen;
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = QL_IMAGE_RGBA8888;

    ql_image_set_decode_budget(PHOTO_DECODE_BUDGET);
    QLImage full;
    const int rc = ql_image_decode(&req, &full);
    ql_image_set_decode_budget(0);     /* 解完立刻恢复默认 */
    free(file);

    if (rc != QL_OK) {
        printf("图片解码失败：%s（%zu 字节）\n", path, flen);
        printf("  · 支持 PNG / BMP / QOI；带平台后端（QLGL_BUILD_JPEG）时也支持 JPEG\n");
        printf("  · 输入文件上限 32MB；像素上限 4096×4096\n");
        return 0;
    }

    int tw = 0, th = 0;
    photo_target(&full, win_w, &tw, &th);
    uint8_t *small = downscale_rgba(&full, tw, th);
    if (full.owned)
        ql_mem_free((void *)full.px);
    if (small == NULL) {
        printf("背景图降采样失败（内存不足）\n");
        return 0;
    }

    memset(&s_photo, 0, sizeof(s_photo));
    s_photo.px = small;
    s_photo.w = (uint16_t)tw;
    s_photo.h = (uint16_t)th;
    s_photo.stride = (uint16_t)(tw * 4);
    s_photo.len = (uint32_t)s_photo.stride * s_photo.h;
    s_photo.format = QL_IMAGE_RGBA8888;
    s_photo.owned = 1;

    printf("背景图：%s（%dx%d → 显示用 %dx%d）\n", path, (int)full.w, (int)full.h,
           tw, th);
    return 1;
}

/* 把 CSS 里的占位符全部换成实际值（注释里也写了它，所以不能只换第一处） */
static char *subst(const char *src, const char *token, const char *value)
{
    const size_t tl = strlen(token);
    const size_t vl = strlen(value);
    size_t n = 0;
    for (const char *p = src; (p = strstr(p, token)) != NULL; p += tl)
        n++;
    char *out = (char *)malloc(strlen(src) + n * vl + 1u);
    if (out == NULL)
        return NULL;
    char *o = out;
    const char *p = src;
    for (const char *at = strstr(p, token); at != NULL; at = strstr(p, token)) {
        const size_t head = (size_t)(at - p);
        memcpy(o, p, head);
        o += head;
        memcpy(o, value, vl);
        o += vl;
        p = at + tl;
    }
    strcpy(o, p);
    return out;
}

/*
 * 载入界面：`__PHOTO_H__` 换成"宽度铺满、按图片比例算出的高度"，并保证比窗口高
 * （滚得动）。照片用 `object-fit: cover`，不会变形。
 */
static QLUIDoc *load_doc(int win_w, int win_h)
{
    char hp[1024], cp[1024];
    snprintf(hp, sizeof(hp), "%s/index.html", QL_EX_DIR);
    snprintf(cp, sizeof(cp), "%s/style.css", QL_EX_DIR);
    char *html = ql_file_read_text(hp);
    char *css = ql_file_read_text(cp);
    if (html == NULL || css == NULL) {
        printf("读不了界面文件：%s / %s\n", hp, cp);
        ql_mem_free(html);
        ql_mem_free(css);
        return NULL;
    }

    int photo_h = (int)((double)win_w * (double)s_photo.h / (double)s_photo.w + 0.5);
    const int min_h = (int)((double)win_h * 1.6 + 0.5);
    if (photo_h < min_h)
        photo_h = min_h;            /* 至少留 60% 的滚动余量 */

    char val[32];
    snprintf(val, sizeof(val), "%dpx", photo_h);
    char *css2 = subst(css, "__PHOTO_H__", val);
    QLUIDoc *doc = (css2 != NULL) ? ql_ui_load_text(html, css2) : NULL;

    ql_mem_free(html);
    ql_mem_free(css);
    free(css2);
    return doc;
}

/*
 * 滚轮：背景是"比窗口高的图片"，滚的是外层容器。
 *
 * 自己拦而不是让界面层命中测试去滚：玻璃/舞台铺满整屏，滚轮会先命中它们，
 * 命中链里没有可滚动祖先 —— 照片就永远滚不动。这里直接对确定的容器调用
 * `ql_ui_scroll_by()`（它内部会重排并产出脏区），与 UI 自己的滚轮语义一致。
 */
static int on_event(QLUIRoot *ui, const QLInputEvent *ev, void *user)
{
    (void)user;
    if (ev->type == QL_INPUT_WHEEL && ev->dy != 0 && s_wrap != NULL) {
        (void)ql_ui_scroll_by(ui, s_wrap, 0,
                              (ql_coord_t)ev->dy * QL_CFG_UI_WHEEL_STEP_PX);
        return 1;               /* 已消费：别再交给界面层 */
    }
    return 0;
}

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    if (node->src == NULL || node->src->tag != QL_UI_TAG_DIV)
        return;
    s_count++;
    snprintf(s_buf, sizeof(s_buf), "第 %d 次点击 · 滚轮可以上下滚动背景", s_count);
    ql_ui_set_text(ui, s_hint, s_buf);
    printf("%s\n", s_buf);
}

static void usage(const char *argv0)
{
    printf("用法：%s --image <图片路径> [--size 720x460] [--dump out.ppm] [--frames N]\n"
           "\n"
           "  --image PATH   背景图（PNG / BMP / QOI；带平台后端时也支持 JPEG）\n"
           "  --size WxH     窗口尺寸（默认 %dx%d）\n"
           "  --scroll N     先把背景滚 N px（截图/验证用）\n"
           "  滚轮            上下滚动背景照片\n",
           argv0, W, H);
}

int main(int argc, char **argv)
{
    const char *image = NULL;
    int win_w = W, win_h = H;
    int start_scroll = 0;          /* --scroll N：先把背景滚 N px（截图/验证用） */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                win_w = w;
                win_h = h;
            }
        } else if (strcmp(argv[i], "--scroll") == 0 && i + 1 < argc) {
            start_scroll = (int)strtol(argv[++i], NULL, 10);
        } else if (argv[i][0] != '-' && image == NULL) {
            image = argv[i];       /* 也接受直接给一个路径 */
        }
    }

    /* 没给路径时试一下例程目录里的 background.png / background.jpg */
    char def[1024];
    if (image == NULL) {
        const char *cands[] = { "background.png", "background.jpg" };
        for (size_t ci = 0; ci < sizeof(cands) / sizeof(cands[0]); ci++) {
            snprintf(def, sizeof(def), "%s/%s", QL_EX_DIR, cands[ci]);
            FILE *f = fopen(def, "rb");
            if (f != NULL) {
                fclose(f);
                image = def;
                break;
            }
        }
    }
    if (image == NULL) {
        usage(argv[0]);
        return 2;
    }

    if (qlgl_init() != QL_OK)
        return 1;
#if defined(QL_FEAT_IMG_LIBJPEG) && QL_FEAT_IMG_LIBJPEG
    /* JPEG 只能靠平台后端：库编进来了，还要应用把后端挂上去 */
    ql_image_set_platform_backend(ql_image_backend_libjpeg());
#endif

    if (!load_photo(image, win_w)) {
        qlgl_shutdown();
        return 1;
    }

    QLUIDoc *doc = load_doc(win_w, win_h);
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        ql_mem_free((void *)s_photo.px);
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        ql_mem_free((void *)s_photo.px);
        qlgl_shutdown();
        return 1;
    }

    /* 已经降采样好的 RGBA 交给资源表（名字与 <img src="photo"> 对应） */
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "photo";
    in.data = s_photo.px;
    in.len = s_photo.len;
    in.w = s_photo.w;
    in.h = s_photo.h;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = s_photo.format;

    /* 可滚动容器 = <img> 的父节点 */
    QLUINodeRT *img = ql_ui_find_by_tag(ui, QL_UI_TAG_IMG, 0);
    s_wrap = (img != NULL) ? img->parent : NULL;

    /* RAW 注册也走解码预算检查（放大到 6M 像素的缓冲可能 >16MB） */
    ql_image_set_decode_budget(PHOTO_DECODE_BUDGET);
    const QLErr irc = ql_ui_register_image(ui, &in);
    ql_image_set_decode_budget(0);
    if (irc != QL_OK)
        printf("（提示）背景图没注册上（err=%d），只显示底色\n", (int)irc);

    QLWindow *win = ql_window_create("QLGL · 38 liquid glass", win_w, win_h,
                                     QL_WINDOW_GL);
    if (win == NULL) {
        printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        ql_mem_free((void *)s_photo.px);
        qlgl_shutdown();
        return 1;
    }
    ql_window_bind_ui(win, ui);

    if (s_wrap != NULL && start_scroll != 0)
        (void)ql_ui_scroll_by(ui, s_wrap, 0, (ql_coord_t)start_scroll);

    /* 状态行是最后一个节点（`.hint` 里的文本节点） */
    s_hint = &ui->nodes[ui->node_count - 1];

    ql_ui_set_click_cb(ui, on_click, NULL);
    ql_window_set_event_filter(win, on_event, NULL);

    const int rc = ql_window_run_args(win, argc, argv);

    ql_window_destroy(win);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    ql_mem_free((void *)s_photo.px);   /* RAW 注册不复制，由我们自己释放 */
    qlgl_shutdown();
    return rc;
}
