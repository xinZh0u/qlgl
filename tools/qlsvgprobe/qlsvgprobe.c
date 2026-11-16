/**
 * @file    qlsvgprobe.c
 * @brief   SVG 资产兼容性自检：批量解码 + 静态特性扫描。
 *
 * 为什么需要它：PlutoSVG 对不支持的标签（clipPath / mask / filter / text /
 * <style> / SMIL…）是**静默忽略**的，解码会"成功"但画面缺一块。所以只看
 * `ql_image_decode()` 的返回值不够，还要把源文件里用到的特性列出来。
 *
 * 用法：
 *     qlsvgprobe assets/                     # 递归扫描目录
 *     qlsvgprobe icon.svg logo.svg --size 96
 *     qlsvgprobe icons/ --strict             # 有告警也返回非 0（CI 门槛）
 *     qlsvgprobe icons/ --quiet              # 只打印汇总
 *
 * 退出码：0 = 没有 fail/blocked（--strict 时连 warn 也算失败）；
 *         1 = 有失败/阻断；2 = 参数或 IO 错误。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#  include <dirent.h>
#  include <sys/stat.h>
#endif

#include <qlgl/qlgl.h>

#define PROBE_MAX_BYTES   (32u * 1024u * 1024u)
#define PROBE_MAX_FILES   8192
#define PROBE_PATH_MAX    1024

/* ------------------------------------------------------------------ */
/* 特性表：静态扫描用（标签/属性都是大小写敏感，但这里按不敏感扫）       */
/* ------------------------------------------------------------------ */

enum {
    FEAT_UNSUPPORTED = 0,
    FEAT_BLOCKED,
    FEAT_IMAGE_EMBED,
};

typedef struct {
    const char *label;
    const char *needle;     /* 小写匹配 */
    int         tag;        /* 1 = 需要标签边界（后面跟空白/>//） */
    int         kind;
} Feat;

static const Feat k_feats[] = {
    { "clipPath",     "<clippath",       1, FEAT_UNSUPPORTED },
    { "mask",         "<mask",           1, FEAT_UNSUPPORTED },
    { "pattern",      "<pattern",        1, FEAT_UNSUPPORTED },
    { "marker",       "<marker",         1, FEAT_UNSUPPORTED },
    { "filter",       "<filter",         1, FEAT_UNSUPPORTED },
    { "fe*",          "<fe",             0, FEAT_UNSUPPORTED },
    { "text",         "<text",           1, FEAT_UNSUPPORTED },
    { "tspan",        "<tspan",          1, FEAT_UNSUPPORTED },
    { "textPath",     "<textpath",       1, FEAT_UNSUPPORTED },
    { "style",        "<style",          1, FEAT_UNSUPPORTED },
    { "animate",      "<animate",        1, FEAT_UNSUPPORTED },
    { "set",          "<set",            1, FEAT_UNSUPPORTED },
    { "foreignObject","<foreignobject",  1, FEAT_UNSUPPORTED },
    { "switch",       "<switch",         1, FEAT_UNSUPPORTED },
    { "clip-path 属性","clip-path=",     0, FEAT_UNSUPPORTED },
    { "filter 属性",  "filter=",         0, FEAT_UNSUPPORTED },
    { "mask 属性",    "mask=",           0, FEAT_UNSUPPORTED },
    { "image 非内嵌", "<image",          1, FEAT_IMAGE_EMBED },
    { "script",       "<script",         1, FEAT_BLOCKED },
    { "javascript:",  "javascript:",     0, FEAT_BLOCKED },
    { "外部 href",    "href=\"http",     0, FEAT_BLOCKED },
    { "外部 href",    "href='http",      0, FEAT_BLOCKED },
    { "协议相对 href","href=\"//",       0, FEAT_BLOCKED },
    { "协议相对 href","href='//",        0, FEAT_BLOCKED },
    { "外部 url()",   "url(http",        0, FEAT_BLOCKED },
    { "@import",      "@import",         0, FEAT_BLOCKED },
};

#define FEAT_COUNT ((int)(sizeof(k_feats) / sizeof(k_feats[0])))

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* 文本里找小写 needle；tag=1 时要求后面是标签边界 */
static int text_has(const char *d, size_t n, const char *needle, int tag)
{
    const size_t m = strlen(needle);
    if (m == 0 || n < m)
        return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        for (; j < m; j++)
            if (ascii_lower((unsigned char)d[i + j]) != needle[j])
                break;
        if (j != m)
            continue;
        if (!tag)
            return 1;
        if (i + m >= n)
            return 1;
        const char c = d[i + m];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
            return 1;
    }
    return 0;
}

/* 扫描特性；返回告警位掩码（blocked 位也在这里） */
static uint64_t scan_features(const char *d, size_t n)
{
    uint64_t mask = 0;
    for (int i = 0; i < FEAT_COUNT; i++) {
        if (k_feats[i].kind == FEAT_IMAGE_EMBED) {
            /* <image> 只有 data:image/png|jpg 才画；其他 href 会被静默忽略 */
            if (text_has(d, n, k_feats[i].needle, k_feats[i].tag) &&
                !text_has(d, n, "data:image", 0))
                mask |= 1ull << i;
            continue;
        }
        if (text_has(d, n, k_feats[i].needle, k_feats[i].tag))
            mask |= 1ull << i;
    }
    return mask;
}

static int mask_has_blocked(uint64_t mask)
{
    for (int i = 0; i < FEAT_COUNT; i++)
        if ((mask & (1ull << i)) && k_feats[i].kind == FEAT_BLOCKED)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 文件收集                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char **path;
    int    count;
    int    cap;
} Paths;

static int path_is_svg(const char *p)
{
    const size_t n = strlen(p);
    if (n < 4)
        return 0;
    const char *e = p + n - 4;
    return ascii_lower((unsigned char)e[0]) == '.' &&
           ascii_lower((unsigned char)e[1]) == 's' &&
           ascii_lower((unsigned char)e[2]) == 'v' &&
           ascii_lower((unsigned char)e[3]) == 'g';
}

static int paths_add(Paths *p, const char *path)
{
    if (p->count >= PROBE_MAX_FILES) {
        fprintf(stderr, "qlsvgprobe: 文件数超过 %d\n", PROBE_MAX_FILES);
        return 0;
    }
    if (p->count >= p->cap) {
        int cap = (p->cap > 0) ? p->cap * 2 : 32;
        char **np = (char **)realloc(p->path, (size_t)cap * sizeof(*np));
        if (np == NULL)
            return 0;
        p->path = np;
        p->cap = cap;
    }
    const size_t n = strlen(path);
    char *copy = (char *)malloc(n + 1u);
    if (copy == NULL)
        return 0;
    memcpy(copy, path, n + 1u);
    p->path[p->count++] = copy;
    return 1;
}

static int collect_path(Paths *out, const char *path);

#if !defined(_WIN32)
static int collect_dir(Paths *out, const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "qlsvgprobe: 打不开目录 \"%s\"\n", dir);
        return 0;
    }
    struct dirent *e;
    int ok = 1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[PROBE_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >=
            (int)sizeof(child))
            continue;
        if (!collect_path(out, child)) {
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok;
}
#endif

static int collect_path(Paths *out, const char *path)
{
#if !defined(_WIN32)
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return collect_dir(out, path);
#else
    (void)out;
#endif
    if (path_is_svg(path))
        return paths_add(out, path);
    fprintf(stderr, "qlsvgprobe: 跳过非 .svg 文件 \"%s\"\n", path);
    return 1;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------------------------------------------------ */
/* 解码 + 统计                                                         */
/* ------------------------------------------------------------------ */

static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > PROBE_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    unsigned char *d = (unsigned char *)malloc((size_t)n);
    if (d == NULL || fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return d;
}

typedef struct {
    int      status;       /* 0=ok 1=warn 2=fail 3=blocked */
    char     error[128];
    uint64_t feats;
    double   ms;
    int      w, h;
    int      coverage;     /* 非透明像素百分比 */
    int      blank;
} Result;

static const char *status_name(int s)
{
    switch (s) {
    case 1: return "warn";
    case 2: return "fail";
    case 3: return "block";
    default: return "ok";
    }
}

static void probe_one(const char *path, int size, Result *r)
{
    memset(r, 0, sizeof(*r));
    size_t n = 0;
    unsigned char *data = read_file(path, &n);
    if (data == NULL) {
        r->status = 2;
        snprintf(r->error, sizeof(r->error), "读不了或超过 %u 字节", PROBE_MAX_BYTES);
        return;
    }

    r->feats = scan_features((const char *)data, n);

    if (mask_has_blocked(r->feats)) {
        r->status = 3;
        snprintf(r->error, sizeof(r->error), "外部引用/脚本（运行期会拒绝）");
        free(data);
        return;
    }

    QLImageDecodeReq req;
    memset(&req, 0, sizeof(req));
    req.data = data;
    req.len = n;
    req.src_format = QL_IMAGE_SRC_AUTO;
    req.out_format = QL_IMAGE_RGBA8888;
    req.target_w = (uint16_t)size;
    req.target_h = 0;                 /* 按固有宽高比 */

    QLImage img;
    memset(&img, 0, sizeof(img));
    const clock_t t0 = clock();
    const int rc = ql_image_decode(&req, &img);
    const clock_t t1 = clock();
    r->ms = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

    if (rc != QL_OK) {
        const char *why = ql_image_last_error();
        r->status = 2;
        snprintf(r->error, sizeof(r->error), "%s",
                 why != NULL ? why : "解码失败");
        if (strstr(r->error, "禁止") != NULL)
            r->status = 3;
        free(data);
        return;
    }

    r->w = img.w;
    r->h = img.h;
    if (img.format == QL_IMAGE_RGBA8888 && img.w > 0 && img.h > 0) {
        uint32_t nz = 0;
        for (uint32_t i = 0; i < img.len; i += 4u)
            if (img.px[i + 3u] != 0)
                nz++;
        r->coverage = (int)((uint64_t)nz * 100u / ((uint64_t)img.w * img.h));
        r->blank = (nz == 0);
    }

    ql_mem_free((void *)img.px);
    free(data);

    r->status = r->blank ? 1 : ((r->feats != 0) ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *a0)
{
    fprintf(stderr,
            "用法: %s [--size N] [--strict] [--quiet] <文件.svg | 目录>...\n"
            "  --size N   解码目标宽度（默认 64，高度按比例）\n"
            "  --strict   有警告也返回非 0（CI 门槛）\n"
            "  --quiet    只打印汇总和失败项\n"
            "退出码: 0 通过 / 1 有失败或阻断 / 2 参数或 IO 错误\n",
            a0);
}

int main(int argc, char **argv)
{
    int size = 64;
    int strict = 0, quiet = 0;
    Paths paths;
    memset(&paths, 0, sizeof(paths));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "--size") == 0 && i + 1 < argc) {
            size = atoi(argv[++i]);
        } else if (strcmp(a, "--strict") == 0) {
            strict = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "qlsvgprobe: 未知参数 %s\n", a);
            usage(argv[0]);
            return 2;
        } else if (!collect_path(&paths, a)) {
            return 2;
        }
    }
    if (size < 1 || size > QL_CFG_IMAGE_SVG_MAX_DIM) {
        fprintf(stderr, "qlsvgprobe: --size 必须在 1..%d\n", QL_CFG_IMAGE_SVG_MAX_DIM);
        return 2;
    }
    if (paths.count == 0) {
        usage(argv[0]);
        return 2;
    }

    qsort(paths.path, (size_t)paths.count, sizeof(paths.path[0]), cmp_path);

    int counts[4] = { 0, 0, 0, 0 };
    int feat_counts[FEAT_COUNT];
    memset(feat_counts, 0, sizeof(feat_counts));

    printf("qlsvgprobe: %d 个文件，目标宽度 %dpx\n\n", paths.count, size);

    for (int i = 0; i < paths.count; i++) {
        Result r;
        probe_one(paths.path[i], size, &r);
        counts[r.status]++;

        if (!quiet || r.status >= 2) {
            printf("  %-5s  %-46s", status_name(r.status), paths.path[i]);
            if (r.status == 0 || r.status == 1) {
                printf(" %dx%d  %.2fms  cov %d%%", r.w, r.h, r.ms, r.coverage);
                if (r.blank)
                    printf("  空白");
            }
            printf("\n");
            if (r.status >= 2)
                printf("          %s\n", r.error);
            if (r.feats != 0) {
                printf("          不支持/风险:");
                for (int k = 0; k < FEAT_COUNT; k++) {
                    if ((r.feats & (1ull << k)) == 0)
                        continue;
                    printf(" %s", k_feats[k].label);
                    feat_counts[k]++;
                }
                printf("\n");
            }
        } else {
            /* quiet 下仍然累计特性计数（用于汇总） */
            for (int k = 0; k < FEAT_COUNT; k++)
                if (r.feats & (1ull << k))
                    feat_counts[k]++;
        }
        free(paths.path[i]);
    }

    printf("\n汇总: ok %d / warn %d / fail %d / block %d\n",
           counts[0], counts[1], counts[2], counts[3]);

    int any_feat = 0;
    for (int k = 0; k < FEAT_COUNT; k++) {
        if (feat_counts[k] == 0)
            continue;
        printf("  %s %s×%d\n", any_feat ? "     " : "特性:", k_feats[k].label,
               feat_counts[k]);
        any_feat = 1;
    }
    if (!any_feat)
        printf("  没有发现已知的不支持特性\n");

    free(paths.path);

    if (counts[2] > 0 || counts[3] > 0)
        return 1;
    if (strict && (counts[1] > 0 || any_feat))
        return 1;
    return 0;
}
