/**
 * @file    common.h
 * @brief   例程共用的几件小事：读文件、挑字体、跑主循环。
 *
 * 每个例程只讲自己那一件事，样板代码都在这里，避免读者被噪音干扰。
 */
#ifndef QL_EXAMPLE_COMMON_H
#define QL_EXAMPLE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <locale.h>

#include <qlgl/qlgl.h>

#if !defined(QLGL_ENABLE_GL)
#define HAS_GL 0
#else
#define HAS_GL 1
#endif

/* 关掉 FreeType 时例程照样能编（只是只有内置位图字体，中文显示不出来） */
#if defined(QLGL_ENABLE_FREETYPE)
#include <qlgl/qlfont_ft.h>
#define HAS_FT 1
#else
#define HAS_FT 0
#endif

/*
 * ---- 例程自己的 HTML / CSS ----
 *
 * 每个例程是一个文件夹：`example/01_window/{main.c,index.html,style.css}`。
 * HTML/CSS 是**运行期从文件读**的，不是编进二进制的字符串 —— 改一行 CSS 重跑
 * 就能看到效果，也能直接喂给 `qltext cc --html ... --css ...` 做构建期编译。
 *
 * `QL_EX_DIR` 由 CMake 注入成该例程目录的绝对路径，所以从任何目录启动都能找到。
 * 直接 `gcc example/01_window/main.c` 时没有这个宏，退回到相对当前目录。
 */
#ifndef QL_EX_DIR
#define QL_EX_DIR "example"
#endif

static const char *ex_dir(void)
{
    /* 想让例程读别处的文件：QLGL_EX_DIR=/path/to/dir ./example_01_window */
    const char *env = getenv("QLGL_EX_DIR");
    return (env != NULL && env[0] != '\0') ? env : QL_EX_DIR;
}

/* 读整个文件到堆上（补结尾 '\0'）。失败返回 NULL 并打印原因 */
static char *ex_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("打不开 %s（例程目录可用 QLGL_EX_DIR 指定）\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/*
 * 相对名 → 真路径。规则只有两条：
 *   - 绝对路径原样用；
 *   - 否则**当前目录下有的优先**（临时改一份 CSS 试效果最顺手），
 *     没有就落到例程目录里。
 */
static int ex_resolve(char *out, size_t n, const char *rel)
{
    if (rel[0] == '/')
        return snprintf(out, n, "%s", rel) > 0;
    FILE *f = fopen(rel, "rb");
    if (f != NULL) {
        fclose(f);
        return snprintf(out, n, "%s", rel) > 0;
    }
    return snprintf(out, n, "%s/%s", ex_dir(), rel) > 0;
}

/* 编译一份界面：`ex_load_ui("index.html", "style.css")` */
static QLUIDoc *ex_load_ui(const char *html, const char *css)
{
    char hp[1024], cp[1024];
    ex_resolve(hp, sizeof(hp), html);
    ex_resolve(cp, sizeof(cp), css);
    char *h = ex_slurp(hp);
    char *c = (h != NULL) ? ex_slurp(cp) : NULL;
    if (h == NULL || c == NULL) {
        free(h);
        free(c);
        return NULL;
    }
    printf("读入 %s + %s\n", hp, cp);
    QLUIDoc *doc = ql_ui_load_text(h, c);
    free(h);
    free(c);
    return doc;
}

/* 找一个能显示中文的字体；找不到就退回内置位图字体（只有 ASCII） */
/*
 * 进程的 locale 影响 XIM：`XOpenIM` 靠它挑输入法（fcitx/ibus）。
 * 不调这一句时中文通常打不进来 —— 而这不是库能代劳的（改全局状态是应用的事）。
 */
static void ex_setup_locale(void)
{
    setlocale(LC_ALL, "");
}

static void ex_load_font(void)
{
#if !HAS_FT
    return;                          /* 没有 FreeType 后端：只有内置位图字体 */
#else
    const char *env = getenv("QLGL_FONT");
    if (env != NULL && ql_font_set_default_file(env, -1))
        return;
    const char *cands[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (ql_font_set_default_file(cands[i], -1))
            return;
#endif
}

/*
 * 把界面渲染成一帧 PPM（没有窗口/没有 GPU 也能验证结果，也方便文档配图）。
 */
static ql_color_t ex_clear_color(QLUIRoot *ui);

static int ex_dump_ppm(QLUIRoot *ui, int w, int h, const char *path)
{
    QLDispDriver *disp = ql_disp_mem_create((ql_coord_t)w, (ql_coord_t)h,
                                            QL_COLOR_RGB888);
    if (disp == NULL)
        return 0;
    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK) {
        ql_disp_mem_destroy(disp);
        return 0;
    }
    /* 清除色 = 根元素背景色（与真实窗口一致）：否则没被页面盖住的区域是黑块 ——
     * 深色主题看不出来，换成白底立刻露馅（也违反了"例程统一白底"的约定）。 */
    ql_canvas_clear(&canvas, ex_clear_color(ui));
    ql_ui_render_canvas(ui, &canvas);
    const int ok = (ql_disp_mem_save_ppm(disp, path) == QL_OK);
    ql_disp_mem_destroy(disp);
    return ok;
}

/*
 * 清除色 = 根元素的背景色。
 *
 * 它有两个作用：一是窗口拉大时 X 服务器立刻用这个色填新露出的区域
 * （`ql_gfx_gl_begin` 会把它同步成窗口背景像素），二是万一一帧没画完，
 * 露出来的是页面底色而不是黑块。写死一个颜色的话，换主题就会闪错色。
 */
static ql_color_t ex_clear_color(QLUIRoot *ui)
{
    const QLUIStyle *s = (ui != NULL && ui->node_count > 0)
                             ? ql_ui_node_style(&ui->nodes[0])
                             : NULL;
    if (s != NULL && s->bg_alpha > 0)
        return s->bg_color;
    return QL_COLOR_RGB(0xFF, 0xFF, 0xFF);
}

/*
 * 打开一个真实窗口跑主循环（需要 -DQLGL_BUILD_GL=ON 且有 EGL/X11）。
 * `--frames N` 跑 N 帧后退出，`--dump out.ppm` 存最后一帧；
 * 只给 `--dump` 不给 `--frames` 时按 1 帧算（截图模式，不会卡住等关窗口）；
 * 没有窗口时自动退回离屏渲染 + PPM，所以这个函数在没有显示器的机器上也能跑通。
 */
/* 帧时间样本（`--stats` 用）：环形记录，退出时打百分位 */
#define EX_FRAME_SAMPLES 4096
static uint32_t s_frame_us[EX_FRAME_SAMPLES];
static uint32_t s_swap_us[EX_FRAME_SAMPLES];
static uint32_t s_gap_us[EX_FRAME_SAMPLES];   /* 相邻两帧的墙钟间隔 */
static int      s_frame_n;

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void ex_print_stats(void)
{
    if (s_frame_n <= 0)
        return;
    qsort(s_frame_us, (size_t)s_frame_n, sizeof(uint32_t), cmp_u32);
    const uint32_t p50 = s_frame_us[s_frame_n / 2];
    const uint32_t p95 = s_frame_us[(int)((double)s_frame_n * 0.95)];
    const uint32_t mx = s_frame_us[s_frame_n - 1];
    /*
     * 两个口径分开打：帧内耗时（begin→end）与相邻帧间隔。
     * 事件驱动的例程只在有脏区时画，"帧内耗时"看着像上千 fps 很正常 ——
     * 真正的帧率要看间隔；--spin 时两者才基本相等。
     */
    int gap_n = 0;
    for (int i = 1; i < s_frame_n; i++)   /* 第一帧没有"上一帧"，丢掉 */
        s_gap_us[gap_n++] = s_gap_us[i];
    uint32_t gap_p50 = 0;
    if (gap_n > 0) {
        qsort(s_gap_us, (size_t)gap_n, sizeof(uint32_t), cmp_u32);
        gap_p50 = s_gap_us[gap_n / 2];
    }
    printf("帧时间：p50 %.2fms  p95 %.2fms  max %.2fms  （%d 帧）\n", p50 / 1000.0,
           p95 / 1000.0, mx / 1000.0, s_frame_n);
    if (gap_n > 0) {
        printf("帧间隔：p50 %.2fms（≈ %.0f fps）  max %.2fms\n", gap_p50 / 1000.0,
               gap_p50 > 0 ? 1000000.0 / (double)gap_p50 : 0.0,
               s_gap_us[gap_n - 1] / 1000.0);
    } else {
        printf("帧间隔：采样太少\n");
    }
    /* 等 vsync 的时间单独看：它大而 frame 也大 → 不是我们画得慢 */
    qsort(s_swap_us, (size_t)s_frame_n, sizeof(uint32_t), cmp_u32);
    printf("其中等 swap：p50 %.2fms  max %.2fms\n", s_swap_us[s_frame_n / 2] / 1000.0,
           s_swap_us[s_frame_n - 1] / 1000.0);
    s_frame_n = 0;
}

static int ex_run(QLUIRoot *ui, int w, int h, const char *title, int argc,
                  char **argv)
{
    long max_frames = 0;
    int want_stats = 0;
    int want_spin = 0;   /* 每帧都渲染（基准测试用）；默认事件驱动 */
    int run_seconds = 0; /* --seconds N：跑 N 秒后退出并打统计 */
    int want_vsync = -1; /* --vsync 0|1；-1 = 用后端默认（开） */
    const char *dump = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0)
            want_stats = 1;
        else if (strcmp(argv[i], "--spin") == 0)
            want_spin = 1;
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            run_seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--vsync") == 0 && i + 1 < argc)
            want_vsync = atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            /* `--size 600x700`：换窗口尺寸（顺便看页面是不是真自适应） */
            int sw = 0, sh = 0;
            if (sscanf(argv[++i], "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) {
                w = sw;
                h = sh;
            }
        }
    }
    /* 截图模式下别等着人关窗口 */
    if (dump != NULL && max_frames == 0)
        max_frames = 1;
    /*
     * `--frames N` 的意思是"渲染 N 帧后退出"。事件驱动的循环在界面静止时
     * 根本不渲染，所以给了 --frames 就自动切成 spin（否则永远等不到 N 帧）。
     */
    if (max_frames > 0)
        want_spin = 1;

#if HAS_GL
    QLGfxGL *gfx = ql_gfx_gl_create(title, w, h, QL_GFX_GL_WINDOW);
    if (gfx != NULL) {
        ql_ui_layout(ui, w, h);
        /*
         * **必须接上输入汇**：平台层只负责把 X11 事件塞进队列，
         * `ql_input_dispatch()` 之后要靠这个 sink 才会送到界面层。
         * 忘了这一句的表现是"窗口能开、能滚、就是点不动也打不了字"。
         */
        ql_ui_attach_input(ui);
        /* Ctrl+C / Ctrl+V 走 X11 剪贴板（协议是异步的，内容下一帧才到） */
        ql_gfx_gl_attach_clipboard(gfx, ui);
        /* 输入法组字文本交给我们画（不接的话拼音只出现在候选窗里） */
        ql_gfx_gl_attach_ime(gfx, ui);
        QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
        /*
         * 保留式画布 + 脏区重绘：静止的界面一个像素都不重画。
         * 建不出来（GL 能力不足）也没关系，begin_rects 会退回整屏。
         */
        (void)ql_gfx_gl_set_retained(gfx, 1);
        if (want_vsync >= 0)
            ql_gfx_gl_set_vsync(gfx, want_vsync);

        long frame = 0;
        int need_full = 1;      /* 首帧 / 改尺寸后必须整屏重画 */
        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        while (ql_gfx_gl_poll(gfx)) {
            if (run_seconds > 0) {
                struct timespec tn;
                clock_gettime(CLOCK_MONOTONIC, &tn);
                if (tn.tv_sec - t_start.tv_sec >= run_seconds)
                    break;
            }
            /*
             * 窗口被拖动缩放：重新按新尺寸布局。
             * 后端只报告"画布变了"，重排是界面层的事。
             */
            if (ql_gfx_gl_resize_pending(gfx)) {
                ql_coord_t cw = w, ch = h;
                ql_gfx_gl_size(gfx, &cw, &ch);
                if (cw > 0 && ch > 0) {
                    ql_ui_layout(ui, cw, ch);
                    /* 记下真实尺寸：`--dump` 和 QL_EX_WINDOW_DUMP 都按它出图 */
                    w = cw;
                    h = ch;
                    need_full = 1;
                }
            }
            ql_input_dispatch();
            /*
             * 把"光标在哪儿"告诉输入法（候选窗靠它定位）。必须在
             * ql_input_dispatch() **之后** 调：这一轮可能刚移了光标/换了焦点。
             * 值没变时后端直接返回，不会每个事件都发一次 X 请求。
             */
            {
                ql_coord_t sx = 0, sy = 0, sh = 0;
                if (ql_ui_ime_spot(ui, &sx, &sy, &sh))
                    ql_gfx_gl_set_ime_spot(gfx, sx, sy, sh);
            }
            const uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

            QLRect areas[16];
            int n = ql_ui_consume_dirty(ui, areas, 16);

            /*
             * 没有脏区、没有动画、没改尺寸、也不是 spin 模式 → 什么都不做，
             * 阻塞等事件。这是"界面静止时零占用、拖动窗口不掉帧"的关键：
             * 之前每帧都 swap 一次（144Hz），合成器在缩放时还要处理这些多余的帧。
             */
            if (!want_spin && !need_full && n == 0 && moving == 0) {
                if (!ql_gfx_gl_wait(gfx, 32))
                    break;
                continue;
            }

            const int full = ql_gfx_gl_begin_rects(gfx, ex_clear_color(ui), areas, n);
            if (full || want_spin || n <= 0) {
                ql_ui_render(ui, d2d);
            } else {
                for (int i = 0; i < n; i++)
                    ql_ui_render_rect(ui, d2d, areas[i]);
            }
            /*
             * 调试用：QL_EX_WINDOW_DUMP=/tmp/x.ppm 把窗口里**真正渲染出来的**那一帧
             * 抓下来（离屏 --dump 走的是 CPU 路径，看不到窗口特有的问题）。
             *
             * 时机：`ql_gfx_gl_flush_frame()` 之后、`ql_gfx_gl_end()` 之前。
             * 早了顶点批次还没上传（只读到清除色），晚了已经 eglSwapBuffers ——
             * 换过去之后后缓冲内容未定义（Wayland 上读出来全黑）。两个坑都踩过。
             */
            {
                const char *wd = getenv("QL_EX_WINDOW_DUMP");
                if (wd != NULL && wd[0] != '\0' && frame == 0) {
                    ql_coord_t gw = 0, gh = 0;
                    ql_gfx_gl_device_size(gfx, &gw, &gh);  /* 设备像素 = CSS × scale */
                    ql_gfx_gl_flush_frame(gfx);
                    ql_gfx_gl_finish(gfx);
                    uint8_t *px = (uint8_t *)calloc(1, (size_t)gw * (size_t)gh * 4u);
                    if (px != NULL) {
                        if (ql_gfx_gl_read_pixels(gfx, px, (uint32_t)gw * 4u) == QL_OK) {
                            FILE *fp = fopen(wd, "wb");
                            if (fp != NULL) {
                                fprintf(fp, "P6\n%d %d\n255\n", (int)gw, (int)gh);
                                for (ql_coord_t yy = 0; yy < gh; yy++)
                                    for (ql_coord_t xx = 0; xx < gw; xx++) {
                                        const uint8_t *q =
                                            px + ((size_t)yy * (size_t)gw + (size_t)xx) * 4u;
                                        fwrite(q, 1, 3, fp);
                                    }
                                fclose(fp);
                                printf("已抓下窗口帧 %s（绘制 %dx%d，窗口 %dx%d，"
                                       "布局 %dx%d）\n", wd, (int)gw, (int)gh, w, h,
                                       (int)ui->width, (int)ui->height);
                            }
                        }
                        free(px);
                    }
                }
            }
            ql_gfx_gl_end(gfx);
            need_full = 0;
            if (want_stats && s_frame_n < EX_FRAME_SAMPLES) {
                static struct timespec t_prev;
                struct timespec t_now;
                clock_gettime(CLOCK_MONOTONIC, &t_now);
                s_gap_us[s_frame_n] =
                    (s_frame_n == 0)
                        ? 0u
                        : (uint32_t)((t_now.tv_sec - t_prev.tv_sec) * 1000000 +
                                     (t_now.tv_nsec - t_prev.tv_nsec) / 1000);
                t_prev = t_now;
                s_frame_us[s_frame_n] = ql_gfx_gl_frame_us(gfx);
                s_swap_us[s_frame_n] = ql_gfx_gl_swap_us(gfx);
                s_frame_n++;
            }
            if (max_frames > 0 && ++frame >= max_frames)
                break;
        }
        if (want_stats)
            ex_print_stats();
        if (want_vsync == 0)
            ql_gfx_gl_set_vsync(gfx, 1);
        if (dump != NULL) {
            /* 窗口关掉之前先抓一帧 */
            if (ex_dump_ppm(ui, w, h, dump))
                printf("已写出 %s\n", dump);
        }
        ql_ui_detach_input();
        ql_gfx_gl_destroy(gfx);
        return 0;
    }
    printf("（没有可用窗口，改为离屏渲染）\n");
#endif

    ql_ui_layout(ui, w, h);
    if (dump != NULL) {
        if (ex_dump_ppm(ui, w, h, dump))
            printf("已写出 %s\n", dump);
        else
            printf("写出 %s 失败\n", dump);
    } else {
        printf("没有窗口也没给 --dump，什么也没做。\n");
    }
    return 0;
}

#endif /* QL_EXAMPLE_COMMON_H */
