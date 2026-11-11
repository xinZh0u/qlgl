/**
 * @file    main.c
 * @brief   液态玻璃圆盘：复刻 codepen.io/Mikhail-Bespalov/pen/MYwrMNy。
 *
 *   ./build/example/example_38_liquid_glass                    # 打开窗口
 *   ./build/example/example_38_liquid_glass --dump /tmp/g.ppm  # 抓一帧
 *
 * 参考稿的核心是把 `backdrop-filter` 交给一个 SVG 滤镜（`feGaussianBlur` 轻度模糊 +
 * `feDisplacementMap` 按法线图位移）。本库把这两步做成两个 CSS 函数：
 *
 *   `blur(<N>px)`      高斯模糊。**半径要小**：模糊只是"看不清细节"，
 *                      一开大，玻璃后面就成了一团均值色，越糊越像塑料。
 *   `refract(<N>px)`   轮廓附近把采样点往外推（透镜斜面），玻璃才有厚度。
 *                      没有它，玻璃就只是一块贴在背景上的半透明圆片。
 *   `saturate(<N>%)`   模糊会把颜色洗淡，这里推回去。
 *
 * 另外两样是"像玻璃"的廉价部分：8% 白底 + 一圈 2px 亮边 + 一层很淡的投影。
 * 它们都是纯绘制，不额外增加模糊次数。
 *
 * 背景是一张**现算的水彩图**（纸纹 + 色斑），不用外部文件：玻璃要靠背景里的
 * 细节才看得出来 —— 底下是纯色渐变的话，糊与不糊根本没区别。
 */
#include "common.h"

#include <math.h>

#define W 720
#define H 460

static QLUIRoot   *s_ui;
static QLUINodeRT *s_hint;
static int         s_count;

/* `ql_ui_set_text()` 不复制字符串：缓冲必须一直有效 */
static char s_buf[64];

/*
 * 水彩背景（RGB888）。尺寸受 `QL_CFG_IMAGE_MAX_BYTES`（512KB）限制，所以按
 * 512×328 生成、由 CSS 的 `background-size: cover` 放大到窗口大小。
 *
 * 里面有三层，缺一层玻璃就看不出效果：
 *   色斑    大块颜色，决定整体色调；
 *   花      十几个带硬边的小圆斑，**模糊会把这些边磨开、折射会把它们掰弯**；
 *   纸纹    ±10 个色阶的高频噪声，就是水彩纸的颗粒。
 */
#define IW 512
#define IH 328

static uint8_t s_bg[IW * IH * 3];

typedef struct {
    float   cx, cy, r;      /* 归一化圆心 + 半径 */
    uint8_t r8, g8, b8;     /* 颜色 */
    float   k;              /* 强度（0..1） */
} Blob;

typedef struct {
    float   x, y, r;        /* 像素坐标 + 半径 */
    uint8_t r8, g8, b8;
    float   k;
} Petal;

/* 一张"夏日水彩"：底色 + 色斑 + 花 + 纸纹。固定种子，两次跑出来必须一样。 */
static void build_watercolor(void)
{
    static const Blob blobs[] = {
        { 0.06f, 0.08f, 0.42f, 255, 148,  58, 1.00f },
        { 0.30f, 0.00f, 0.34f, 255, 196,  74, 0.90f },
        { 0.56f, 0.00f, 0.40f, 255, 108, 128, 0.95f },
        { 0.83f, 0.05f, 0.36f, 198,  78, 190, 0.85f },
        { 0.99f, 0.34f, 0.34f,  88, 120, 235, 0.80f },
        { 0.02f, 0.56f, 0.38f, 148,  88, 210, 0.85f },
        { 0.22f, 0.86f, 0.40f, 255, 188,  88, 0.90f },
        { 0.49f, 0.72f, 0.34f,  68, 204, 180, 0.85f },
        { 0.73f, 0.96f, 0.40f, 255, 118,  88, 0.85f },
        { 0.92f, 0.78f, 0.36f,  58, 170, 200, 0.70f },
        { 0.63f, 0.40f, 0.22f, 252, 242, 222, 0.50f },
    };
    /* 花：白/黄/粉的小圆斑，边缘只有 1.5px 的过渡 —— 正是模糊与折射的"试纸" */
    static const Petal petals[] = {
        {  62,  64, 26, 255, 244, 214, 0.85f }, {  96,  44, 16, 255, 214, 120, 0.80f },
        { 150,  96, 30, 255, 236, 196, 0.75f }, { 108, 120, 14, 250, 160, 120, 0.70f },
        { 214,  52, 22, 255, 246, 220, 0.80f }, { 262,  90, 34, 255, 220, 140, 0.70f },
        { 330,  60, 18, 255, 240, 200, 0.75f }, { 384,  96, 28, 252, 250, 238, 0.70f },
        { 436,  56, 20, 255, 226, 150, 0.75f }, { 470, 118, 30, 255, 240, 210, 0.65f },
        {  48, 200, 26, 255, 232, 190, 0.70f }, { 128, 240, 34, 255, 244, 214, 0.65f },
        { 200, 190, 16, 250, 170, 130, 0.70f }, { 250, 260, 24, 255, 236, 196, 0.70f },
        { 340, 210, 30, 252, 248, 236, 0.65f }, { 410, 250, 20, 255, 226, 150, 0.70f },
        { 470, 200, 26, 255, 240, 210, 0.65f }, { 300, 300, 30, 255, 232, 190, 0.60f },
    };
    const float aspect = (float)IH / (float)IW;

    /* 纸纹用整数 LCG：不依赖 libc 的 rand()，跨平台一致 */
    uint32_t seed = 12345u;
    for (int y = 0; y < IH; y++) {
        for (int x = 0; x < IW; x++) {
            const float fx = (float)x / (float)IW;
            const float fy = (float)y / (float)IH;
            float r = 248.0f - 16.0f * fy;
            float g = 234.0f - 28.0f * fy;
            float b = 208.0f - 24.0f * fy;

            for (size_t i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
                const Blob *o = &blobs[i];
                const float dx = fx - o->cx;
                const float dy = (fy - o->cy) * aspect;
                const float d = sqrtf(dx * dx + dy * dy) / o->r;
                float w = 1.0f - d;
                if (w <= 0.0f)
                    continue;
                w = w * w * (3.0f - 2.0f * w) * o->k;
                r = r * (1.0f - w) + (float)o->r8 * w;
                g = g * (1.0f - w) + (float)o->g8 * w;
                b = b * (1.0f - w) + (float)o->b8 * w;
            }

            for (size_t i = 0; i < sizeof(petals) / sizeof(petals[0]); i++) {
                const Petal *o = &petals[i];
                const float dx = (float)x - o->x;
                const float dy = (float)y - o->y;
                const float d = sqrtf(dx * dx + dy * dy);
                float w = (o->r - d) / 1.5f;          /* 1.5px 的软边 */
                if (w <= 0.0f)
                    continue;
                if (w > 1.0f)
                    w = 1.0f;
                w *= o->k;
                r = r * (1.0f - w) + (float)o->r8 * w;
                g = g * (1.0f - w) + (float)o->g8 * w;
                b = b * (1.0f - w) + (float)o->b8 * w;
            }

            /* 纸纹：高频噪声。模糊/折射要靠它才看得出来 */
            seed = seed * 1664525u + 1013904223u;
            const float n = (float)((seed >> 16) & 0xFFu) / 255.0f - 0.5f;

            uint8_t *p = s_bg + ((size_t)y * (size_t)IW + (size_t)x) * 3u;
            const float v[3] = { r, g, b };
            for (int c = 0; c < 3; c++) {
                const float t = v[c] + n * 20.0f;
                p[c] = (uint8_t)(t < 0.0f ? 0.0f : (t > 255.0f ? 255.0f : t));
            }
        }
    }
}

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)user;
    if (node->src == NULL || node->src->tag != QL_UI_TAG_DIV)
        return;
    s_count++;
    snprintf(s_buf, sizeof(s_buf), "第 %d 次点击 · 玻璃后面还是那张水彩", s_count);
    ql_ui_set_text(ui, s_hint, s_buf);
    printf("%s\n", s_buf);
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
    if (doc == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }
    s_ui = ui;

    ql_ui_layout(ui, W, H);

    /* 背景图：名字与 CSS 里的 `url(watercolor)` 对应（运行期注册，不带外部文件） */
    build_watercolor();
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = "watercolor";
    in.data = s_bg;
    in.len = sizeof(s_bg);
    in.w = IW;
    in.h = IH;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_RGB888;
    if (ql_ui_register_image(ui, &in) != QL_OK)
        printf("（提示）水彩背景没注册上，玻璃后面就只有底色\n");

    /* 状态行是最后一个节点（`.hint` 里的文本节点） */
    s_hint = &ui->nodes[ui->node_count - 1];

    ql_ui_set_click_cb(ui, on_click, NULL);

    const int rc = ex_run(ui, W, H, "QLGL · 38 liquid glass", argc, argv);
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return rc;
}
