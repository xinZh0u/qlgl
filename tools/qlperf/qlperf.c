/**
 * @file    qlperf.c
 * @brief   进程级性能探针：CPU / GPU / 内存 + 窗口缩放驱动。
 *
 * 为什么要有这个工具：调"拖动窗口掉帧"这类问题时，靠肉眼看不出到底是
 * **CPU 打满**、**GPU 打满**、还是**在等 vsync**。三者表现都是"卡"，但改法完全不同：
 *
 *   CPU 打满   → 布局/光栅/事件循环里有贵操作（看是哪一帧）
 *   GPU 打满   → 填充率或批次问题（看每帧多少个 draw call）
 *   等 vsync   → 一切正常，只是被刷新率限制（这时"掉帧"是错觉）
 *
 * 用法：
 *     qlperf run [选项] -- ./example_22_media            # 启动并监控
 *     qlperf watch [选项] <pid>                          # 监控已有进程
 *
 * 选项：
 *     --interval MS      采样间隔（默认 250ms）
 *     --seconds S        监控时长（默认 10s；run 模式进程退出会提前结束）
 *     --resize TITLE     期间不断缩放标题含 TITLE 的窗口（需要 X11）
 *     --resize-range A-B 缩放宽度范围（默认 700-900），高度固定 420
 *     --csv FILE         把每次采样写成 CSV
 *     --quiet            只打汇总
 *
 * 数据来源（全部是内核/驱动给的，不做任何猜测）：
 *     CPU    /proc/<pid>/stat 的 utime+stime（时钟滴答 → %）
 *     内存   /proc/<pid>/status 的 VmRSS
 *     GPU    进程级：/proc/<pid>/fdinfo 里 amdgpu 的 drm-engine-* 计数器
 *            整机级：/sys/class/drm/cardN/device/gpu_busy_percent
 *     帧     应用自己用 `--stats` 打（ql_gfx_gl_frame_us 的百分位）
 *
 * 退出码：0 = 正常；1 = 用法/环境错误；run 模式下等于子进程退出码。
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

#define MAX_SAMPLES 20000

typedef struct {
    double t;             /* 相对开始的秒数 */
    double cpu_pct;       /* 进程 CPU（单核 100% 口径） */
    double gpu_proc_pct;  /* 进程占用的 GPU 引擎时间 / 墙钟（可能 > 100） */
    double gpu_busy_pct;  /* 整机 GPU 占用（驱动报告，-1 = 读不到） */
    double rss_mb;
    int    threads;
} Sample;

static Sample g_samples[MAX_SAMPLES];
static int    g_nsamples;

/* ------------------------------ 小工具 ------------------------------ */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    size_t n = fread(buf, 1, cap - 1u, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

/* /proc/<pid>/stat → utime+stime（单位：时钟滴答） */
static int proc_cpu_ticks(pid_t pid, unsigned long long *out)
{
    char path[64], buf[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (!read_file(path, buf, sizeof(buf)))
        return 0;
    /* 第 2 个字段（comm）里可能有空格和括号，从最后一个 ')' 之后开始数 */
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return 0;
    p++;
    /* 现在 p 指向 state，字段序号从 3 开始；utime 是第 14 个，stime 第 15 个 */
    unsigned long long vals[16];
    int idx = 0;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok != NULL && idx < 16;
         tok = strtok_r(NULL, " ", &save)) {
        vals[idx++] = strtoull(tok, NULL, 10);
    }
    if (idx < 15)
        return 0;
    *out = vals[11] + vals[12];   /* 数组下标 = 字段号 - 3 - 1 */
    return 1;
}

/* /proc/<pid>/status → VmRSS（KB）与 Threads */
static void proc_mem_threads(pid_t pid, double *rss_mb, int *threads)
{
    char path[64], buf[8192];
    *rss_mb = -1;
    *threads = -1;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    if (!read_file(path, buf, sizeof(buf)))
        return;
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, "VmRSS:", 6) == 0)
            *rss_mb = atof(line + 6) / 1024.0;
        else if (strncmp(line, "Threads:", 8) == 0)
            *threads = atoi(line + 8);
    }
}

/*
 * 进程级 GPU 时间：amdgpu/i915 在 /proc/<pid>/fdinfo/<fd> 里维护
 * `drm-engine-<name>: <ns>` 计数器（渲染引擎累计纳秒）。
 * 返回所有 drm 句柄、所有引擎的累计和；没有 DRM 句柄时返回 0。
 */
static unsigned long long proc_gpu_ns(pid_t pid)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/fdinfo", (int)pid);
    DIR *d = opendir(dir);
    if (d == NULL)
        return 0;
    unsigned long long total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char path[512], buf[8192];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (!read_file(path, buf, sizeof(buf)))
            continue;
        for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            if (strncmp(line, "drm-engine-", 11) != 0)
                continue;
            const char *colon = strchr(line, ':');
            if (colon != NULL)
                total += strtoull(colon + 1, NULL, 10);
        }
    }
    closedir(d);
    return total;
}

/* 整机 GPU 占用百分比（amdgpu 的 gpu_busy_percent）；读不到返回 -1 */
static double sysfs_gpu_busy(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (d == NULL)
        return -1;
    double best = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "card", 4) != 0 || strchr(de->d_name, '-') != NULL)
            continue;
        char path[512], buf[64];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/gpu_busy_percent",
                 de->d_name);
        if (read_file(path, buf, sizeof(buf))) {
            const double v = atof(buf);
            if (v > best)
                best = v;
        }
    }
    closedir(d);
    return best;
}

/* ------------------------------ 缩放驱动 ------------------------------ */

typedef struct {
    const char *title;   /* 标题片段；NULL = 不驱动 */
    int         w0, w1;
    void       *dpy;     /* Display*（避免头文件依赖传染，用 void* 存） */
    unsigned long win;
    int         dir;
    int         w;
    double      next_s;
    int         steps;
} Resizer;

#ifdef HAVE_X11
static Window x_find_win(Display *d, Window w, const char *needle)
{
    char *name = NULL;
    if (XFetchName(d, w, &name) && name != NULL) {
        const int hit = (strstr(name, needle) != NULL);
        XFree(name);
        if (hit)
            return w;
    }
    Window root, parent, *kids = NULL;
    unsigned int n = 0;
    Window found = 0;
    if (XQueryTree(d, w, &root, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n && found == 0; i++)
            found = x_find_win(d, kids[i], needle);
        if (kids != NULL)
            XFree(kids);
    }
    return found;
}
#endif

static int resizer_init(Resizer *r)
{
    memset(r, 0, sizeof(*r));
    r->dir = 1;
    r->next_s = 0;
    return 0;
}

static void resizer_tick(Resizer *r, double now)
{
#ifdef HAVE_X11
    if (r->title == NULL || now < r->next_s)
        return;
    r->next_s = now + 0.012;

    if (r->dpy == NULL) {
        Display *d = XOpenDisplay(NULL);
        if (d == NULL) {
            fprintf(stderr, "qlperf: 打不开 X11 显示，缩放驱动关闭\n");
            r->title = NULL;
            return;
        }
        r->dpy = d;
        /* 窗口可能还没建好：最多等 3 秒 */
        for (int i = 0; i < 60 && r->win == 0; i++) {
            r->win = (unsigned long)x_find_win(d, DefaultRootWindow(d), r->title);
            if (r->win == 0)
                usleep(50000);
        }
        if (r->win == 0) {
            fprintf(stderr, "qlperf: 找不到标题含 \"%s\" 的窗口，缩放驱动关闭\n",
                    r->title);
            r->title = NULL;
            return;
        }
        r->w = r->w0;
        fprintf(stderr, "qlperf: 开始缩放窗口 0x%lx（宽 %d..%d）\n", r->win, r->w0,
                r->w1);
    }
    XResizeWindow(r->dpy, (Window)r->win, (unsigned)r->w, 420);
    XFlush(r->dpy);
    r->w += r->dir * 6;
    if (r->w >= r->w1)
        r->dir = -1;
    if (r->w <= r->w0)
        r->dir = 1;
    r->steps++;
#else
    (void)r;
    (void)now;
#endif
}

/* ------------------------------ 采样与汇总 ------------------------------ */

typedef struct {
    pid_t pid;
    int   interval_ms;
    int   seconds;
    int   quiet;
    const char *csv;
} Opts;

static void sample_once(pid_t pid, double t0, unsigned long long *last_ticks,
                        unsigned long long *last_gpu, double *last_t, Opts *o)
{
    static int hz = 0;
    if (hz == 0)
        hz = (int)sysconf(_SC_CLK_TCK);

    unsigned long long ticks = 0;
    if (!proc_cpu_ticks(pid, &ticks))
        return;
    const unsigned long long gpu = proc_gpu_ns(pid);
    const double now = now_s();
    const double dt = now - *last_t;

    Sample *s = &g_samples[g_nsamples < MAX_SAMPLES ? g_nsamples : MAX_SAMPLES - 1];
    memset(s, 0, sizeof(*s));
    s->t = now - t0;
    if (*last_ticks == 0 && *last_t == 0) {
        s->cpu_pct = 0;
        s->gpu_proc_pct = 0;
    } else if (dt > 0) {
        s->cpu_pct = (double)(ticks - *last_ticks) / (double)hz / dt * 100.0;
        s->gpu_proc_pct = (double)(gpu - *last_gpu) / (dt * 1e9) * 100.0;
    }
    s->gpu_busy_pct = sysfs_gpu_busy();
    proc_mem_threads(pid, &s->rss_mb, &s->threads);
    if (g_nsamples < MAX_SAMPLES)
        g_nsamples++;

    *last_ticks = ticks;
    *last_gpu = gpu;
    *last_t = now;

    if (!o->quiet && g_nsamples > 1)
        printf("  %6.2f  %6.1f  %9.1f  %9.1f  %8.1f  %5d\n", s->t, s->cpu_pct,
               s->gpu_proc_pct, s->gpu_busy_pct, s->rss_mb, s->threads);
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double pct(double *v, int n, double p)
{
    if (n <= 0)
        return 0;
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    int k = (int)(p * (double)(n - 1) + 0.5);
    if (k < 0)
        k = 0;
    if (k >= n)
        k = n - 1;
    return v[k];
}

static void report(Resizer *rz, double wall_s)
{
    if (g_nsamples <= 0) {
        printf("qlperf: 没采到样本（进程已经退出？）\n");
        return;
    }
    double cpu[MAX_SAMPLES], gp[MAX_SAMPLES], gb[MAX_SAMPLES], rss[MAX_SAMPLES];
    int n = g_nsamples, nb = 0;
    for (int i = 0; i < n; i++) {
        cpu[i] = g_samples[i].cpu_pct;
        gp[i] = g_samples[i].gpu_proc_pct;
        rss[i] = g_samples[i].rss_mb;
        if (g_samples[i].gpu_busy_pct >= 0)
            gb[nb++] = g_samples[i].gpu_busy_pct;
    }
    double gpu_avg = 0, gpu_max = 0;
    for (int i = 0; i < n; i++) {
        gpu_avg += gp[i];
        if (gp[i] > gpu_max)
            gpu_max = gp[i];
    }
    gpu_avg /= (double)n;
    double sys_avg = 0, sys_max = 0;
    for (int i = 0; i < nb; i++) {
        sys_avg += gb[i];
        if (gb[i] > sys_max)
            sys_max = gb[i];
    }
    if (nb > 0)
        sys_avg /= (double)nb;

    printf("\n==== qlperf 汇总（%.1fs，%d 个采样点）====\n", wall_s, n);
    printf("  CPU       p50 %5.1f%%   p95 %5.1f%%   max %5.1f%%\n",
           pct(cpu, n, 0.50), pct(cpu, n, 0.95), pct(cpu, n, 1.0));
    printf("  GPU(进程) avg %5.1f%%   max %5.1f%%   （引擎时间 / 墙钟）\n", gpu_avg,
           gpu_max);
    if (nb > 0)
        printf("  GPU(整机) avg %5.1f%%   max %5.1f%%   （驱动报告）\n", sys_avg,
               sys_max);
    else
        printf("  GPU(整机) 读不到（非 amdgpu 或没有权限）\n");
    printf("  RSS       max %.1f MB\n", pct(rss, n, 1.0));
    if (rz->title != NULL)
        printf("  缩放驱动  %d 次\n", rz->steps);

    /* 结论：让人一眼知道往哪儿改 */
    const double cpu_p95 = pct(cpu, n, 0.95);
    if (cpu_p95 > 85.0)
        printf("  结论：CPU 接近打满 → 找热点（布局 / 光栅 / 事件循环）。\n");
    else if (sys_max > 85.0 || gpu_max > 85.0)
        printf("  结论：GPU 接近打满 → 填充率或批次问题。\n");
    else if (cpu_p95 < 40.0)
        printf("  结论：CPU/GPU 都没满 → 时间花在等 vsync / 合成器上"
               "（这时\"掉帧\"多半是刷新率限制，不是我们的问题）。\n");
    else
        printf("  结论：资源占用中等，看不出瓶颈；建议配合应用的 --stats 看帧时间分布。\n");
}

/* ------------------------------ 入口 ------------------------------ */

static void usage(void)
{
    printf(
        "用法:\n"
        "  qlperf run [选项] -- <命令> [参数...]   启动并监控\n"
        "  qlperf watch [选项] <pid>               监控已有进程\n"
        "\n"
        "选项:\n"
        "  --interval MS       采样间隔（默认 250）\n"
        "  --seconds S         监控时长（默认 10）\n"
        "  --resize TITLE      期间不断缩放标题含 TITLE 的窗口（X11）\n"
        "  --resize-range A-B  缩放宽度范围（默认 700-900）\n"
        "  --csv FILE          把采样写成 CSV\n"
        "  --quiet             只打汇总\n");
}

int main(int argc, char **argv)
{
    Opts o;
    memset(&o, 0, sizeof(o));
    o.interval_ms = 250;
    o.seconds = 10;

    Resizer rz;
    resizer_init(&rz);
    rz.w0 = 700;
    rz.w1 = 900;

    if (argc < 2) {
        usage();
        return 1;
    }
    const int run_mode = (strcmp(argv[1], "run") == 0);
    const int watch_mode = (strcmp(argv[1], "watch") == 0);
    if (!run_mode && !watch_mode) {
        usage();
        return 1;
    }

    char **cmd = NULL;
    pid_t pid = 0;
    int i = 2;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd = &argv[i + 1];
            break;
        }
        if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
            o.interval_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            o.seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--resize") == 0 && i + 1 < argc)
            rz.title = argv[++i];
        else if (strcmp(argv[i], "--resize-range") == 0 && i + 1 < argc) {
            int a = 0, b = 0;
            if (sscanf(argv[++i], "%d-%d", &a, &b) == 2 && a > 0 && b > a) {
                rz.w0 = a;
                rz.w1 = b;
            }
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
            o.csv = argv[++i];
        else if (strcmp(argv[i], "--quiet") == 0)
            o.quiet = 1;
        else if (watch_mode)
            pid = (pid_t)atoi(argv[i]);
        else {
            fprintf(stderr, "qlperf: 不认识的参数 %s\n", argv[i]);
            return 1;
        }
    }

    if (run_mode && (cmd == NULL || cmd[0] == NULL)) {
        fprintf(stderr, "qlperf run: 缺少命令（用 -- 隔开）\n");
        return 1;
    }
    if (watch_mode && pid <= 0) {
        fprintf(stderr, "qlperf watch: 缺少 pid\n");
        return 1;
    }
    if (rz.w0 < 64)
        rz.w0 = 64;
    if (rz.w1 <= rz.w0)
        rz.w1 = rz.w0 + 200;

    if (run_mode) {
        pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            execvp(cmd[0], cmd);
            perror(cmd[0]);
            _exit(127);
        }
        /* 让子进程先跑起来（窗口/GL 初始化），避免采到一堆 0 */
        usleep(120000);
    }

    if (!o.quiet) {
        printf("qlperf: 监控 pid %d，间隔 %dms，最多 %ds%s\n", (int)pid, o.interval_ms,
               o.seconds, rz.title != NULL ? "，期间缩放窗口" : "");
        printf("  %6s  %6s  %9s  %9s  %8s  %5s\n", "t(s)", "CPU%", "GPU进程%",
               "GPU整机%", "RSS(MB)", "线程");
    }

    const double t0 = now_s();
    unsigned long long last_ticks = 0, last_gpu = 0;
    double last_t = 0;
    int status = 0;
    int exited = 0;

    double next_sample = 0;
    while (now_s() - t0 < (double)o.seconds) {
        if (run_mode) {
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = 1;
                break;
            }
        } else {
            if (kill(pid, 0) != 0 && errno == ESRCH) {
                exited = 1;
                break;
            }
        }
        const double now = now_s() - t0;
        /* 缩放每 ~8ms 推一次（模拟拖边框的密度），采样按 --interval 走 */
        resizer_tick(&rz, now);
        if (now >= next_sample) {
            sample_once(pid, t0, &last_ticks, &last_gpu, &last_t, &o);
            next_sample = now + (double)o.interval_ms / 1000.0;
        }
        usleep(3000);
    }

    /* 子进程退了也要留最后一次样本 */
    if (exited && g_nsamples == 0)
        sample_once(pid, t0, &last_ticks, &last_gpu, &last_t, &o);

    const double wall = now_s() - t0;
    report(&rz, wall);

    if (o.csv != NULL) {
        FILE *f = fopen(o.csv, "w");
        if (f != NULL) {
            fprintf(f, "t,cpu_pct,gpu_proc_pct,gpu_busy_pct,rss_mb,threads\n");
            for (int k = 0; k < g_nsamples; k++)
                fprintf(f, "%.3f,%.2f,%.2f,%.2f,%.2f,%d\n", g_samples[k].t,
                        g_samples[k].cpu_pct, g_samples[k].gpu_proc_pct,
                        g_samples[k].gpu_busy_pct, g_samples[k].rss_mb,
                        g_samples[k].threads);
            fclose(f);
            printf("  CSV → %s\n", o.csv);
        }
    }

    if (run_mode) {
        if (!exited) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        return 1;
    }
    return 0;
}
