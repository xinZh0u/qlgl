/**
 * @file    example_ui_blob.c
 * @brief   从 .qlui 加载界面，并支持"改 CSS → 窗口自动更新"的热重载。
 *
 * 这就是 blob 形态的实际价值：**换界面不用重新编译 C 代码**。
 *
 * 运行：
 *   ./build/example_ui_blob                      加载 build/ui/ui_demo.qlui，Esc 退出
 *   ./build/example_ui_blob --watch              监视文件 mtime，一变就重载
 *   ./build/example_ui_blob --frames 120         跑 120 帧后退出
 *   ./build/example_ui_blob --file x.qlui        指定文件
 *   ./build/example_ui_blob --mem --ppm a.ppm    无窗口时导出图片
 *
 * 配合热重载的开发流程（两个终端）：
 *   # 终端 1
 *   ./build/example_ui_blob --watch
 *   # 终端 2：改完 css 后
 *   python3 tools/qlui/qlui.py --html test/web/ui_demo.html \
 *                              --css test/web/ui_demo.css --out build/ui
 *   cmake --build build --target qlblob && ./build/qlblob build/ui/ui_demo.qlui
 *   # 窗口自动更新
 */
#include <stdio.h>
#include <string.h>

#include <qlgl/qlgl.h>

#include "../window_helper.h"

#if defined(__linux__)
#include <time.h>
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

#define WIN_W 320
#define WIN_H 320

#if !defined(QL_UI_BLOB_DEFAULT)
#define QL_UI_BLOB_DEFAULT "ui_demo.qlui"
#endif

/* ------------------------- 文件修改时间（热重载判定） ------------------------- */

static long file_mtime(const char *path)
{
#if defined(__linux__)
    struct timespec ts;
    /* 用 stat 需要 sys/stat.h；这里用 fopen+mtime 的替代：读文件头一个字节
     * 并配合 size，够判断"改过了"。真正产品里应该用 inotify。 */
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ts.tv_sec = 0;
    (void)ts;
    return size;
#else
    (void)path;
    return -1;
#endif
}

int main(int argc, char **argv)
{
    QLWin win;
    ql_win_parse_args(&win, argc, argv);

    const char *blob_path = QL_UI_BLOB_DEFAULT;
    int watch = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            blob_path = argv[++i];
        else if (strcmp(argv[i], "--watch") == 0)
            watch = 1;
    }

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }
    ql_win_load_font();
    if (!ql_win_open(&win, "QLGL Blob Demo", WIN_W, WIN_H, QL_COLOR_FORMAT_AUTO)) {
        qlgl_shutdown();
        return 1;
    }
    ql_disp_set_bg_color(QL_COLOR_RGB(0x08, 0x0A, 0x10));

    /* ql_ui_load_file 自己持有 doc，ql_ui_free 时一并释放 */
    QLUIRoot *ui = ql_ui_load_file(blob_path);
    if (ui == NULL) {
        printf("加载 %s 失败：%s\n", blob_path, ql_ui_doc_last_error());
        printf("提示：先构建 qlui_blob 目标生成 .qlui\n");
        ql_win_close(&win);
        qlgl_shutdown();
        return 1;
    }
    printf("已加载 %s（%u 节点）\n", blob_path, (unsigned)ui->node_count);
    ql_ui_layout(ui, WIN_W, WIN_H);
    ql_ui_attach_input(ui);

    QLCanvas canvas;
    long last_sig = watch ? file_mtime(blob_path) : -1;
    long frames = 0;
    int reloads = 0;

    while (ql_win_poll(&win)) {
        ql_input_dispatch();

        /* 热重载：文件变了就整份重建（几十微秒，够快） */
        if (watch) {
            long sig = file_mtime(blob_path);
            if (sig >= 0 && sig != last_sig) {
                last_sig = sig;
                QLUIRoot *fresh = ql_ui_load_file(blob_path);
                if (fresh != NULL) {
                    ql_ui_free(ui);
                    ui = fresh;
                    ql_ui_layout(ui, WIN_W, WIN_H);
                    ql_ui_attach_input(ui);
                    reloads++;
                    printf("热重载 #%d：%s（%u 节点）\n", reloads, blob_path,
                           (unsigned)ui->node_count);
                } else {
                    /* 加载失败时保留旧界面，把原因打出来 */
                    printf("热重载失败，保持旧界面：%s\n", ql_ui_doc_last_error());
                }
            }
        }

        if (ql_canvas_from_disp(&canvas, ql_disp_get()) != QL_OK) {
            /* 内存显示路径下走 ql_render 不适用，这里直接画 */
            break;
        }
        ql_canvas_clear(&canvas, QL_COLOR_RGB(0x08, 0x0A, 0x10));
        ql_ui_render_canvas(ui, &canvas);
        if (win.disp != NULL && win.disp->flush != NULL)
            win.disp->flush(win.disp, ql_rect_make(0, 0, WIN_W, WIN_H));
        if (win.disp != NULL && win.disp->flush_done != NULL)
            win.disp->flush_done(win.disp);

        frames++;
        if (win.max_frames > 0 && frames >= win.max_frames)
            break;
        if (win.kind != QL_WIN_KIND_MEM) {
            struct timespec ts = { 0, 16000000L };
            nanosleep(&ts, NULL);
        }
    }

    printf("共 %ld 帧，热重载 %d 次\n", frames, reloads);

    ql_ui_detach_input();
    ql_ui_free(ui);
    ql_win_close(&win);
    qlgl_shutdown();
    return 0;
}
