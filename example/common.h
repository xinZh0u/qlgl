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
    ql_canvas_clear(&canvas, 0x000000);
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
static int ex_run(QLUIRoot *ui, int w, int h, const char *title, int argc,
                  char **argv)
{
    long max_frames = 0;
    const char *dump = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
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
        QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
        long frame = 0;
        while (ql_gfx_gl_poll(gfx)) {
            /*
             * 窗口被拖动缩放：重新按新尺寸布局。
             * 后端只报告"画布变了"，重排是界面层的事。
             */
            if (ql_gfx_gl_resize_pending(gfx)) {
                ql_coord_t cw = w, ch = h;
                ql_gfx_gl_size(gfx, &cw, &ch);
                if (cw > 0 && ch > 0)
                    ql_ui_layout(ui, cw, ch);
            }
            ql_input_dispatch();
            (void)ql_ui_anim_tick(ui, ql_anim_now_ms());
            ql_gfx_gl_begin(gfx, ex_clear_color(ui));
            ql_ui_render(ui, d2d);
            ql_gfx_gl_end(gfx);
            if (max_frames > 0 && ++frame >= max_frames)
                break;
        }
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
