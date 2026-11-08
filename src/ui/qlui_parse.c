/**
 * @file    qlui_parse.c
 * @brief   HTML + CSS 文本 → QLUIDoc（M3.5 第 3 步）。
 *
 * **HTML/CSS 的语义只有这一份**：构建期工具（`tools/qltext`）与运行期
 * （`ql_ui_load_text()`）走的是同一个函数。所以这个文件既决定"程序里长什么样"，
 * 也决定"构建产物长什么样"，两边不可能漂移。
 *
 * 唯一的护栏是"别改变已有输入的结果"：`test_golden` 拿提交在 `test/golden/`
 * 的 27 个基准守着 blob 逐字节相同，`test_text_reject` 守着接受/拒绝集合。
 * 任何"这里可以优化一下"的念头都有可能让基准对不上——先跑测试再动手。
 *
 * 分层：
 *   ① 错误与内存        ② 值解析（颜色/长度/渐变/阴影/transform/clip-path/…）
 *   ③ 声明集合 + 简写展开 ④ CSS 词法与选择器     ⑤ HTML 词法
 *   ⑥ 级联 + 继承        ⑦ 编译成 QLUIStyle     ⑧ 建表（节点/样式/文本/字号/关键帧/动画）
 *
 * 可摘：core 里没有任何静态引用指向本文件，MCU 侧不加进构建即零字节成本。
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

/* ============================== ① 错误与内存 ============================== */

static char s_error[192];

#define QL_UI_ALIGN8(x)   (((x) + 7u) & ~(size_t)7u)

const char *ql_ui_text_last_error(void)
{
    return s_error[0] != '\0' ? s_error : "no error";
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

static void *xalloc(size_t size)
{
    return ql_mem_alloc(size);
}

static void *xcalloc(size_t count, size_t size)
{
    return ql_mem_calloc(count, size);
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xalloc(n + 1u);
    if (p == NULL)
        return NULL;
    if (n > 0)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *xstrdup(const char *s)
{
    return (s != NULL) ? xstrndup(s, strlen(s)) : NULL;
}

/* 增长型字节缓冲 */
typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} SB;

static int sb_reserve(SB *sb, size_t extra)
{
    if (sb->len + extra + 1u <= sb->cap)
        return 1;
    size_t cap = (sb->cap > 0) ? sb->cap : 64u;
    while (cap < sb->len + extra + 1u)
        cap *= 2u;
    char *np = (char *)ql_mem_realloc(sb->p, cap);
    if (np == NULL)
        return 0;
    sb->p = np;
    sb->cap = cap;
    return 1;
}

static int sb_add(SB *sb, const char *data, size_t n)
{
    if (!sb_reserve(sb, n))
        return 0;
    if (n > 0)
        memcpy(sb->p + sb->len, data, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
    return 1;
}

static int sb_addc(SB *sb, char c)
{
    return sb_add(sb, &c, 1u);
}

static int sb_adds(SB *sb, const char *s)
{
    return (s != NULL) ? sb_add(sb, s, strlen(s)) : 1;
}

static void sb_free(SB *sb)
{
    ql_mem_free(sb->p);
    sb->p = NULL;
    sb->len = sb->cap = 0;
}

/* ============================== ② 值解析 ============================== */

/*
 * Python 的 round() 是**银行家舍入**（.5 进偶数），不是 C 的 round()。
 * 差一个色阶就过不了 blob 逐字节比对，所以这里必须用 nearbyint（默认就近取偶）。
 */
static long py_round(double v)
{
    return (long)nearbyint(v);
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/*
 * 空白长度（字节）。ASCII 空白返回 1；Python 的 str 版 `\s` 还包含这些
 * Unicode 空白（U+00A0/U+1680/U+2000..200A/U+2028/U+2029/U+202F/U+205F/U+3000），
 * 它们在中文界面里很常见（示例文本里就有一个全角空格）。
 * 不处理的话"折叠空白"的结果会与构建期工具不同，blob 就对不上。
 */
static size_t u8_ws_len(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] == ' ' || u[0] == '\t' || u[0] == '\n' || u[0] == '\r' || u[0] == '\f' ||
        u[0] == '\v')
        return 1;
    if (u[0] == 0xC2 && u[1] == 0xA0)
        return 2;
    if (u[0] == 0xE1 && u[1] == 0x9A && u[2] == 0x80)
        return 3;
    if (u[0] == 0xE2 && u[1] == 0x80 && u[2] >= 0x80 && u[2] <= 0x8A)
        return 3;
    if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0xA8 || u[2] == 0xA9 || u[2] == 0xAF))
        return 3;
    if (u[0] == 0xE2 && u[1] == 0x81 && u[2] == 0x9F)
        return 3;
    if (u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80)
        return 3;
    return 0;
}

static const char *skip_space(const char *s)
{
    while (s != NULL && is_space(*s))
        s++;
    return s;
}

/* 去掉两端空白并转小写，写进 out（长度上限 cap-1） */
static void trim_lower(const char *s, char *out, size_t cap)
{
    if (cap == 0)
        return;
    while (is_space(*s))
        s++;
    const char *end = s + strlen(s);
    while (end > s && is_space(end[-1]))
        end--;
    size_t n = (size_t)(end - s);
    if (n >= cap)
        n = cap - 1u;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? (s[i] - 'A' + 'a') : s[i]);
    out[n] = '\0';
}

/* 严格："-?\d+(\.\d+)?" 整体匹配则返回 1 并写 *out */
static int match_number(const char *s, const char **end, double *out)
{
    const char *p = s;
    if (*p == '-' || *p == '+')
        p++;
    const char *digits = p;
    while (*p >= '0' && *p <= '9')
        p++;
    int ndig = (int)(p - digits);
    if (ndig == 0)
        return 0;
    if (*p == '.') {
        const char *frac = p + 1;
        while (*frac >= '0' && *frac <= '9')
            frac++;
        if (frac == p + 1)
            return 0;              /* "1." 不合法（Python 侧的正则要求 \d+） */
        p = frac;
    }
    if (end != NULL)
        *end = p;
    if (out != NULL) {
        char buf[64];
        size_t n = (size_t)(p - s);
        if (n >= sizeof(buf))
            return 0;
        memcpy(buf, s, n);
        buf[n] = '\0';
        *out = strtod(buf, NULL);
    }
    return 1;
}

/* 命名色表（transparent 用 sentinel 表示） */
typedef struct {
    const char *name;
    int         transparent;
    uint32_t    rgb;
} NamedColor;

static const NamedColor k_named[] = {
    { "transparent", 1, 0x000000u }, { "black", 0, 0x000000u },
    { "white", 0, 0xFFFFFFu },       { "red", 0, 0xFF0000u },
    { "green", 0, 0x008000u },       { "lime", 0, 0x00FF00u },
    { "blue", 0, 0x0000FFu },        { "gray", 0, 0x808080u },
    { "grey", 0, 0x808080u },        { "silver", 0, 0xC0C0C0u },
    { "yellow", 0, 0xFFFF00u },      { "orange", 0, 0xFFA500u },
    { "purple", 0, 0x800080u },      { "navy", 0, 0x000080u },
    { "teal", 0, 0x008080u },
};

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 返回 1 成功。rgb 写 0xRRGGBB，*alpha 写 0..255 */
static int parse_color(const char *text, const char *where, uint32_t *rgb,
                       uint8_t *alpha)
{
    char t[128];
    trim_lower(text, t, sizeof(t));

    for (size_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++) {
        if (strcmp(t, k_named[i].name) == 0) {
            *rgb = k_named[i].rgb;
            *alpha = k_named[i].transparent ? 0u : 255u;
            return 1;
        }
    }

    if (t[0] == '#') {
        const char *h = t + 1;
        size_t n = strlen(h);
        if (n == 3) {
            char e[7];
            e[0] = h[0];
            e[1] = h[0];
            e[2] = h[1];
            e[3] = h[1];
            e[4] = h[2];
            e[5] = h[2];
            e[6] = '\0';
            for (int i = 0; i < 6; i++) {
                if (hex_value(e[i]) < 0) {
                    set_error("%s: 非法颜色 '%s'", where, text);
                    return 0;
                }
            }
            uint32_t v = (uint32_t)strtoul(e, NULL, 16);
            *rgb = v;
            *alpha = 255u;
            return 1;
        }
        if (n == 6) {
            for (int i = 0; i < 6; i++) {
                if (hex_value(h[i]) < 0) {
                    set_error("%s: 非法颜色 '%s'", where, text);
                    return 0;
                }
            }
            uint32_t v = (uint32_t)strtoul(h, NULL, 16);
            *rgb = v;
            *alpha = 255u;
            return 1;
        }
        set_error("%s: 非法颜色 '%s'", where, text);
        return 0;
    }

    /* rgb(r, g, b) / rgba(r, g, b, a) */
    if (strncmp(t, "rgb", 3) == 0 && (t[3] == '(' || t[3] == 'a')) {
        int is_rgba = (t[3] == 'a');
        const char *open = strchr(t, '(');
        const char *close = strrchr(t, ')');
        if (open == NULL || close == NULL || close < open) {
            set_error("%s: 不支持的颜色写法 '%s'", where, text);
            return 0;
        }
        double v[4] = { 0, 0, 0, 0 };
        int nv = 0;
        const char *p = open + 1;
        while (p < close && nv < 4) {
            p = skip_space(p);
            const char *end = NULL;
            double num = 0;
            if (!match_number(p, &end, &num))
                break;
            v[nv++] = num;
            p = skip_space(end);
            if (p < close && *p == ',')
                p++;
            else
                break;
        }
        p = skip_space(p);
        if (nv != (is_rgba ? 4 : 3) || p != close) {
            set_error("%s: 不支持的颜色写法 '%s'", where, text);
            return 0;
        }
        for (int i = 0; i < 3; i++) {
            if (v[i] < 0 || v[i] > 255) {
                set_error("%s: rgb() 分量超范围 '%s'", where, text);
                return 0;
            }
        }
        if (is_rgba && (v[3] < 0 || v[3] > 1)) {
            set_error("%s: rgba() 分量超范围 '%s'", where, text);
            return 0;
        }
        *rgb = ((uint32_t)(long)v[0] << 16) | ((uint32_t)(long)v[1] << 8) |
               (uint32_t)(long)v[2];
        *alpha = is_rgba ? (uint8_t)py_round(v[3] * 255.0) : 255u;
        return 1;
    }

    set_error("%s: 不支持的颜色写法 '%s'（支持 #rgb/#rrggbb/rgb()/rgba()/命名色）",
              where, text);
    return 0;
}

/* 长度：auto / <num>px / <num>%。unit 用 QL_UI_UNIT_* */
static int parse_length(const char *text, const char *where, int allow_percent,
                        uint8_t *unit, int *value)
{
    char t[128];
    trim_lower(text, t, sizeof(t));

    if (strcmp(t, "auto") == 0) {
        *unit = QL_UI_UNIT_AUTO;
        *value = 0;
        return 1;
    }
    size_t n = strlen(t);
    if (n > 2 && strcmp(t + n - 2u, "px") == 0) {
        char num[128];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *unit = QL_UI_UNIT_PX;
            *value = (int)py_round(v);
            return 1;
        }
    } else if (n > 1 && t[n - 1u] == '%') {
        char num[128];
        memcpy(num, t, n - 1u);
        num[n - 1u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            if (!allow_percent) {
                set_error("%s: 此处不支持百分比 '%s'", where, text);
                return 0;
            }
            *unit = QL_UI_UNIT_PERCENT;
            *value = (int)py_round(v);
            return 1;
        }
    } else if (strcmp(t, "0") == 0) {
        *unit = QL_UI_UNIT_PX;
        *value = 0;
        return 1;
    }

    set_error("%s: 不支持的长度 '%s'（只支持 px 与 %%）", where, text);
    return 0;
}

/* parse_number：px / % / 纯数字 / auto */
enum { NUM_PX, NUM_PCT, NUM_NUM, NUM_AUTO };

static int parse_number(const char *text, const char *where, int allow_percent,
                        int allow_auto, int *kind, double *value)
{
    char t[128];
    trim_lower(text, t, sizeof(t));

    if (allow_auto && strcmp(t, "auto") == 0) {
        *kind = NUM_AUTO;
        *value = 0;
        return 1;
    }
    size_t n = strlen(t);
    if (n > 2 && strcmp(t + n - 2u, "px") == 0) {
        char num[128];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *kind = NUM_PX;
            *value = (double)py_round(v);
            return 1;
        }
    }
    if (allow_percent && n > 1 && t[n - 1u] == '%') {
        char num[128];
        memcpy(num, t, n - 1u);
        num[n - 1u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *kind = NUM_PCT;
            *value = (double)py_round(v);
            return 1;
        }
    }
    {
        const char *end = NULL;
        double v = 0;
        if (match_number(t, &end, &v) && *end == '\0') {
            *kind = NUM_NUM;
            *value = v;
            return 1;
        }
    }
    if (strcmp(t, "0") == 0) {
        *kind = NUM_PX;
        *value = 0;
        return 1;
    }
    set_error("%s: 无法解析数值 '%s'", where, text);
    return 0;
}

/*
 * margin/padding 的 1/2/4 值语法。values 写 (上,右,下,左)。
 * `where` 只用于报错；错误信息与 Python 侧保持同一语义（不逐字相同）。
 */
static int parse_edges(const char *text, const char *where, int out[4])
{
    double vals[4] = { 0, 0, 0, 0 };
    int n = 0;
    const char *p = text;
    while (*p != '\0' && n < 4) {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        char tok[64];
        size_t len = (size_t)(p - start);
        if (len >= sizeof(tok)) {
            set_error("%s: margin/padding 的值太长", where);
            return 0;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';
        uint8_t unit;
        int v;
        if (!parse_length(tok, where, 0, &unit, &v))
            return 0;
        if (unit != QL_UI_UNIT_PX) {
            set_error("%s: margin/padding 只支持 px", where);
            return 0;
        }
        vals[n++] = (double)v;
    }
    if (n == 1) {
        out[0] = out[1] = out[2] = out[3] = (int)vals[0];
    } else if (n == 2) {
        out[0] = out[2] = (int)vals[0];
        out[1] = out[3] = (int)vals[1];
    } else if (n == 4) {
        for (int i = 0; i < 4; i++)
            out[i] = (int)vals[i];
    } else {
        set_error("%s: margin/padding 需要 1/2/4 个值", where);
        return 0;
    }
    return 1;
}

/* 渐变方向：返回 bg_grad_dir 的值（1 垂直 / 2 水平 / 3 径向） */
static int gradient_dir_of(const char *token)
{
    char t[64];
    trim_lower(token, t, sizeof(t));
    if (strcmp(t, "to bottom") == 0 || strcmp(t, "180deg") == 0 ||
        strcmp(t, "to top") == 0)
        return 1;
    if (strcmp(t, "to right") == 0 || strcmp(t, "90deg") == 0 ||
        strcmp(t, "to left") == 0)
        return 2;
    return 0;
}

/* linear-gradient(...) / radial-gradient(...) → 起色、止色、方向 */
static int parse_gradient(const char *text, const char *where, uint32_t *from,
                          uint32_t *to, int *dir)
{
    const char *p = skip_space(text);
    int radial = 0;
    if (strncmp(p, "radial-gradient(", 16) == 0) {
        radial = 1;
        p += 16;
    } else if (strncmp(p, "linear-gradient(", 16) == 0) {
        p += 16;
    } else {
        set_error("%s: 只支持 linear-gradient() / radial-gradient()，收到 '%s'", where,
                  text);
        return 0;
    }

    /* 按逗号切三段（颜色里不会有逗号，因为有 alpha 时是 rgba(...) 形式——
       这里用"括号深度"保护，与 Python 的 split(',') 行为一致的是：
       rgba(0,0,0,.5) 里确实有逗号，所以必须先配对括号） */
    const char *parts[3] = { NULL, NULL, NULL };
    size_t lens[3] = { 0, 0, 0 };
    int np = 0;
    const char *start = p;
    int depth = 0;
    for (const char *q = p;; q++) {
        if (*q == '(') {
            depth++;
        } else if (*q == ')') {
            if (depth == 0) {
                if (np < 3) {
                    parts[np] = start;
                    lens[np] = (size_t)(q - start);
                    np++;
                }
                break;
            }
            depth--;
        } else if (*q == ',' && depth == 0) {
            if (np < 3) {
                parts[np] = start;
                lens[np] = (size_t)(q - start);
                np++;
            }
            start = q + 1;
        } else if (*q == '\0') {
            break;
        }
    }
    if (np != 3) {
        set_error("%s: %s 需要 3 个参数（%s, 起色, 止色）", where,
                  radial ? "radial-gradient" : "linear-gradient",
                  radial ? "位置" : "方向");
        return 0;
    }
    char seg[3][128];
    for (int i = 0; i < 3; i++) {
        size_t n = lens[i];
        const char *s = parts[i];
        while (n > 0 && is_space(*s)) {
            s++;
            n--;
        }
        while (n > 0 && is_space(s[n - 1]))
            n--;
        if (n >= sizeof(seg[i]))
            n = sizeof(seg[i]) - 1u;
        memcpy(seg[i], s, n);
        seg[i][n] = '\0';
    }

    if (radial) {
        char centre[64];
        trim_lower(seg[0], centre, sizeof(centre));
        /* 去掉空白后比较（Python 侧是 replace(" ","")） */
        char squeeze[64];
        size_t k = 0;
        for (size_t i = 0; centre[i] != '\0' && k + 1u < sizeof(squeeze); i++)
            if (!is_space(centre[i]))
                squeeze[k++] = centre[i];
        squeeze[k] = '\0';
        if (strcmp(squeeze, "circle") != 0 && strcmp(squeeze, "circleatcenter") != 0 &&
            strcmp(squeeze, "ellipse") != 0 && strcmp(squeeze, "50%") != 0 &&
            strcmp(squeeze, "center") != 0) {
            set_error("%s: radial-gradient 只支持圆心在元素中心（收到 '%s'）", where,
                      seg[0]);
            return 0;
        }
        *dir = 3;
    } else {
        *dir = gradient_dir_of(seg[0]);
        if (*dir == 0) {
            set_error("%s: 不支持的渐变方向 '%s'（支持 to bottom/right、180deg/90deg）",
                      where, seg[0]);
            return 0;
        }
    }

    uint8_t a0 = 0, a1 = 0;
    if (!parse_color(seg[1], where, from, &a0) || !parse_color(seg[2], where, to, &a1))
        return 0;
    return 1;
}

/* box-shadow: <dx> <dy> [blur] <color> */
static int parse_shadow(const char *text, const char *where, int *dx, int *dy,
                        int *blur, uint32_t *rgb, uint8_t *alpha)
{
    *dx = *dy = *blur = 0;
    *rgb = 0;
    *alpha = 0;

    /* 把连续空白压成一个空格（Python 侧是 " ".join(split())） */
    char t[192];
    size_t k = 0;
    int prev_space = 0;
    const char *s = text;
    while (is_space(*s))
        s++;
    for (; *s != '\0' && k + 1u < sizeof(t); s++) {
        if (is_space(*s)) {
            if (!prev_space) {
                t[k++] = ' ';
                prev_space = 1;
            }
        } else {
            t[k++] = *s;
            prev_space = 0;
        }
    }
    while (k > 0 && t[k - 1] == ' ')
        k--;
    t[k] = '\0';
    if (strcmp(t, "none") == 0)
        return 1;

    /*
     * 颜色：先抠出来（rgba(...) 里有空格，不能简单 split）。
     * 顺序必须与 Python 一致：先找 rgba()/rgb()，再找 #hex，最后才找"第一段字母"。
     * 顺序错了会把 `1px` 里的 "px" 当颜色（真踩过）。
     */
    char color_txt[128] = "";
    char rest[192];
    size_t cstart = 0, clen = 0;

    {
        const char *p = NULL;
        for (const char *q = t; *q != '\0'; q++) {
            if ((q == t || q[-1] == ' ') && strncmp(q, "rgba(", 5) == 0) {
                p = q;
                break;
            }
            if ((q == t || q[-1] == ' ') && strncmp(q, "rgb(", 4) == 0) {
                p = q;
                break;
            }
        }
        if (p != NULL) {
            const char *close = strchr(p, ')');
            if (close != NULL) {
                cstart = (size_t)(p - t);
                clen = (size_t)(close - p) + 1u;
            }
        }
    }
    if (clen == 0) {
        /* #hex */
        const char *hash = strchr(t, '#');
        if (hash != NULL) {
            size_t n = 1;
            while (hex_value(hash[n]) >= 0)
                n++;
            if (n > 1) {
                cstart = (size_t)(hash - t);
                clen = n;
            }
        }
    }
    if (clen == 0) {
        /* 第一段字母 */
        for (const char *q = t; *q != '\0'; q++) {
            if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) {
                size_t n = 0;
                while (q[n] != '\0' &&
                       ((q[n] >= 'a' && q[n] <= 'z') || (q[n] >= 'A' && q[n] <= 'Z')))
                    n++;
                cstart = (size_t)(q - t);
                clen = n;
                break;
            }
        }
    }
    if (clen == 0 || clen >= sizeof(color_txt)) {
        set_error("%s: box-shadow 缺少颜色（不支持默认色）", where);
        return 0;
    }
    memcpy(color_txt, t + cstart, clen);
    color_txt[clen] = '\0';

    /* rest = 去掉颜色后的部分 */
    size_t rn = 0;
    for (size_t i = 0; i < k; i++) {
        if (i >= cstart && i < cstart + clen)
            continue;
        rest[rn++] = t[i];
    }
    rest[rn] = '\0';

    int nums[3] = { 0, 0, 0 };
    int nn = 0;
    const char *p = rest;
    while (*p != '\0' && nn < 4) {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        char tok[64];
        size_t len = (size_t)(p - start);
        if (len >= sizeof(tok)) {
            set_error("%s: box-shadow 里无法识别 '%s'", where, tok);
            return 0;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';
        if (strcmp(tok, "0") == 0) {
            nums[nn++] = 0;
            continue;
        }
        size_t tn = strlen(tok);
        if (tn > 2 && strcmp(tok + tn - 2u, "px") == 0) {
            char num[64];
            memcpy(num, tok, tn - 2u);
            num[tn - 2u] = '\0';
            const char *end = NULL;
            double v = 0;
            if (match_number(num, &end, &v) && *end == '\0') {
                nums[nn++] = (int)py_round(v);
                continue;
            }
        }
        set_error("%s: box-shadow 里无法识别 '%s'", where, tok);
        return 0;
    }
    if (nn < 2 || nn > 3) {
        set_error("%s: box-shadow 需要 2 或 3 个长度，收到 %d 个", where, nn);
        return 0;
    }
    *dx = nums[0];
    *dy = nums[1];
    *blur = (nn == 3) ? nums[2] : 0;
    if (*blur < 0) {
        set_error("%s: box-shadow 的模糊半径不能为负", where);
        return 0;
    }
    uint8_t a = 0;
    return parse_color(color_txt, where, rgb, &a) ? (*alpha = a, 1) : 0;
}

/* transform 里的 px 参数：`-40px` 或 `0` */
static int tf_px(const char *token, const char *where, int *out)
{
    char t[64];
    trim_lower(token, t, sizeof(t));
    size_t n = strlen(t);
    if (n > 2 && strcmp(t + n - 2u, "px") == 0) {
        char num[64];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *out = (int)py_round(v);
            return 1;
        }
    } else if (strcmp(t, "0") == 0) {
        *out = 0;
        return 1;
    }
    set_error("%s: transform 的长度必须带 px 单位（拿到 '%s'）", where, token);
    return 0;
}

/* 缩放倍数 → 千分比。0 合法（翻转动画的中间帧） */
static int tf_scale(const char *token, const char *where, const char *fn, int *out)
{
    double v = 0;
    const char *end = NULL;
    const char *p = skip_space(token);
    if (!match_number(p, &end, &v) || *skip_space(end) != '\0') {
        set_error("%s: %s 的参数不是数字（收到 '%s'）", where, fn, token);
        return 0;
    }
    long sc = py_round(v * 1000.0);
    if (sc < 0 || sc > 32767) {
        set_error("%s: scale 倍数必须在 0~32.767（收到 '%s'）", where, fn);
        return 0;
    }
    *out = (int)sc;
    return 1;
}

/*
 * transform：translate / translateX / translateY / scale / scaleX / scaleY。
 * rotate/skew/matrix/3d 需要非轴对齐光栅化，明确报错（不静默降级）。
 */
static int parse_transform(const char *text, const char *where, int *tx, int *ty,
                           int *scx, int *scy)
{
    *tx = *ty = 0;
    *scx = *scy = 1000;

    char t[192];
    size_t k = 0;
    int prev_space = 0;
    const char *s = skip_space(text);
    for (; *s != '\0' && k + 1u < sizeof(t); s++) {
        if (is_space(*s)) {
            if (!prev_space) {
                t[k++] = ' ';
                prev_space = 1;
            }
        } else {
            t[k++] = *s;
            prev_space = 0;
        }
    }
    while (k > 0 && t[k - 1] == ' ')
        k--;
    t[k] = '\0';

    char lower[192];
    trim_lower(t, lower, sizeof(lower));
    if (strcmp(lower, "none") == 0)
        return 1;

    int seen = 0;
    const char *p = t;
    while ((p = strchr(p, '(')) != NULL) {
        /* 函数名：从 p 往回退到非字母 */
        const char *name_end = p;
        const char *name_start = p;
        while (name_start > t && ((name_start[-1] >= 'a' && name_start[-1] <= 'z') ||
                                  (name_start[-1] >= 'A' && name_start[-1] <= 'Z')))
            name_start--;
        const char *close = strchr(p, ')');
        if (close == NULL) {
            set_error("%s: transform 缺少右括号", where);
            return 0;
        }
        char fn[32];
        size_t fnlen = (size_t)(name_end - name_start);
        if (fnlen >= sizeof(fn))
            fnlen = sizeof(fn) - 1u;
        for (size_t i = 0; i < fnlen; i++) {
            char c = name_start[i];
            fn[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        fn[fnlen] = '\0';

        /* 参数按 ',' 切 */
        char args[4][64];
        int nargs = 0;
        const char *ap = p + 1;
        while (ap <= close && nargs < 4) {
            const char *comma = ap;
            while (comma < close && *comma != ',')
                comma++;
            size_t len = (size_t)(comma - ap);
            while (len > 0 && is_space(*ap)) {
                ap++;
                len--;
            }
            while (len > 0 && is_space(ap[len - 1]))
                len--;
            if (len >= sizeof(args[0]))
                len = sizeof(args[0]) - 1u;
            if (len > 0 || nargs == 0) {
                memcpy(args[nargs], ap, len);
                args[nargs][len] = '\0';
                nargs++;
            }
            if (comma >= close)
                break;
            ap = comma + 1;
        }

        if (strcmp(fn, "translate") == 0) {
            if (nargs < 1 || !tf_px(args[0], where, tx))
                return 0;
            if (nargs > 1 && !tf_px(args[1], where, ty))
                return 0;
        } else if (strcmp(fn, "translatex") == 0) {
            if (nargs < 1 || !tf_px(args[0], where, tx))
                return 0;
        } else if (strcmp(fn, "translatey") == 0) {
            if (nargs < 1 || !tf_px(args[0], where, ty))
                return 0;
        } else if (strcmp(fn, "scale") == 0) {
            if (nargs < 1)
                return 0;
            if (!tf_scale(args[0], where, "scale", scx))
                return 0;
            if (nargs > 1) {
                if (!tf_scale(args[1], where, "scale", scy))
                    return 0;
            } else {
                *scy = *scx;
            }
        } else if (strcmp(fn, "scalex") == 0) {
            if (nargs < 1 || !tf_scale(args[0], where, "scaleX", scx))
                return 0;
        } else if (strcmp(fn, "scaley") == 0) {
            if (nargs < 1 || !tf_scale(args[0], where, "scaleY", scy))
                return 0;
        } else {
            set_error("%s: transform 只支持 translate()/scale()/scaleX()/scaleY()"
                      "（收到 '%s'）", where, fn);
            return 0;
        }
        seen++;
        p = close + 1;
    }

    if (seen == 0)
        set_error("%s: 无法解析的 transform '%s'", where, text);
    return seen > 0;
}

/* clip-path: circle(<pct>%) → (kind, 千分比) */
static int parse_clip_path(const char *text, const char *where, int *kind, int *r)
{
    *kind = 0;
    *r = 0;

    /* 去掉所有空白并转小写（Python: re.sub(r"\s+", "", ...)） */
    char t[128];
    size_t k = 0;
    const char *s = text;
    while (*s != '\0' && k + 1u < sizeof(t)) {
        if (!is_space(*s)) {
            char c = *s;
            t[k++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        s++;
    }
    t[k] = '\0';

    if (strcmp(t, "none") == 0 || t[0] == '\0')
        return 1;
    if (strncmp(t, "circle(", 7) != 0 || t[k - 1] != ')') {
        set_error("%s: clip-path 只支持 circle(<百分比>)（0%%~100%%），收到 '%s'", where,
                  text);
        return 0;
    }
    /* 中间必须形如 "<num>%" */
    const char *inner = t + 7;
    size_t n = strlen(inner) - 1u;      /* 去掉 ')' */
    if (n < 2 || inner[n - 1u] != '%') {
        set_error("%s: clip-path 只支持 circle(<百分比>)（0%%~100%%），收到 '%s'", where,
                  text);
        return 0;
    }
    char num[64];
    if (n - 1u >= sizeof(num)) {
        set_error("%s: clip-path 的百分比太长", where);
        return 0;
    }
    memcpy(num, inner, n - 1u);
    num[n - 1u] = '\0';
    double v = 0;
    const char *end = NULL;
    /* Python 的正则是 [0-9.]+（不带负号） */
    if (num[0] == '-' || !match_number(num, &end, &v) || *end != '\0') {
        set_error("%s: clip-path 只支持 circle(<百分比>)（0%%~100%%），收到 '%s'", where,
                  text);
        return 0;
    }
    if (v < 0.0 || v > 100.0) {
        set_error("%s: clip-path circle() 的百分比必须在 0~100（收到 '%s'）", where,
                  text);
        return 0;
    }
    *kind = 1;
    *r = (int)py_round(v * 10.0);
    return 1;
}

/* `200ms` / `1.5s` → 毫秒 */
static int parse_duration(const char *text, const char *where, const char *prop,
                          int *ms)
{
    char t[64];
    trim_lower(text, t, sizeof(t));
    size_t n = strlen(t);
    if (n > 2 && strcmp(t + n - 2u, "ms") == 0) {
        char num[64];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        double v = 0;
        const char *end = NULL;
        if (match_number(num, &end, &v) && *end == '\0') {
            long r = py_round(v);
            if (r < 0 || r > 65535) {
                set_error("%s: %s 超出范围（'%s'）", where, prop, text);
                return 0;
            }
            *ms = (int)r;
            return 1;
        }
    } else if (n > 1 && t[n - 1u] == 's') {
        char num[64];
        memcpy(num, t, n - 1u);
        num[n - 1u] = '\0';
        double v = 0;
        const char *end = NULL;
        if (match_number(num, &end, &v) && *end == '\0') {
            long r = py_round(v * 1000.0);
            if (r < 0 || r > 65535) {
                set_error("%s: %s 超出范围（'%s'）", where, prop, text);
                return 0;
            }
            *ms = (int)r;
            return 1;
        }
    }
    set_error("%s: %s 需要 ms/s 单位（'%s'）", where, prop, text);
    return 0;
}

/* flex: 1 / 1 0 auto / 0 0 100px / none / auto → grow, shrink, basis(kind,val) */
static int parse_flex_shorthand(const char *text, const char *where, double *grow,
                                double *shrink, int *bk, double *bv)
{
    char t[128];
    trim_lower(text, t, sizeof(t));
    if (strcmp(t, "none") == 0) {
        *grow = 0;
        *shrink = 0;
        *bk = NUM_AUTO;
        *bv = 0;
        return 1;
    }
    if (strcmp(t, "auto") == 0) {
        *grow = 1;
        *shrink = 1;
        *bk = NUM_AUTO;
        *bv = 0;
        return 1;
    }

    char parts[3][64];
    int np = 0;
    const char *p = t;
    while (*p != '\0' && np < 3) {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(parts[0])) {
            set_error("%s: flex 的值太长", where);
            return 0;
        }
        memcpy(parts[np], start, len);
        parts[np][len] = '\0';
        np++;
    }
    if (np < 1) {
        set_error("%s: flex 只能是 1~3 个值，收到 '%s'", where, text);
        return 0;
    }

    if (np == 1) {
        int kind = 0;
        double val = 0;
        if (!parse_number(parts[0], where, 1, 1, &kind, &val))
            return 0;
        if (kind != NUM_NUM) {
            *grow = 1;
            *shrink = 1;
            *bk = kind;
            *bv = val;
            return 1;
        }
        *grow = val;
        *shrink = 1;
        *bk = NUM_AUTO;
        *bv = 0;
        return 1;
    }
    if (np == 2) {
        int kind = 0;
        double val = 0;
        if (!parse_number(parts[0], where, 1, 1, &kind, &val))
            return 0;
        if (kind == NUM_NUM) {
            double g = val;
            if (!parse_number(parts[1], where, 1, 1, &kind, &val))
                return 0;
            if (kind != NUM_NUM) {
                *grow = g;
                *shrink = 1;
                *bk = kind;
                *bv = val;
                return 1;
            }
            *grow = g;
            *shrink = val;
            *bk = NUM_AUTO;
            *bv = 0;
            return 1;
        }
        /* 第一个是 basis、第二个是 grow/shrink 的写法不支持（Python 会走 num() 报错） */
        set_error("%s: flex 里的 '%s' 不是数字", where, parts[0]);
        return 0;
    }

    {
        int kind = 0;
        double val = 0;
        double g = 0, sh = 0;
        int k0 = 0, k1 = 0;
        double v0 = 0, v1 = 0;
        if (!parse_number(parts[0], where, 1, 1, &k0, &v0) || k0 != NUM_NUM) {
            set_error("%s: flex 里的 '%s' 不是数字", where, parts[0]);
            return 0;
        }
        g = v0;
        if (!parse_number(parts[1], where, 1, 1, &k1, &v1) || k1 != NUM_NUM) {
            set_error("%s: flex 里的 '%s' 不是数字", where, parts[1]);
            return 0;
        }
        sh = v1;
        if (!parse_number(parts[2], where, 1, 1, &kind, &val))
            return 0;
        *grow = g;
        *shrink = sh;
        *bk = kind;
        *bv = val;
    }
    return 1;
}

/* gap: <row> [<col>] → 两个 px */
static int parse_gap(const char *text, const char *where, int *gr, int *gc)
{
    char parts[2][64];
    int np = 0;
    const char *p = text;
    while (*p != '\0' && np < 2) {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(parts[0])) {
            set_error("%s: gap 的值太长", where);
            return 0;
        }
        memcpy(parts[np], start, len);
        parts[np][len] = '\0';
        np++;
    }
    if (np == 1) {
        int kind = 0;
        double v = 0;
        if (!parse_number(parts[0], where, 0, 0, &kind, &v))
            return 0;
        if (kind == NUM_PCT) {
            set_error("%s: gap 不支持百分比", where);
            return 0;
        }
        *gr = *gc = (int)v;
        return 1;
    }
    if (np == 2) {
        int k1 = 0, k2 = 0;
        double v1 = 0, v2 = 0;
        if (!parse_number(parts[0], where, 0, 0, &k1, &v1) ||
            !parse_number(parts[1], where, 0, 0, &k2, &v2))
            return 0;
        if (k1 == NUM_PCT || k2 == NUM_PCT) {
            set_error("%s: gap 不支持百分比", where);
            return 0;
        }
        *gr = (int)v1;
        *gc = (int)v2;
        return 1;
    }
    set_error("%s: gap 只支持 1~2 个值，收到 '%s'", where, text);
    return 0;
}

/* ============================== ③ 声明集合 ============================== */

typedef struct {
    char *key;
    char *val;
} Decl;

typedef struct {
    Decl *d;
    int   n;
    int   cap;
} Decls;

static void decls_free(Decls *ds)
{
    for (int i = 0; i < ds->n; i++) {
        ql_mem_free(ds->d[i].key);
        ql_mem_free(ds->d[i].val);
    }
    ql_mem_free(ds->d);
    ds->d = NULL;
    ds->n = ds->cap = 0;
}

static const char *decls_get(const Decls *ds, const char *key)
{
    for (int i = 0; i < ds->n; i++)
        if (strcmp(ds->d[i].key, key) == 0)
            return ds->d[i].val;
    return NULL;
}

/* set 语义：已存在则改值（保持首次插入位置），否则追加 */
static int decls_set(Decls *ds, const char *key, const char *val)
{
    for (int i = 0; i < ds->n; i++) {
        if (strcmp(ds->d[i].key, key) == 0) {
            char *nv = xstrdup(val);
            if (nv == NULL)
                return 0;
            ql_mem_free(ds->d[i].val);
            ds->d[i].val = nv;
            return 1;
        }
    }
    if (ds->n >= (int)QL_CFG_UI_MAX_DECLS) {
        set_error("单条规则/元素的声明数超过上限 %d", (int)QL_CFG_UI_MAX_DECLS);
        return 0;
    }
    if (ds->n == ds->cap) {
        int cap = (ds->cap > 0) ? ds->cap * 2 : 8;
        Decl *nd = (Decl *)ql_mem_realloc(ds->d, (size_t)cap * sizeof(Decl));
        if (nd == NULL)
            return 0;
        ds->d = nd;
        ds->cap = cap;
    }
    char *nk = xstrdup(key);
    char *nv = xstrdup(val);
    if (nk == NULL || nv == NULL) {
        ql_mem_free(nk);
        ql_mem_free(nv);
        return 0;
    }
    ds->d[ds->n].key = nk;
    ds->d[ds->n].val = nv;
    ds->n++;
    return 1;
}

static int decls_remove(Decls *ds, const char *key)
{
    for (int i = 0; i < ds->n; i++) {
        if (strcmp(ds->d[i].key, key) == 0) {
            ql_mem_free(ds->d[i].key);
            ql_mem_free(ds->d[i].val);
            for (int j = i + 1; j < ds->n; j++)
                ds->d[j - 1] = ds->d[j];
            ds->n--;
            return 1;
        }
    }
    return 0;
}

static int decls_copy(Decls *dst, const Decls *src)
{
    for (int i = 0; i < src->n; i++)
        if (!decls_set(dst, src->d[i].key, src->d[i].val))
            return 0;
    return 1;
}

/* ---------------------- 属性名与值的小工具 ---------------------- */

static int str_in(const char *s, const char *const *list, int n)
{
    for (int i = 0; i < n; i++)
        if (strcmp(s, list[i]) == 0)
            return 1;
    return 0;
}

static const char *const k_easings[] = { "linear", "ease-in", "ease-out", "ease-in-out",
                                         "ease" };

/* 缓动的枚举值（与 QLEasing 对应） */
static int easing_value(const char *name, int *out)
{
    if (strcmp(name, "linear") == 0) {
        *out = QL_EASE_LINEAR;
        return 1;
    }
    if (strcmp(name, "ease-in") == 0) {
        *out = QL_EASE_IN;
        return 1;
    }
    if (strcmp(name, "ease-out") == 0) {
        *out = QL_EASE_OUT;
        return 1;
    }
    if (strcmp(name, "ease-in-out") == 0) {
        *out = QL_EASE_IN_OUT;
        return 1;
    }
    if (strcmp(name, "ease") == 0) {
        *out = QL_EASE_CSS;
        return 1;
    }
    return 0;
}

static int is_all_digits(const char *s)
{
    if (*s == '\0')
        return 0;
    for (const char *p = s; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

/* 是否是 "-?[0-9.]+m?s" 形式（Python 侧用来识别"时长"） */
static int looks_like_duration(const char *s)
{
    const char *p = s;
    if (*p == '-')
        p++;
    int digits = 0;
    while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (*p >= '0' && *p <= '9')
            digits++;
        p++;
    }
    if (digits == 0)
        return 0;
    if (p[0] == 'm' && p[1] == 's' && p[2] == '\0')
        return 1;
    if (p[0] == 's' && p[1] == '\0')
        return 1;
    return 0;
}

/* 把 "1.0" 这类数字按 Python 的 "%g" 输出（6 位有效数字，去掉尾零） */
static void fmt_g(char *out, size_t cap, double v)
{
    snprintf(out, cap, "%g", v);
}

/* ======================= 简写展开（**必须在单条规则内**做） ======================= */
/*
 * 级联合并之后再展开会覆盖后出现的长写：
 *   `.a { animation: pulse 1s }` 与 `.b { animation-name: other }` 是两条规则，
 * 合并后 animation-name 已是 other，这时再展开 shorthand 会把它改回 pulse ——
 * 10 张卡片全部退化成同一段关键帧（真踩过）。所以只在 parse_decls 里展开。
 */
static int expand_shorthands(Decls *ds)
{
    const char *v = NULL;

    if ((v = decls_get(ds, "transition")) != NULL) {
        /* 时长；两个时长 = 第二个是 delay（不支持 → 报错） */
        char buf[192];
        size_t n = strlen(v);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1u;
        memcpy(buf, v, n);
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++)
            if (buf[i] == ',')
                buf[i] = ' ';

        char dur[128] = "";
        char tf[128] = "";
        const char *p = buf;
        while (*p != '\0') {
            while (is_space(*p))
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && !is_space(*p))
                p++;
            char tok[128];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(tok))
                len = sizeof(tok) - 1u;
            memcpy(tok, start, len);
            tok[len] = '\0';
            char low[128];
            trim_lower(tok, low, sizeof(low));

            if (strcmp(low, "all") == 0 || strcmp(low, "background-color") == 0 ||
                strcmp(low, "opacity") == 0 || strcmp(low, "color") == 0 ||
                strcmp(low, "border-color") == 0 || strcmp(low, "border-radius") == 0 ||
                strcmp(low, "transform") == 0 || strcmp(low, "clip-path") == 0) {
                if (!decls_set(ds, "transition-property", low))
                    return 0;
            } else if (str_in(low, k_easings, 5)) {
                snprintf(tf, sizeof(tf), "%s", low);
            } else if (looks_like_duration(low)) {
                if (dur[0] == '\0')
                    snprintf(dur, sizeof(dur), "%s", low);
                else {
                    set_error("transition: 暂不支持 delay（'%s'）", tok);
                    return 0;
                }
            } else {
                set_error("transition: 无法识别的部分 '%s'", tok);
                return 0;
            }
        }
        if (dur[0] != '\0' && !decls_set(ds, "transition-duration", dur))
            return 0;
        if (tf[0] != '\0' && !decls_set(ds, "transition-timing-function", tf))
            return 0;
        (void)decls_remove(ds, "transition");
    }

    if ((v = decls_get(ds, "animation")) != NULL) {
        if (strstr(v, "steps(") != NULL || strstr(v, "cubic-bezier") != NULL) {
            set_error("animation: 暂不支持 steps()/cubic-bezier()，"
                      "请用 linear / ease / ease-in / ease-out / ease-in-out");
            return 0;
        }
        char buf[192];
        size_t n = strlen(v);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1u;
        memcpy(buf, v, n);
        buf[n] = '\0';

        const char *p = buf;
        while (*p != '\0') {
            while (is_space(*p))
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && !is_space(*p))
                p++;
            char tok[128];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(tok))
                len = sizeof(tok) - 1u;
            memcpy(tok, start, len);
            tok[len] = '\0';
            char low[128];
            trim_lower(tok, low, sizeof(low));

            if (looks_like_duration(low)) {
                if (decls_get(ds, "animation-duration") == NULL) {
                    if (!decls_set(ds, "animation-duration", low))
                        return 0;
                } else if (decls_get(ds, "animation-delay") == NULL) {
                    if (!decls_set(ds, "animation-delay", low))
                        return 0;
                } else {
                    set_error("animation: 时长写了三个（'%s'）", tok);
                    return 0;
                }
            } else if (strcmp(low, "infinite") == 0 || is_all_digits(low)) {
                if (!decls_set(ds, "animation-iteration-count", low))
                    return 0;
            } else if (str_in(low, k_easings, 5)) {
                if (!decls_set(ds, "animation-timing-function", low))
                    return 0;
            } else if (strcmp(low, "normal") == 0 || strcmp(low, "reverse") == 0 ||
                       strcmp(low, "alternate") == 0 ||
                       strcmp(low, "alternate-reverse") == 0) {
                if (!decls_set(ds, "animation-direction", low))
                    return 0;
            } else if (strcmp(low, "forwards") == 0 || strcmp(low, "backwards") == 0 ||
                       strcmp(low, "both") == 0) {
                if (!decls_set(ds, "animation-fill-mode", low))
                    return 0;
            } else if (strcmp(low, "running") == 0 || strcmp(low, "paused") == 0) {
                if (strcmp(low, "paused") == 0) {
                    set_error("animation-play-state: 暂不支持 paused");
                    return 0;
                }
            } else if (strcmp(low, "none") == 0) {
                if (!decls_set(ds, "animation-name", "none"))
                    return 0;
            } else {
                if (!decls_set(ds, "animation-name", low))
                    return 0;
            }
        }
        (void)decls_remove(ds, "animation");
    }

    if ((v = decls_get(ds, "flex")) != NULL) {
        double g = 0, sh = 0, bv = 0;
        int bk = 0;
        if (!parse_flex_shorthand(v, "flex", &g, &sh, &bk, &bv))
            return 0;
        char nb[32];
        fmt_g(nb, sizeof(nb), g);
        if (!decls_set(ds, "flex-grow", nb))
            return 0;
        fmt_g(nb, sizeof(nb), sh);
        if (!decls_set(ds, "flex-shrink", nb))
            return 0;
        if (bk == NUM_AUTO) {
            if (!decls_set(ds, "flex-basis", "auto"))
                return 0;
        } else if (bk == NUM_PCT) {
            snprintf(nb, sizeof(nb), "%d%%", (int)bv);
            if (!decls_set(ds, "flex-basis", nb))
                return 0;
        } else {
            snprintf(nb, sizeof(nb), "%dpx", (int)bv);
            if (!decls_set(ds, "flex-basis", nb))
                return 0;
        }
        (void)decls_remove(ds, "flex");
    }

    if ((v = decls_get(ds, "gap")) != NULL) {
        int gr = 0, gc = 0;
        if (!parse_gap(v, "gap", &gr, &gc))
            return 0;
        char nb[32];
        snprintf(nb, sizeof(nb), "%dpx", gr);
        if (!decls_set(ds, "row-gap", nb))
            return 0;
        snprintf(nb, sizeof(nb), "%dpx", gc);
        if (!decls_set(ds, "column-gap", nb))
            return 0;
        (void)decls_remove(ds, "gap");
    }

    if ((v = decls_get(ds, "background")) != NULL) {
        char *copy = xstrdup(v);
        if (copy == NULL)
            return 0;
        (void)decls_remove(ds, "background");
        int ok;
        if (strstr(copy, "gradient(") != NULL)
            ok = decls_set(ds, "background-image", copy);
        else
            ok = decls_set(ds, "background-color", copy);
        ql_mem_free(copy);
        if (!ok)
            return 0;
    }

    if ((v = decls_get(ds, "border")) != NULL) {
        char buf[128];
        size_t n = strlen(v);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1u;
        memcpy(buf, v, n);
        buf[n] = '\0';
        (void)decls_remove(ds, "border");

        const char *p = buf;
        while (*p != '\0') {
            while (is_space(*p))
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && !is_space(*p))
                p++;
            char tok[64];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(tok))
                len = sizeof(tok) - 1u;
            memcpy(tok, start, len);
            tok[len] = '\0';
            char low[64];
            trim_lower(tok, low, sizeof(low));

            size_t ln = strlen(low);
            if (ln > 2 && strcmp(low + ln - 2u, "px") == 0) {
                if (!decls_set(ds, "border-width", low))
                    return 0;
            } else if (strcmp(low, "solid") == 0 || strcmp(low, "none") == 0 ||
                       strcmp(low, "dashed") == 0 || strcmp(low, "dotted") == 0) {
                if (strcmp(low, "solid") != 0) {
                    set_error("只支持 solid 边框，收到 '%s'", low);
                    return 0;
                }
            } else {
                if (!decls_set(ds, "border-color", low))
                    return 0;
            }
        }
    }

    return 1;
}

/* ============================== ④ CSS 词法 ============================== */

static const char *const k_supported_props[] = {
    "display", "width", "height", "margin", "padding", "border", "border-width",
    "border-color", "border-radius", "background", "background-color", "color",
    "font-size", "text-align", "line-height", "white-space", "box-sizing",
    "box-shadow", "font-weight", "font-style", "text-decoration",
    "flex", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis",
    "justify-content", "align-items", "gap", "row-gap", "column-gap",
    "position", "top", "right", "bottom", "left", "z-index", "overflow",
    "transform", "clip-path", "backdrop-filter",
    "transition", "transition-duration", "transition-timing-function",
    "transition-property",
    "animation", "animation-name", "animation-duration",
    "animation-timing-function", "animation-iteration-count",
    "animation-direction", "animation-fill-mode", "animation-delay",
    "animation-play-state",
    "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding-top", "padding-right", "padding-bottom", "padding-left",
    "background-image",
};

/* @keyframes 里允许的属性（与 Python 的 KEYFRAME_PROPS 一致） */
static const char *const k_keyframe_props[] = {
    "background", "background-color", "background-image", "opacity", "transform",
    "color", "border-color", "border-radius", "clip-path",
    "animation-timing-function",
};

static int in_supported_props(const char *prop)
{
    for (size_t i = 0; i < sizeof(k_supported_props) / sizeof(k_supported_props[0]);
         i++)
        if (strcmp(prop, k_supported_props[i]) == 0)
            return 1;
    return 0;
}

static int in_keyframe_props(const char *prop)
{
    for (size_t i = 0; i < sizeof(k_keyframe_props) / sizeof(k_keyframe_props[0]);
         i++)
        if (strcmp(prop, k_keyframe_props[i]) == 0)
            return 1;
    return 0;
}

/* 从 open_pos（指向 '{'）找到配对的 '}'，返回它的下标；找不到返回 -1 */
static long match_brace(const char *text, size_t open_pos, const char *where)
{
    int depth = 0;
    for (size_t i = open_pos; text[i] != '\0'; i++) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0)
                return (long)i;
        }
    }
    set_error("%s: 规则缺少右花括号", where);
    return -1;
}

/*
 * 解析 `a: b; c: d`。
 * allowed != NULL 时是 @keyframes 的声明块（属性白名单更窄、**不展开简写**）。
 */
static int parse_decls(const char *body, size_t len, const char *where,
                       int keyframe_block, Decls *out)
{
    size_t pos = 0;
    while (pos <= len) {
        size_t end = pos;
        while (end < len && body[end] != ';')
            end++;
        /* piece = body[pos..end) */
        const char *piece = body + pos;
        size_t plen = end - pos;

        while (plen > 0 && is_space(*piece)) {
            piece++;
            plen--;
        }
        while (plen > 0 && is_space(piece[plen - 1]))
            plen--;

        if (plen > 0) {
            const char *colon = memchr(piece, ':', plen);
            if (colon == NULL) {
                set_error("%s: 声明缺少冒号", where);
                return 0;
            }
            size_t klen = (size_t)(colon - piece);
            while (klen > 0 && is_space(piece[klen - 1]))
                klen--;
            char prop[64];
            if (klen >= sizeof(prop)) {
                set_error("%s: 属性名太长", where);
                return 0;
            }
            for (size_t i = 0; i < klen; i++) {
                char c = piece[i];
                prop[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            prop[klen] = '\0';

            const char *val = colon + 1;
            size_t vlen = plen - (size_t)(colon - piece) - 1u;
            while (vlen > 0 && is_space(*val)) {
                val++;
                vlen--;
            }
            while (vlen > 0 && is_space(val[vlen - 1]))
                vlen--;
            char *vs = xstrndup(val, vlen);
            if (vs == NULL)
                return 0;

            int ok;
            if (keyframe_block) {
                if (!in_keyframe_props(prop)) {
                    set_error("%s: @keyframes 里不支持的属性 '%s'", where, prop);
                    ql_mem_free(vs);
                    return 0;
                }
                ok = decls_set(out, prop, vs);
            } else {
                if (!in_supported_props(prop)) {
                    set_error("%s: 不支持的属性 '%s'", where, prop);
                    ql_mem_free(vs);
                    return 0;
                }
                ok = decls_set(out, prop, vs);
            }
            ql_mem_free(vs);
            if (!ok)
                return 0;
        }

        if (end >= len)
            break;
        pos = end + 1;
    }

    return keyframe_block ? 1 : expand_shorthands(out);
}

/* --------------------------- 选择器 --------------------------- */

#define SEL_COMB_DESCENDANT 0
#define SEL_COMB_CHILD      1

typedef struct {
    int    comb;          /* 与**左边**那一段的组合符 */
    char  *tag;           /* NULL = 不限定 */
    char  *id;            /* NULL = 不限定 */
    char **classes;
    int    nclasses;
    int    states;
} SelPart;

typedef struct {
    SelPart *parts;
    int      nparts;
    int      spec_ids;
    int      spec_cls;
    int      spec_tags;
    int      states;
    Decls    decls;
    int      order;
} Rule;

typedef struct {
    Rule *r;
    int   n;
    int   cap;
} Rules;

static void selpart_free(SelPart *p)
{
    ql_mem_free(p->tag);
    ql_mem_free(p->id);
    for (int i = 0; i < p->nclasses; i++)
        ql_mem_free(p->classes[i]);
    ql_mem_free(p->classes);
    memset(p, 0, sizeof(*p));
}

static void rule_free(Rule *r)
{
    for (int i = 0; i < r->nparts; i++)
        selpart_free(&r->parts[i]);
    ql_mem_free(r->parts);
    decls_free(&r->decls);
    memset(r, 0, sizeof(*r));
}

static void rules_free(Rules *rs)
{
    for (int i = 0; i < rs->n; i++)
        rule_free(&rs->r[i]);
    ql_mem_free(rs->r);
    rs->r = NULL;
    rs->n = rs->cap = 0;
}

static int selpart_add_class(SelPart *p, const char *s, size_t len)
{
    char *name = xstrndup(s, len);
    if (name == NULL)
        return 0;
    char **nc = (char **)ql_mem_realloc(p->classes,
                                        (size_t)(p->nclasses + 1) * sizeof(char *));
    if (nc == NULL) {
        ql_mem_free(name);
        return 0;
    }
    p->classes = nc;
    p->classes[p->nclasses++] = name;
    return 1;
}

/* 解析一个"简单选择器"token，写进 p，并累计 specificity */
static int parse_simple_selector(const char *tok, SelPart *p, int *spec_ids,
                                 int *spec_cls, int *spec_tags)
{
    const char *s = tok;
    int anything = 0;

    /* 标签：字母开头，后接 [A-Za-z0-9_-] */
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) {
        const char *start = s;
        s++;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')
            s++;
        size_t len = (size_t)(s - start);
        char *tag = xstrndup(start, len);
        if (tag == NULL)
            return 0;
        for (size_t i = 0; i < len; i++)
            if (tag[i] >= 'A' && tag[i] <= 'Z')
                tag[i] = (char)(tag[i] - 'A' + 'a');
        p->tag = tag;
        (*spec_tags)++;
        anything = 1;
    }

    /* 类：.name 可重复 */
    while (*s == '.') {
        s++;
        const char *start = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')
            s++;
        if (s == start)
            return 0;
        if (!selpart_add_class(p, start, (size_t)(s - start)))
            return 0;
        (*spec_cls)++;
        anything = 1;
    }

    /* id */
    if (*s == '#') {
        s++;
        const char *start = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')
            s++;
        if (s == start)
            return 0;
        p->id = xstrndup(start, (size_t)(s - start));
        if (p->id == NULL)
            return 0;
        (*spec_ids)++;
        anything = 1;
    }

    /* 伪类：可重复 */
    while (*s == ':') {
        s++;
        const char *start = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-')
            s++;
        if (s == start)
            return 0;
        char name[32];
        size_t len = (size_t)(s - start);
        if (len >= sizeof(name))
            return 0;
        for (size_t i = 0; i < len; i++) {
            char c = start[i];
            name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        name[len] = '\0';
        if (strcmp(name, "hover") == 0)
            p->states |= 1;
        else if (strcmp(name, "active") == 0)
            p->states |= 2;
        else if (strcmp(name, "focus") == 0)
            p->states |= 4;
        else {
            set_error("不支持的伪类 ':%s'（只支持 :hover :active :focus）", name);
            return 0;
        }
        (*spec_cls)++;
        anything = 1;
    }

    if (*s != '\0' || !anything) {
        set_error("不支持的简单选择器：'%s'", tok);
        return 0;
    }
    return 1;
}

/* 解析完整选择器（不含逗号） */
static int parse_selector(const char *sel, Rule *r)
{
    const char *p = skip_space(sel);
    if (*p == '\0')
        return 0;

    int comb = SEL_COMB_DESCENDANT;
    while (*p != '\0') {
        p = skip_space(p);
        if (*p == '\0')
            break;
        if (*p == '>' || *p == '+' || *p == '~') {
            if (*p == '+' || *p == '~') {
                set_error("不支持相邻/兄弟选择器 '%c'", *p);
                return 0;
            }
            if (r->nparts == 0) {
                set_error("选择器不能以组合符开头：'%s'", sel);
                return 0;
            }
            comb = SEL_COMB_CHILD;
            p++;
            continue;
        }

        /* 一个简单选择器 token */
        const char *start = p;
        while (*p != '\0' && !is_space(*p) && *p != '>' && *p != '+' && *p != '~')
            p++;
        char tok[128];
        size_t len = (size_t)(p - start);
        if (len == 0 || len >= sizeof(tok)) {
            set_error("不支持的简单选择器：'%s'", sel);
            return 0;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';

        if (r->nparts >= (int)QL_CFG_UI_MAX_SEL_PARTS) {
            set_error("选择器的段数超过上限 %d", (int)QL_CFG_UI_MAX_SEL_PARTS);
            return 0;
        }
        SelPart part;
        memset(&part, 0, sizeof(part));
        part.comb = comb;
        if (!parse_simple_selector(tok, &part, &r->spec_ids, &r->spec_cls,
                                   &r->spec_tags)) {
            selpart_free(&part);
            return 0;
        }
        r->states |= part.states;

        SelPart *np = (SelPart *)ql_mem_realloc(
            r->parts, (size_t)(r->nparts + 1) * sizeof(SelPart));
        if (np == NULL) {
            selpart_free(&part);
            return 0;
        }
        r->parts = np;
        r->parts[r->nparts++] = part;
        comb = SEL_COMB_DESCENDANT;
    }

    if (r->nparts == 0) {
        set_error("空选择器：'%s'", sel);
        return 0;
    }
    return 1;
}

/* --------------------------- @keyframes --------------------------- */

typedef struct {
    int   offset;
    Decls decls;
    int   has_decls;
} KfRaw;

typedef struct {
    char  *name;
    KfRaw *f;
    int    n;
    int    cap;
} KfDef;

typedef struct {
    KfDef *d;
    int    n;
    int    cap;
} KfDefs;

static void kfdefs_free(KfDefs *k)
{
    for (int i = 0; i < k->n; i++) {
        for (int j = 0; j < k->d[i].n; j++)
            decls_free(&k->d[i].f[j].decls);
        ql_mem_free(k->d[i].f);
        ql_mem_free(k->d[i].name);
    }
    ql_mem_free(k->d);
    k->d = NULL;
    k->n = k->cap = 0;
}

static KfDef *kfdefs_find(KfDefs *k, const char *name)
{
    for (int i = 0; i < k->n; i++)
        if (strcmp(k->d[i].name, name) == 0)
            return &k->d[i];
    return NULL;
}

/* `from` / `to` / `N%` → 0..1000 */
static int kf_offset(const char *sel, const char *where, int *out)
{
    char t[64];
    trim_lower(sel, t, sizeof(t));
    if (strcmp(t, "from") == 0) {
        *out = 0;
        return 1;
    }
    if (strcmp(t, "to") == 0) {
        *out = 1000;
        return 1;
    }
    size_t n = strlen(t);
    if (n > 1 && t[n - 1] == '%') {
        char num[64];
        memcpy(num, t, n - 1u);
        num[n - 1u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (num[0] != '-' && match_number(num, &end, &v) && *end == '\0') {
            if (v < 0.0 || v > 100.0) {
                set_error("%s: @keyframes 的百分比必须在 0%%~100%%（收到 '%s'）", where,
                          sel);
                return 0;
            }
            *out = (int)py_round(v * 10.0);
            return 1;
        }
    }
    set_error("%s: @keyframes 里的选择器只能是 from / to / N%%（收到 '%s'）", where, sel);
    return 0;
}

/* `0% { ... } 50% { ... } to { ... }`，追加到 def */
static int parse_keyframes(const char *body, size_t len, const char *where, KfDef *def)
{
    size_t pos = 0;
    while (pos < len) {
        const char *brace = memchr(body + pos, '{', len - pos);
        if (brace == NULL)
            break;
        size_t bpos = (size_t)(brace - body);
        /* sel = body[pos..bpos) 去空白 */
        const char *sel = body + pos;
        size_t slen = bpos - pos;
        while (slen > 0 && is_space(*sel)) {
            sel++;
            slen--;
        }
        while (slen > 0 && is_space(sel[slen - 1]))
            slen--;
        if (slen == 0) {
            set_error("%s: @keyframes 里有空选择器", where);
            return 0;
        }
        char *selbuf = xstrndup(sel, slen);
        if (selbuf == NULL)
            return 0;

        long end = match_brace(body, bpos, where);
        if (end < 0) {
            ql_mem_free(selbuf);
            return 0;
        }

        Decls decls;
        memset(&decls, 0, sizeof(decls));
        if (!parse_decls(body + bpos + 1u, (size_t)end - bpos - 1u, where, 1, &decls)) {
            decls_free(&decls);
            ql_mem_free(selbuf);
            return 0;
        }

        /* 一个选择器列表可能写多个偏移（`0%, 50% { ... }`） */
        char *save = selbuf;
        for (;;) {
            char *comma = strchr(save, ',');
            if (comma != NULL)
                *comma = '\0';
            int off = 0;
            if (!kf_offset(save, where, &off)) {
                decls_free(&decls);
                ql_mem_free(selbuf);
                return 0;
            }
            if (def->n >= (int)QL_CFG_UI_MAX_KEYFRAMES) {
                set_error("%s: 关键帧数超过上限 %d", where,
                          (int)QL_CFG_UI_MAX_KEYFRAMES);
                decls_free(&decls);
                ql_mem_free(selbuf);
                return 0;
            }
            if (def->n == def->cap) {
                int cap = (def->cap > 0) ? def->cap * 2 : 8;
                KfRaw *nf = (KfRaw *)ql_mem_realloc(def->f,
                                                    (size_t)cap * sizeof(KfRaw));
                if (nf == NULL) {
                    decls_free(&decls);
                    ql_mem_free(selbuf);
                    return 0;
                }
                def->f = nf;
                def->cap = cap;
            }
            memset(&def->f[def->n], 0, sizeof(KfRaw));
            def->f[def->n].offset = off;
            def->f[def->n].has_decls = 1;
            if (!decls_copy(&def->f[def->n].decls, &decls)) {
                decls_free(&decls);
                ql_mem_free(selbuf);
                return 0;
            }
            def->n++;

            if (comma == NULL)
                break;
            save = comma + 1;
        }
        decls_free(&decls);
        ql_mem_free(selbuf);
        pos = (size_t)end + 1u;
    }
    return 1;
}

/* 去掉 C 风格注释（与 Python 侧的 re.sub 一致：非贪婪，跨行） */
static char *strip_comments(const char *css)
{
    size_t len = strlen(css);
    char *out = (char *)xalloc(len + 1u);
    if (out == NULL)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < len;) {
        if (css[i] == '/' && i + 1u < len && css[i + 1u] == '*') {
            size_t j = i + 2u;
            while (j + 1u < len && !(css[j] == '*' && css[j + 1u] == '/'))
                j++;
            i = (j + 1u < len) ? (j + 2u) : len;
        } else {
            out[k++] = css[i++];
        }
    }
    out[k] = '\0';
    return out;
}

/* 解析整份 CSS */
static int parse_css(const char *css, Rules *rules, KfDefs *kfs)
{
    char *text = strip_comments(css);
    if (text == NULL)
        return 0;

    const size_t len = strlen(text);
    size_t pos = 0;
    int order = 0;
    int ok = 1;

    while (pos < len && ok) {
        const char *brace = memchr(text + pos, '{', len - pos);
        if (brace == NULL)
            break;
        size_t bpos = (size_t)(brace - text);

        /* prelude = text[pos..bpos) 去空白 */
        const char *prel = text + pos;
        size_t plen = bpos - pos;
        while (plen > 0 && is_space(*prel)) {
            prel++;
            plen--;
        }
        while (plen > 0 && is_space(prel[plen - 1]))
            plen--;
        char *prelude = xstrndup(prel, plen);
        if (prelude == NULL) {
            ok = 0;
            break;
        }

        /* 行号：text[:bpos] 里 '\n' 的个数 + 1 */
        int line = 1;
        for (size_t i = 0; i < bpos; i++)
            if (text[i] == '\n')
                line++;
        char where[64];
        snprintf(where, sizeof(where), "css:%d", line);

        long end = match_brace(text, bpos, where);
        if (end < 0) {
            ql_mem_free(prelude);
            ok = 0;
            break;
        }
        const char *body = text + bpos + 1u;
        size_t blen = (size_t)end - bpos - 1u;

        if (prelude[0] == '@') {
            /* @keyframes <name> */
            const char *nm = prelude + 1;
            const char *kw = "keyframes";
            size_t kwlen = strlen(kw);
            if (strncmp(nm, kw, kwlen) != 0) {
                set_error("%s: 不支持 at-rule '%s'", where, prelude);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            nm += kwlen;
            while (is_space(*nm))
                nm++;
            char name[64];
            size_t nlen = 0;
            while (nm[nlen] != '\0' && !is_space(nm[nlen]) && nlen + 1u < sizeof(name)) {
                name[nlen] = nm[nlen];
                nlen++;
            }
            name[nlen] = '\0';
            if (nlen == 0) {
                set_error("%s: 不支持 at-rule '%s'", where, prelude);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            if (strcmp(name, "none") == 0 || strcmp(name, "initial") == 0 ||
                strcmp(name, "inherit") == 0 || strcmp(name, "infinite") == 0) {
                set_error("%s: @keyframes 名字 '%s' 与 CSS 关键字冲突", where, name);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            if (kfdefs_find(kfs, name) != NULL) {
                set_error("%s: @keyframes '%s' 重复定义", where, name);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            if (kfs->n >= (int)QL_CFG_UI_MAX_ANIMS) {
                set_error("%s: @keyframes 段数超过上限 %d", where,
                          (int)QL_CFG_UI_MAX_ANIMS);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            if (kfs->n == kfs->cap) {
                int cap = (kfs->cap > 0) ? kfs->cap * 2 : 8;
                KfDef *nd = (KfDef *)ql_mem_realloc(kfs->d, (size_t)cap * sizeof(KfDef));
                if (nd == NULL) {
                    ql_mem_free(prelude);
                    ok = 0;
                    break;
                }
                kfs->d = nd;
                kfs->cap = cap;
            }
            memset(&kfs->d[kfs->n], 0, sizeof(KfDef));
            kfs->d[kfs->n].name = xstrdup(name);
            if (kfs->d[kfs->n].name == NULL) {
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            KfDef *def = &kfs->d[kfs->n];
            kfs->n++;
            if (!parse_keyframes(body, blen, where, def) || def->n == 0) {
                if (s_error[0] == '\0')
                    set_error("%s: @keyframes '%s' 没有任何关键帧", where, name);
                ql_mem_free(prelude);
                ok = 0;
                break;
            }
            ql_mem_free(prelude);
            pos = (size_t)end + 1u;
            continue;
        }

        Decls decls;
        memset(&decls, 0, sizeof(decls));
        if (!parse_decls(body, blen, where, 0, &decls)) {
            decls_free(&decls);
            ql_mem_free(prelude);
            ok = 0;
            break;
        }

        /* 逗号分隔的选择器列表 */
        char *save = prelude;
        for (;;) {
            char *comma = strchr(save, ',');
            if (comma != NULL)
                *comma = '\0';
            char *one = save;
            while (is_space(*one))
                one++;
            size_t olen = strlen(one);
            while (olen > 0 && is_space(one[olen - 1]))
                one[--olen] = '\0';

            if (olen > 0) {
                if (rules->n >= (int)QL_CFG_UI_MAX_CSS_RULES) {
                    set_error("%s: CSS 规则数超过上限 %d", where,
                              (int)QL_CFG_UI_MAX_CSS_RULES);
                    ok = 0;
                    break;
                }
                if (rules->n == rules->cap) {
                    int cap = (rules->cap > 0) ? rules->cap * 2 : 16;
                    Rule *nr = (Rule *)ql_mem_realloc(rules->r,
                                                      (size_t)cap * sizeof(Rule));
                    if (nr == NULL) {
                        ok = 0;
                        break;
                    }
                    rules->r = nr;
                    rules->cap = cap;
                }
                memset(&rules->r[rules->n], 0, sizeof(Rule));
                Rule *r = &rules->r[rules->n];
                rules->n++;
                if (!parse_selector(one, r) || !decls_copy(&r->decls, &decls)) {
                    ok = 0;
                    break;
                }
                r->order = order++;
            }

            if (comma == NULL)
                break;
            save = comma + 1;
        }
        decls_free(&decls);
        ql_mem_free(prelude);
        if (!ok)
            break;
        pos = (size_t)end + 1u;
    }

    ql_mem_free(text);
    return ok;
}

/* ============================== ⑤ HTML 词法 ============================== */

typedef struct PNode PNode;

typedef struct {
    int    is_text;
    char  *text;
    PNode *node;
} Item;

struct PNode {
    char  *tag;
    char **classes;
    int    nclasses;
    char  *id;
    char  *style_attr;
    uint8_t flags;           /* QL_UI_NODE_*（<input type="password">） */
    PNode *parent;
    PNode *first_child;
    PNode *next_sibling;
    char  *text;             /* 纯文本快路径 */
    Item  *items;            /* 文档顺序的内容（文本段 or 子元素） */
    int    nitems;
    int    capitems;
};

/* 下标 == QLUITag 的枚举值，顺序不能改（只在末尾追加） */
/* 本次解析里被降级/裁掉的标签个数；ql_ui_load_text 开始时清零 */
static uint16_t s_degraded_tags;

static const char *const k_tags[] = {
    "div", "section", "p", "span", "b", "i", "button", "h1", "h2", "h3",
    "h4", "h5", "h6",
    "header", "footer", "nav", "main", "article", "aside", "figure",
    "figcaption", "blockquote",
    "ul", "ol", "li",
    "hr", "br",
    "mark", "em", "strong", "u", "s",
    "input", "select", "option",
    /* --- 纯语义标签 --- */
    "html", "body",
    "dl", "dt", "dd", "menu", "address", "hgroup", "search",
    "a", "abbr", "code", "kbd", "samp", "var", "q", "dfn", "cite", "data",
    "time", "small", "bdi", "bdo", "wbr",
    "del", "ins",
    "form", "label", "fieldset", "legend", "optgroup",
    /* --- META 包：解析期跳过，不进树 --- */
    "base", "head", "link", "meta", "style", "title",
    /* --- WIDGET 包 --- */
    "output", "datalist", "progress", "meter", "details", "summary", "dialog",
    "img",
};

#define TAG_COUNT ((int)(sizeof(k_tags) / sizeof(k_tags[0])))

/* 标签名 → 枚举；不认识的返回 -1 */
static int tag_index(const char *tag)
{
    for (int i = 0; i < TAG_COUNT; i++)
        if (strcmp(tag, k_tags[i]) == 0)
            return i;
    return -1;
}

static int tag_supported(const char *tag)
{
    return tag_index(tag) >= 0;
}

static QLUITag tag_value(const char *tag)
{
    const int i = tag_index(tag);
    return (QLUITag)((i >= 0) ? i : QL_UI_TAG_DIV);
}

/*
 * 这些标签的 UA 默认 display 是 inline。**只用来判断"两个行内元素之间的空白
 * 要不要留成一个空格"**（见 flatten_node 的混排分支）——那个判断发生在级联之前，
 * 拿不到最终 display。如果你把 span 改成 display:block，这个判断不会跟着变，
 * 但那种写法本来就该自己管空白。
 */
static const char *const k_inline_tags[] = {
    "span", "b", "i", "em", "strong", "u", "s", "mark", "del", "ins",
    "a", "abbr", "code", "kbd", "samp", "var", "q", "dfn", "cite", "data",
    "time", "small", "bdi", "bdo", "wbr", "label", "legend",
};

/* 这些标签的 UA 默认 display 是 inline（表里的）+ 有额外样式的几个（下面单独判） */
static int tag_inline_by_default(const char *tag)
{
    for (size_t i = 0; i < sizeof(k_inline_tags) / sizeof(k_inline_tags[0]); i++)
        if (strcmp(tag, k_inline_tags[i]) == 0)
            return 1;
    return 0;
}

/*
 * 空元素（HTML 里没有结束标签、也没有子节点的那些）。
 * 本库只认这两个：`<hr>` 和 `<br>`。
 */
static int tag_is_void(const char *tag)
{
    return strcmp(tag, "hr") == 0 || strcmp(tag, "br") == 0 ||
           strcmp(tag, "input") == 0 || strcmp(tag, "wbr") == 0 ||
           strcmp(tag, "img") == 0;
}

/* ---------------- META 包：文档元数据（解析期整段跳过） ---------------- */

static int is_name_char(char c);   /* 定义在解析器里，这里先声明 */

/* 这批标签不进树、不产生盒子，只为了让"整页 HTML 贴进来"能编译 */
static int tag_is_metadata(const char *tag)
{
    return strcmp(tag, "base") == 0 || strcmp(tag, "head") == 0 ||
           strcmp(tag, "link") == 0 || strcmp(tag, "meta") == 0 ||
           strcmp(tag, "style") == 0 || strcmp(tag, "title") == 0;
}

/* 大小写不敏感地比较 s[0..len) 与 name（HTML 标签名不分大小写） */
static int name_is_ci(const char *s, size_t len, const char *name)
{
    size_t i = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (name[i] == '\0' || name[i] != c)
            return 0;
    }
    return name[i] == '\0';
}

/*
 * 跳过 raw text 元素（`<style>` / `<title>`）的内容，返回结束标签之后的位置。
 * raw text 的规矩：内容里**不认标签**，只认 `</style>` / `</title>` 本身。
 */
static const char *skip_raw_text(const char *body, const char *name)
{
    const size_t nlen = strlen(name);
    for (const char *p = body; *p != '\0'; p++) {
        if (p[0] != '<' || p[1] != '/')
            continue;
        if (!name_is_ci(p + 2, nlen, name))
            continue;
        const char *q = p + 2 + nlen;
        while (is_space(*q))
            q++;
        if (*q == '>')
            return q + 1;
    }
    set_error("HTML: <%s> 未闭合", name);
    return NULL;
}

/*
 * 从元素内容起点开始，跳过整个元素（内容 + 结束标签），返回之后的位置。
 * 空元素（`<base>` / `<link>` / `<meta>`）与自闭合写法没有内容，直接跳过起始标签。
 *
 * 这里是个**只看结构**的小扫描器：认注释、声明、raw text 与同名嵌套，
 * 不做实体解码、不建节点 —— 元数据本来就不该出现在树里。
 */
static const char *skip_element(const char *content, const char *name, int self_closing)
{
    if (self_closing || strcmp(name, "base") == 0 || strcmp(name, "link") == 0 ||
        strcmp(name, "meta") == 0)
        return content;

    if (strcmp(name, "style") == 0 || strcmp(name, "title") == 0)
        return skip_raw_text(content, name);

    /* 容器（目前只有 <head>）：按标签配平扫到匹配的结束标签 */
    int depth = 1;
    const char *p = content;
    while (*p != '\0') {
        if (*p != '<') {
            p++;
            continue;
        }
        if (strncmp(p, "<!--", 4) == 0) {
            const char *e = strstr(p + 4, "-->");
            if (e == NULL) {
                set_error("HTML: 注释未闭合");
                return NULL;
            }
            p = e + 3;
            continue;
        }
        if (p[1] == '!' || p[1] == '?') {
            const char *e = strchr(p, '>');
            if (e == NULL) {
                set_error("HTML: 声明未闭合");
                return NULL;
            }
            p = e + 1;
            continue;
        }

        const int closing = (p[1] == '/');
        const char *ns = p + (closing ? 2 : 1);
        const char *q = ns;
        while (is_name_char(*q))
            q++;
        const size_t len = (size_t)(q - ns);

        if (!closing && (name_is_ci(ns, len, "style") || name_is_ci(ns, len, "title"))) {
            /* 里面可能有 </head> 字样的文本，先按 raw text 跳过去 */
            const char *gt = strchr(q, '>');
            if (gt == NULL) {
                set_error("HTML: 标签格式错误");
                return NULL;
            }
            const char *e = name_is_ci(ns, len, "style") ? skip_raw_text(gt + 1, "style")
                                                         : skip_raw_text(gt + 1, "title");
            if (e == NULL)
                return NULL;
            p = e;
            continue;
        }

        if (name_is_ci(ns, len, name)) {
            if (!closing) {
                depth++;
            } else if (--depth == 0) {
                const char *gt = strchr(q, '>');
                if (gt == NULL) {
                    set_error("HTML: 结束标签格式错误 </%s>", name);
                    return NULL;
                }
                return gt + 1;
            }
        }
        const char *gt = strchr(q, '>');
        if (gt == NULL) {
            set_error("HTML: 标签格式错误");
            return NULL;
        }
        p = gt + 1;
    }
    set_error("HTML: <%s> 未闭合", name);
    return NULL;
}

static void pnode_free(PNode *n)
{
    if (n == NULL)
        return;
    ql_mem_free(n->tag);
    for (int i = 0; i < n->nclasses; i++)
        ql_mem_free(n->classes[i]);
    ql_mem_free(n->classes);
    ql_mem_free(n->id);
    ql_mem_free(n->style_attr);
    ql_mem_free(n->text);
    if (n->nitems > 0) {
        /* 还没 flatten：子元素在 items 里 */
        for (int i = 0; i < n->nitems; i++) {
            if (n->items[i].is_text)
                ql_mem_free(n->items[i].text);
            else
                pnode_free(n->items[i].node);
        }
    } else {
        /* flatten 之后：子元素已经连成 first_child 链，items 清空了 */
        PNode *c = n->first_child;
        while (c != NULL) {
            PNode *next = c->next_sibling;
            pnode_free(c);
            c = next;
        }
    }
    ql_mem_free(n->items);
    ql_mem_free(n);
}

static PNode *pnode_new(const char *tag, PNode *parent)
{
    PNode *n = (PNode *)xcalloc(1, sizeof(PNode));
    if (n == NULL)
        return NULL;
    n->tag = xstrdup(tag);
    n->parent = parent;
    if (n->tag == NULL) {
        ql_mem_free(n);
        return NULL;
    }
    return n;
}

static int pnode_add_item(PNode *n, Item it)
{
    if (n->nitems == n->capitems) {
        int cap = (n->capitems > 0) ? n->capitems * 2 : 4;
        Item *ni = (Item *)ql_mem_realloc(n->items, (size_t)cap * sizeof(Item));
        if (ni == NULL)
            return 0;
        n->items = ni;
        n->capitems = cap;
    }
    n->items[n->nitems++] = it;
    return 1;
}

/* `\s+` → 单个空格（Python 的 add_text） */
static char *collapse_ws(const char *chunk)
{
    size_t len = strlen(chunk);
    char *out = (char *)xalloc(len + 1u);
    if (out == NULL)
        return NULL;
    size_t k = 0;
    int prev = 0;
    for (size_t i = 0; i < len;) {
        size_t wl = u8_ws_len(chunk + i);
        if (wl > 0) {
            if (!prev) {
                out[k++] = ' ';
                prev = 1;
            }
            i += wl;
        } else {
            out[k++] = chunk[i];
            prev = 0;
            i++;
        }
    }
    out[k] = '\0';
    return out;
}

static int html_add_text(PNode *n, const char *chunk)
{
    char *collapsed = collapse_ws(chunk);
    if (collapsed == NULL)
        return 0;
    if (collapsed[0] == '\0') {
        ql_mem_free(collapsed);
        return 1;
    }
    Item it;
    it.is_text = 1;
    it.text = collapsed;
    it.node = NULL;
    if (!pnode_add_item(n, it)) {
        ql_mem_free(collapsed);
        return 0;
    }
    return 1;
}

/* 实体：只支持常见的一批；未知命名实体明确报错（不静默保留 &xxx;） */
static int decode_entities(const char *s, size_t len, SB *out)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '&') {
            if (!sb_addc(out, s[i]))
                return 0;
            continue;
        }
        size_t semi = i + 1u;
        while (semi < len && s[semi] != ';' && semi - i < 12u)
            semi++;
        if (semi >= len || s[semi] != ';') {
            if (!sb_addc(out, s[i]))
                return 0;
            continue;
        }
        size_t nlen = semi - i - 1u;
        if (nlen == 0) {
            if (!sb_addc(out, s[i]))
                return 0;
            continue;
        }
        char name[16];
        if (nlen >= sizeof(name))
            nlen = sizeof(name) - 1u;
        memcpy(name, s + i + 1u, nlen);
        name[nlen] = '\0';

        if (name[0] == '#') {
            long code = 0;
            if (name[1] == 'x' || name[1] == 'X')
                code = strtol(name + 2, NULL, 16);
            else
                code = strtol(name + 1, NULL, 10);
            if (code <= 0 || code > 0x10FFFF) {
                set_error("HTML: 非法字符引用 '&%s;'", name);
                return 0;
            }
            /* UTF-8 编码 */
            char tmp[4];
            size_t n = 0;
            if (code < 0x80) {
                tmp[n++] = (char)code;
            } else if (code < 0x800) {
                tmp[n++] = (char)(0xC0 | (code >> 6));
                tmp[n++] = (char)(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                tmp[n++] = (char)(0xE0 | (code >> 12));
                tmp[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                tmp[n++] = (char)(0x80 | (code & 0x3F));
            } else {
                tmp[n++] = (char)(0xF0 | (code >> 18));
                tmp[n++] = (char)(0x80 | ((code >> 12) & 0x3F));
                tmp[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                tmp[n++] = (char)(0x80 | (code & 0x3F));
            }
            if (!sb_add(out, tmp, n))
                return 0;
        } else {
            const char *rep = NULL;
            if (strcmp(name, "amp") == 0)
                rep = "&";
            else if (strcmp(name, "lt") == 0)
                rep = "<";
            else if (strcmp(name, "gt") == 0)
                rep = ">";
            else if (strcmp(name, "quot") == 0)
                rep = "\"";
            else if (strcmp(name, "apos") == 0)
                rep = "'";
            else if (strcmp(name, "nbsp") == 0)
                rep = " ";
            else {
                set_error("HTML: 不支持的实体 '&%s;'（只支持 &amp; &lt; &gt; &quot; "
                          "&apos; &nbsp; 与数字引用）", name);
                return 0;
            }
            if (!sb_adds(out, rep))
                return 0;
        }
        i = semi;
    }
    return 1;
}

/*
 * 解析起始标签的属性表。
 *
 * 只留五个：`class` / `id` / `style`（样式与选择器用），
 * `value` / `type`（只对 `<input>` 有意义）。
 * 其余属性（`data-*` / `aria-*` / `href` …）**静默忽略** —— 它们不影响渲染，
 * 为它们报错只会让真实 HTML 片段跑不起来。
 */
typedef struct {
    char *cls;
    char *id;
    char *style;
    char *value;      /* <input value> / <progress value> */
    char *type;       /* <input type="text|password"> */
    char *max;        /* <progress max> / <meter max> */
    char *src;        /* <img src>：只是资源名，字节另找（见图像管线设计） */
    uint8_t has_open; /* <details open> / <dialog open>：布尔属性，只需"有没有" */
} Attrs;

static void attrs_free(Attrs *a)
{
    ql_mem_free(a->cls);
    ql_mem_free(a->value);
    ql_mem_free(a->type);
    ql_mem_free(a->max);
    ql_mem_free(a->src);
    ql_mem_free(a->id);
    ql_mem_free(a->style);
    memset(a, 0, sizeof(*a));
}

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
}

/*
 * 解析 `<div class="a b" id="x">` 里标签名之后的部分。
 * 返回指向 '>' 之后的位置；失败返回 NULL。
 */
static const char *parse_attrs(const char *p, Attrs *a)
{
    for (;;) {
        while (is_space(*p))
            p++;
        if (*p == '\0') {
            set_error("HTML: 标签未闭合");
            return NULL;
        }
        if (*p == '>')
            return p + 1;
        if (*p == '/' && p[1] == '>')
            return p + 2;

        const char *nstart = p;
        while (is_name_char(*p))
            p++;
        if (p == nstart) {
            set_error("HTML: 属性名非法（'%c'）", *p);
            return NULL;
        }
        char name[64];
        size_t nlen = (size_t)(p - nstart);
        if (nlen >= sizeof(name))
            nlen = sizeof(name) - 1u;
        for (size_t i = 0; i < nlen; i++) {
            char c = nstart[i];
            name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        name[nlen] = '\0';

        const char *val = NULL;
        size_t vlen = 0;
        char quote = 0;
        const char *q = skip_space(p);
        if (*q == '=') {
            q++;
            q = skip_space(q);
            if (*q == '"' || *q == '\'') {
                quote = *q;
                q++;
                const char *vstart = q;
                while (*q != '\0' && *q != quote)
                    q++;
                if (*q != quote) {
                    set_error("HTML: 属性值缺少引号收尾");
                    return NULL;
                }
                val = vstart;
                vlen = (size_t)(q - vstart);
                q++;
            } else {
                const char *vstart = q;
                while (*q != '\0' && !is_space(*q) && *q != '>' && *q != '/')
                    q++;
                val = vstart;
                vlen = (size_t)(q - vstart);
            }
            p = q;
        }

        /* 布尔属性：`open` 没有值也算（`<details open>` 是常见写法） */
        if (strcmp(name, "open") == 0)
            a->has_open = 1;

        if (val != NULL && (strcmp(name, "class") == 0 || strcmp(name, "id") == 0 ||
                            strcmp(name, "style") == 0 || strcmp(name, "value") == 0 ||
                            strcmp(name, "type") == 0 || strcmp(name, "max") == 0 ||
                            strcmp(name, "src") == 0)) {
            SB sb = { 0 };
            if (!decode_entities(val, vlen, &sb)) {
                sb_free(&sb);
                return NULL;
            }
            char *s = (sb.p != NULL) ? sb.p : xstrdup("");
            if (s == NULL)
                return NULL;
            if (strcmp(name, "class") == 0) {
                ql_mem_free(a->cls);
                a->cls = s;
            } else if (strcmp(name, "id") == 0) {
                ql_mem_free(a->id);
                a->id = s;
            } else if (strcmp(name, "value") == 0) {
                ql_mem_free(a->value);
                a->value = s;
            } else if (strcmp(name, "type") == 0) {
                ql_mem_free(a->type);
                a->type = s;
            } else if (strcmp(name, "max") == 0) {
                ql_mem_free(a->max);
                a->max = s;
            } else if (strcmp(name, "src") == 0) {
                ql_mem_free(a->src);
                a->src = s;
            } else {
                ql_mem_free(a->style);
                a->style = s;
            }
        }
    }
}

/* 把 "a b  c" 拆成类名列表 */
static int set_classes(PNode *n, const char *cls)
{
    const char *p = cls;
    while (*p != '\0') {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        size_t len = (size_t)(p - start);
        char *name = xstrndup(start, len);
        if (name == NULL)
            return 0;
        char **nc = (char **)ql_mem_realloc(n->classes,
                                            (size_t)(n->nclasses + 1) * sizeof(char *));
        if (nc == NULL) {
            ql_mem_free(name);
            return 0;
        }
        n->classes = nc;
        n->classes[n->nclasses++] = name;
    }
    return 1;
}

/*
 * 混合内容在**解析期**消解成匿名的 inline 子节点（与 Python 的 _flatten 一致）：
 *   <p>Hello <b>world</b>!</p> → <p><span>Hello </span><b>world</b><span>!</span></p>
 * 这样运行期只认识元素树，不需要为混合内容加任何数据模型。
 */
static int flatten_node(PNode *n)
{
    for (int i = 0; i < n->nitems; i++)
        if (!n->items[i].is_text && !flatten_node(n->items[i].node))
            return 0;

    /* #document 只是个容器：把子元素连成链表就够（Python 侧它在解析时就有 children，
       本实现是"items → 链表"一次性建的，所以这里必须先连链再返回） */
    if (strcmp(n->tag, "#document") == 0) {
        PNode *prev = NULL;
        for (int i = 0; i < n->nitems; i++) {
            if (n->items[i].is_text)
                continue;
            PNode *child = n->items[i].node;
            if (prev != NULL)
                prev->next_sibling = child;
            else
                n->first_child = child;
            prev = child;
        }
        for (int i = 0; i < n->nitems; i++)
            if (n->items[i].is_text)
                ql_mem_free(n->items[i].text);
        n->nitems = 0;
        return 1;
    }

    int has_text = 0, has_elem = 0;
    for (int i = 0; i < n->nitems; i++) {
        if (n->items[i].is_text)
            has_text = 1;
        else
            has_elem = 1;
    }

    if (!has_text) {
        /* 没有文本：按 items 顺序把子元素连成链表 */
        PNode *prev = NULL;
        for (int i = 0; i < n->nitems; i++) {
            PNode *child = n->items[i].node;
            if (prev != NULL)
                prev->next_sibling = child;
            else
                n->first_child = child;
            prev = child;
        }
        n->nitems = 0;
        return 1;
    }

    if (!has_elem) {
        /* 纯文本快路径：join + strip + 折叠空白 */
        SB sb = { 0 };
        for (int i = 0; i < n->nitems; i++)
            if (!sb_adds(&sb, n->items[i].text)) {
                sb_free(&sb);
                return 0;
            }
        for (int i = 0; i < n->nitems; i++)
            ql_mem_free(n->items[i].text);
        n->nitems = 0;
        if (sb.p == NULL) {
            n->text = NULL;
            return 1;
        }
        /* strip（含 Unicode 空白） */
        char *s = sb.p;
        for (;;) {
            size_t wl = u8_ws_len(s);
            if (wl == 0)
                break;
            s += wl;
        }
        size_t len = strlen(s);
        while (len > 0) {
            /* 从尾部回退一个码点再判空白 */
            size_t back = 1;
            while (back < 4 && back < len && ((unsigned char)s[len - back] & 0xC0u) == 0x80u)
                back++;
            if (u8_ws_len(s + len - back) != back)
                break;
            len -= back;
        }
        s[len] = '\0';
        char *collapsed = collapse_ws(s);
        sb_free(&sb);
        if (collapsed == NULL)
            return 0;
        if (collapsed[0] == '\0') {
            ql_mem_free(collapsed);
            n->text = NULL;
        } else {
            n->text = collapsed;
        }
        return 1;
    }

    /* 混排：重建 children，文本段包成匿名 span */
    int first_text = -1, last_text = -1;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i].is_text) {
            if (first_text < 0)
                first_text = i;
            last_text = i;
        }

    PNode *prev = NULL;
    for (int i = 0; i < n->nitems; i++) {
        if (n->items[i].is_text) {
            char *txt = n->items[i].text;
            char *dup = xstrdup(txt);
            if (dup == NULL)
                return 0;
            /* 首段 lstrip、末段 rstrip */
            char *t = dup;
            if (i == first_text) {
                for (;;) {
                    size_t wl = u8_ws_len(t);
                    if (wl == 0)
                        break;
                    t += wl;
                }
            }
            if (i == last_text) {
                size_t len = strlen(t);
                while (len > 0) {
                    size_t back = 1;
                    while (back < 4 && back < len &&
                           ((unsigned char)t[len - back] & 0xC0u) == 0x80u)
                        back++;
                    if (u8_ws_len(t + len - back) != back)
                        break;
                    len -= back;
                }
                t[len] = '\0';
            }
            int blank = 1;
            for (const char *q = t; *q != '\0';) {
                size_t wl = u8_ws_len(q);
                if (wl > 0) {
                    q += wl;
                    continue;
                }
                blank = 0;
                break;
            }
            if (blank) {
                /*
                 * 空白段（元素之间那点缩进/空格）。
                 *
                 * CSS 的规则：两个**行内**盒子之间的空白折叠成一个空格；
                 * 块级盒子之间的空白没有意义，直接丢掉。
                 * 这里没有级联结果，只能按标签的默认 display 判断 ——
                 * 不这么做的话 `<mark>a</mark> <em>b</em>` 会渲染成 "ab"
                 * （真实 HTML 里到处都是这种写法，肉眼一看就错）。
                 *
                 * 首段/末段的空白仍然丢掉：行首行尾的空格在 CSS 里也不显示。
                 */
                /*
                 * 注意不能用 first_text/last_text 判断"行首/行尾"：
                 * `<em>a</em> <strong>b</strong>` 里那段空格就是**最后一个**
                 * 文本段，但它夹在两个元素中间，必须保留（丢掉就成了 "ab"）。
                 * 真正的判据是有没有前/后邻居元素。
                 */
                int keep = 0;
                {
                    const PNode *pv = NULL;
                    const PNode *nx = NULL;
                    for (int k = i - 1; k >= 0; k--)
                        if (!n->items[k].is_text) {
                            pv = n->items[k].node;
                            break;
                        }
                    for (int k = i + 1; k < n->nitems; k++)
                        if (!n->items[k].is_text) {
                            nx = n->items[k].node;
                            break;
                        }
                    keep = (pv != NULL && nx != NULL &&
                            tag_inline_by_default(pv->tag) &&
                            tag_inline_by_default(nx->tag));
                }
                ql_mem_free(dup);
                if (!keep)
                    continue;
                PNode *space = pnode_new("span", n);
                if (space == NULL)
                    return 0;
                space->text = xstrdup(" ");
                if (space->text == NULL) {
                    pnode_free(space);
                    return 0;
                }
                if (prev != NULL)
                    prev->next_sibling = space;
                else
                    n->first_child = space;
                prev = space;
                continue;
            }
            PNode *span = pnode_new("span", n);
            if (span == NULL) {
                ql_mem_free(dup);
                return 0;
            }
            span->text = xstrdup(t);
            ql_mem_free(dup);
            if (span->text == NULL)
                return 0;
            if (prev != NULL)
                prev->next_sibling = span;
            else
                n->first_child = span;
            prev = span;
        } else {
            PNode *child = n->items[i].node;
            if (prev != NULL)
                prev->next_sibling = child;
            else
                n->first_child = child;
            prev = child;
        }
    }

    /* items 里的文本已经转移，元素的所有权也转移到链表上了 */
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i].is_text)
            ql_mem_free(n->items[i].text);
    n->nitems = 0;
    return 1;
}

/* 解析整份 HTML，返回 #document 节点 */
static PNode *parse_html(const char *html)
{
    PNode *root = (PNode *)xcalloc(1, sizeof(PNode));
    if (root == NULL)
        return NULL;
    root->tag = xstrdup("#document");
    if (root->tag == NULL) {
        ql_mem_free(root);
        return NULL;
    }

    PNode *cur = root;
    int depth = 0;
    const char *p = html;

    while (*p != '\0') {
        if (*p != '<') {
            const char *start = p;
            while (*p != '\0' && *p != '<')
                p++;
            SB sb = { 0 };
            if (!decode_entities(start, (size_t)(p - start), &sb)) {
                sb_free(&sb);
                pnode_free(root);
                return NULL;
            }
            int ok = 1;
            if (sb.p != NULL && sb.p[0] != '\0')
                ok = html_add_text(cur, sb.p);
            sb_free(&sb);
            if (!ok) {
                pnode_free(root);
                return NULL;
            }
            continue;
        }

        /* 注释 / DOCTYPE / 处理指令：跳过 */
        if (strncmp(p, "<!--", 4) == 0) {
            const char *end = strstr(p + 4, "-->");
            if (end == NULL) {
                set_error("HTML: 注释未闭合");
                pnode_free(root);
                return NULL;
            }
            p = end + 3;
            continue;
        }
        if (p[1] == '!' || p[1] == '?') {
            const char *end = strchr(p, '>');
            if (end == NULL) {
                set_error("HTML: 声明未闭合");
                pnode_free(root);
                return NULL;
            }
            p = end + 1;
            continue;
        }

        /* 结束标签 */
        if (p[1] == '/') {
            const char *q = p + 2;
            const char *nstart = q;
            while (is_name_char(*q))
                q++;
            char name[32];
            size_t nlen = (size_t)(q - nstart);
            if (nlen >= sizeof(name))
                nlen = sizeof(name) - 1u;
            for (size_t i = 0; i < nlen; i++) {
                char c = nstart[i];
                name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            name[nlen] = '\0';
            while (is_space(*q))
                q++;
            if (*q != '>') {
                set_error("HTML: 结束标签格式错误 </%s>", name);
                pnode_free(root);
                return NULL;
            }
            if (cur == root) {
                set_error("HTML: 多余的结束标签 </%s>", name);
                pnode_free(root);
                return NULL;
            }
            if (strcmp(cur->tag, name) != 0) {
                set_error("HTML: 结束标签 </%s> 与 <%s> 不匹配", name, cur->tag);
                pnode_free(root);
                return NULL;
            }
            cur = cur->parent;
            depth--;
            p = q + 1;
            continue;
        }

        /* 起始标签 */
        {
            const char *q = p + 1;
            const char *nstart = q;
            while (is_name_char(*q))
                q++;
            char name[32];
            size_t nlen = (size_t)(q - nstart);
            if (nlen == 0 || nlen >= sizeof(name)) {
                set_error("HTML: 标签名非法");
                pnode_free(root);
                return NULL;
            }
            for (size_t i = 0; i < nlen; i++) {
                char c = nstart[i];
                name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            name[nlen] = '\0';

            Attrs at;
            memset(&at, 0, sizeof(at));
            const char *after = parse_attrs(q, &at);
            if (after == NULL) {
                attrs_free(&at);
                pnode_free(root);
                return NULL;
            }
            int self_closing = (after[-2] == '/');

            /*
             * 文档元数据（META 包）：整段跳过，不进树。
             * `<style>` / `<title>` 的内容连字都不进文本池 —— 这一条是"整页 HTML
             * 贴进来能编译"的关键：不跳过的话样式文本会变成界面上的文字。
             */
            if (tag_is_metadata(name)) {
                if (!QL_FEAT_UI_META && !QL_CFG_UI_LENIENT_TAGS) {
                    set_error("HTML: <%s> 需要 QL_FEAT_UI_META（文档元数据），本构建没有启用",
                              name);
                    attrs_free(&at);
                    pnode_free(root);
                    return NULL;
                }
                if (!QL_FEAT_UI_META)
                    s_degraded_tags++;   /* 裁掉的包：明确允许时只计数 */
                const char *next = skip_element(after, name, self_closing);
                attrs_free(&at);
                if (next == NULL) {
                    pnode_free(root);
                    return NULL;
                }
                p = next;
                continue;
            }

            if (!tag_supported(name)) {
                /* 对几个"一定会有人写"的标签给出比"不支持"更有用的提示 */
                if (strcmp(name, "img") == 0)
                    set_error("HTML: 不支持 <img>（本库没有图片解码器）："
                              "装饰用 div + background-image / 圆角做，"
                              "图标用字体字符或 SDF 图元做");
                else if (strcmp(name, "input") == 0 || strcmp(name, "textarea") == 0 ||
                         strcmp(name, "select") == 0)
                    set_error("HTML: 不支持 <%s>（还没有输入控件）："
                              "文本展示用 div，点击用 :active/:hover + 事件", name);
                else if (strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 ||
                         strcmp(name, "td") == 0)
                    set_error("HTML: 不支持表格 <%s>：用嵌套的 flex 行来做", name);
                else
                    set_error("HTML: 不支持的标签 <%s>", name);
                attrs_free(&at);
                pnode_free(root);
                return NULL;
            }
            if (depth >= (int)QL_CFG_UI_MAX_DEPTH) {
                set_error("HTML: 嵌套深度超过上限 %d", (int)QL_CFG_UI_MAX_DEPTH);
                attrs_free(&at);
                pnode_free(root);
                return NULL;
            }

            PNode *n = pnode_new(name, cur);
            if (n == NULL) {
                attrs_free(&at);
                pnode_free(root);
                return NULL;
            }
            if (at.cls != NULL && !set_classes(n, at.cls)) {
                attrs_free(&at);
                pnode_free(n);
                pnode_free(root);
                return NULL;
            }
            if (at.id != NULL) {
                n->id = xstrdup(at.id);
                if (n->id == NULL) {
                    attrs_free(&at);
                    pnode_free(n);
                    pnode_free(root);
                    return NULL;
                }
            }
            if (at.style != NULL) {
                n->style_attr = xstrdup(at.style);
                if (n->style_attr == NULL) {
                    attrs_free(&at);
                    pnode_free(n);
                    pnode_free(root);
                    return NULL;
                }
            }
            /*
             * <input>：`value` 变成这个节点的初始文本（之后由运行期池接管），
             * `type` 只认 text / password。
             */
            if (strcmp(name, "input") == 0) {
                if (at.type != NULL && at.type[0] != '\0') {
                    char t[16];
                    size_t tn = strlen(at.type);
                    if (tn >= sizeof(t)) tn = sizeof(t) - 1u;
                    for (size_t i = 0; i < tn; i++) {
                        const char c = at.type[i];
                        t[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
                    }
                    t[tn] = '\0';
                    if (strcmp(t, "password") == 0) {
                        n->flags |= QL_UI_NODE_PASSWORD;
                    } else if (strcmp(t, "text") != 0 && strcmp(t, "search") != 0 &&
                               strcmp(t, "email") != 0 && strcmp(t, "number") != 0) {
                        set_error("HTML: 不支持的 <input type=\"%s\">"
                                  "（只支持 text / password）", at.type);
                        attrs_free(&at);
                        pnode_free(n);
                        pnode_free(root);
                        return NULL;
                    }
                }
                if (at.value != NULL && at.value[0] != '\0') {
                    Item tit;
                    tit.is_text = 1;
                    tit.text = xstrdup(at.value);
                    tit.node = NULL;
                    if (tit.text == NULL || !pnode_add_item(n, tit)) {
                        ql_mem_free(tit.text);
                        attrs_free(&at);
                        pnode_free(n);
                        pnode_free(root);
                        return NULL;
                    }
                }
            }
            /*
             * <progress> / <meter>：`value` / `max` 编成 "value/max" 放进这个节点的文本，
             * 由 ql_ui_build 折成 0..1000 的完成度后**清掉**（见 qlui.c）。
             * 为什么不给 QLUINode 加两个字段：那是每个节点都要付的 4 字节，
             * 而这里只是一串去重后的十进制数字（"70/100" 全文只存一份）。
             */
            /*
             * <img src>：`src` 存进节点文本（资源名）。不加节点字段 ——
             * 资源名本来就在文本池里，运行时按名字找到解码结果。
             */
            if (strcmp(name, "img") == 0 && at.src != NULL && at.src[0] != '\0') {
                Item tit;
                tit.is_text = 1;
                tit.text = xstrdup(at.src);
                tit.node = NULL;
                if (tit.text == NULL || !pnode_add_item(n, tit)) {
                    ql_mem_free(tit.text);
                    attrs_free(&at);
                    pnode_free(n);
                    pnode_free(root);
                    return NULL;
                }
            }

            if ((strcmp(name, "details") == 0 || strcmp(name, "dialog") == 0) &&
                at.has_open)
                n->flags |= QL_UI_NODE_OPEN;

            if (strcmp(name, "progress") == 0 || strcmp(name, "meter") == 0) {
                if (at.value != NULL && at.value[0] != '\0') {
                    char spec[32];
                    snprintf(spec, sizeof(spec), "%s/%s", at.value,
                             (at.max != NULL && at.max[0] != '\0') ? at.max : "100");
                    Item tit;
                    tit.is_text = 1;
                    tit.text = xstrdup(spec);
                    tit.node = NULL;
                    if (tit.text == NULL || !pnode_add_item(n, tit)) {
                        ql_mem_free(tit.text);
                        attrs_free(&at);
                        pnode_free(n);
                        pnode_free(root);
                        return NULL;
                    }
                }
            }
            attrs_free(&at);

            Item it;
            it.is_text = 0;
            it.text = NULL;
            it.node = n;
            if (!pnode_add_item(cur, it)) {
                pnode_free(n);
                pnode_free(root);
                return NULL;
            }

            /* 自闭合写法 <br/> 与空元素 <br> 等价：都不压栈 */
            if (self_closing || tag_is_void(name)) {
                /* 不压栈（等价于 start + end） */
            } else {
                cur = n;
                depth++;
            }
            p = after;
        }
    }

    if (cur != root) {
        set_error("HTML: 标签未闭合 <%s>", cur->tag);
        pnode_free(root);
        return NULL;
    }
    if (!flatten_node(root)) {
        pnode_free(root);
        return NULL;
    }
    return root;
}

/* ============================== ⑥ 级联 ============================== */

static int simple_matches(const PNode *n, const SelPart *sp)
{
    if (sp->tag != NULL && strcmp(n->tag, sp->tag) != 0)
        return 0;
    if (sp->id != NULL) {
        if (n->id == NULL || strcmp(n->id, sp->id) != 0)
            return 0;
    }
    for (int i = 0; i < sp->nclasses; i++) {
        int found = 0;
        for (int j = 0; j < n->nclasses; j++)
            if (strcmp(n->classes[j], sp->classes[i]) == 0) {
                found = 1;
                break;
            }
        if (!found)
            return 0;
    }
    return 1;
}

/* 从右往左匹配（与 Python 的 matches() 逐行对应） */
static int match_rec(const PNode *n, const Rule *r, int idx)
{
    if (n == NULL || !simple_matches(n, &r->parts[idx]))
        return 0;
    if (idx == 0)
        return 1;

    const int comb = r->parts[idx].comb;
    const PNode *p = n->parent;
    if (comb == SEL_COMB_CHILD)
        return p != NULL && strcmp(p->tag, "#document") != 0 &&
               match_rec(p, r, idx - 1);
    while (p != NULL && strcmp(p->tag, "#document") != 0) {
        if (match_rec(p, r, idx - 1))
            return 1;
        p = p->parent;
    }
    return 0;
}

static int rule_matches(const PNode *n, const Rule *r)
{
    return r->nparts > 0 && match_rec(n, r, r->nparts - 1);
}

/*
 * 每个标签的 UA 默认样式（相当于浏览器的内置样式表）。
 *
 * 两条原则：
 *   1. **只给"看结构就该知道"的样式**：块级/行内、标题字号、列表缩进、<hr> 的细线。
 *      不做逐像素对标浏览器 —— 我们的 UA 默认值是给这个库的观感调的，不是给
 *      "和 Chrome 截图一致"调的。
 *   2. **不确定性优先报错**：想让某个标签长得不一样，自己写 CSS 覆盖它，
 *      别指望这里悄悄塞个值。
 *
 * 注意哪些标签这里**故意没有效果**：
 *   - `em` / `strong` / `b` / `i` / `u` / `s`：要加粗/斜体/下划线，得先有
 *     字体变体（合成加粗、合成斜体、装饰线），那是下一步；现在它们与 `span` 等价。
 *   - `ul` / `ol`：结构与缩进有了，但**不画项目符号** —— 本库没有生成内容
 *     （`::marker` / `::before`），标记请自己写字符。
 */
static int ua_set_block(Decls *out)
{
    return decls_set(out, "display", "block");
}

static int ua_set_inline(Decls *out)
{
    return decls_set(out, "display", "inline");
}

static int ua_heading(Decls *out, int px, int margin)
{
    char size[16];
    char m[16];
    snprintf(size, sizeof(size), "%dpx", px);
    snprintf(m, sizeof(m), "%dpx 0", margin);
    return decls_set(out, "display", "block") && decls_set(out, "font-size", size) &&
           decls_set(out, "margin", m);
}

static int ua_defaults(const char *tag, Decls *out)
{
    /* ---- 行内 ---- */
    if (strcmp(tag, "span") == 0)
        return ua_set_inline(out);

    /*
     * 语义行内标签现在**真的有区别了**（合成加粗/斜体/装饰线，见 QL_UI_FONT_*）。
     * 与浏览器一致：b/strong = 加粗，i/em = 斜体，u = 下划线，s = 删除线。
     */
    if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0)
        return ua_set_inline(out) && decls_set(out, "font-weight", "bold");
    if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0)
        return ua_set_inline(out) && decls_set(out, "font-style", "italic");
    if (strcmp(tag, "u") == 0)
        return ua_set_inline(out) && decls_set(out, "text-decoration", "underline");
    if (strcmp(tag, "s") == 0)
        return ua_set_inline(out) && decls_set(out, "text-decoration", "line-through");

    /* 高亮：行内元素也有自己的背景盒，所以这个标签现在就能看出区别 */
    if (strcmp(tag, "mark") == 0)
        return decls_set(out, "display", "inline") &&
               decls_set(out, "background-color", "#FFE58F") &&
               decls_set(out, "color", "#000");

    /* ---- 标题：浏览器量级，但用 px ---- */
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
        static const int k_h_px[6] = { 24, 20, 17, 15, 13, 12 };
        static const int k_h_margin[6] = { 10, 9, 8, 7, 6, 5 };
        const int i = tag[1] - '1';
        return ua_heading(out, k_h_px[i], k_h_margin[i]);
    }

    if (strcmp(tag, "p") == 0)
        return decls_set(out, "display", "block") && decls_set(out, "margin", "8px 0");

    /*
     * <input>：单行输入框。默认高度 + 内边距给足可点面积；宽度 auto 时按
     * max-content 走（和文本节点一样），想固定就自己写 width。
     */
    if (strcmp(tag, "input") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "height", "24px") &&
               decls_set(out, "padding", "0 8px") &&
               decls_set(out, "line-height", "24px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "border-color", "#555555") &&
               decls_set(out, "background-color", "#FFFFFF") &&
               decls_set(out, "color", "#000000");
    }

    /*
     * <select>：长得像输入框，但显示的是选中项的文字。
     * <option>：**不参与布局**（display:none），只在展开的面板里画。
     */
    if (strcmp(tag, "select") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "height", "24px") &&
               decls_set(out, "padding", "0 8px") &&
               decls_set(out, "line-height", "24px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "border-color", "#555555") &&
               decls_set(out, "background-color", "#FFFFFF") &&
               decls_set(out, "color", "#000000");
    }
    if (strcmp(tag, "option") == 0) {
        return decls_set(out, "display", "none");
    }
    /* <optgroup> 与 <option> 一样不参与布局（组名现在不画，见文档） */
    if (strcmp(tag, "optgroup") == 0 || strcmp(tag, "datalist") == 0) {
        return decls_set(out, "display", "none");
    }

    /*
     * <progress> / <meter>：一根细条。轨道用 background-color，填充用 color ——
     * 换配色只写 CSS，运行期只改"多满"（见 ql_ui_progress_set）。
     */
    if (strcmp(tag, "progress") == 0 || strcmp(tag, "meter") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "height", "8px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "background-color", "#E3E6EA") &&
               decls_set(out, "color", "#0969DA");
    }
    if (strcmp(tag, "output") == 0)
        return ua_set_inline(out);

    /*
     * <img>：行内块，尺寸来自资源固有尺寸（见 QLUI阅读指南 §5.7.1）。
     * 不给默认宽高：没指定就按图片自己的像素尺寸排。
     */
    if (strcmp(tag, "img") == 0)
        return decls_set(out, "display", "inline-block");

    /*
     * <dialog>：默认 70% 宽、居中（居中在布局末尾做）。自带白底与圆角，
     * 因为浮层必须**不透明**，否则下面的内容会透上来。
     */
    if (strcmp(tag, "dialog") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "width", "70%") &&
               decls_set(out, "padding", "16px") &&
               decls_set(out, "border-radius", "8px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-color", "#8A8A8A") &&
               decls_set(out, "background-color", "#FFFFFF") &&
               decls_set(out, "color", "#000000");
    }

    /* <details> 是块；<summary> 是它的标题行（没有三角标记，见文档） */
    if (strcmp(tag, "details") == 0 || strcmp(tag, "summary") == 0)
        return decls_set(out, "display", "block");

    /* ---- 纯语义标签：只给"看结构就该知道"的 display ---- */
    /*
     * 描述列表：<dd> 的缩进用 padding-left（与 <ul> 一致，39 行的 40px margin 不要）。
     */
    if (strcmp(tag, "dl") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "margin", "8px 0");
    }
    if (strcmp(tag, "dd") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "padding-left", "18px");
    }
    if (strcmp(tag, "menu") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "margin", "8px 0") &&
               decls_set(out, "padding-left", "18px");
    }
    if (strcmp(tag, "address") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "font-style", "italic");
    }
    /* <fieldset> 自带一圈边框；<legend> 排在它里面当一个行内标题 */
    if (strcmp(tag, "fieldset") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "margin", "8px 0") &&
               decls_set(out, "padding", "8px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "border-color", "#8A8A8A");
    }
    /* 引用/定义/变量按浏览器惯例用斜体（装饰线已经支持，所以这是真的斜体） */
    if (strcmp(tag, "cite") == 0 || strcmp(tag, "var") == 0 ||
        strcmp(tag, "dfn") == 0) {
        return ua_set_inline(out) && decls_set(out, "font-style", "italic");
    }
    if (strcmp(tag, "del") == 0)
        return ua_set_inline(out) && decls_set(out, "text-decoration", "line-through");
    if (strcmp(tag, "ins") == 0)
        return ua_set_inline(out) && decls_set(out, "text-decoration", "underline");

    /* 剩下这些纯行内标签 UA 默认只有 display:inline（与 tag_inline_by_default 同表） */
    if (tag_inline_by_default(tag))
        return ua_set_inline(out);

    /* ---- 按钮：一行能看见的边框 + 内边距（用 line-height 撑不出来的高度） ---- */
    if (strcmp(tag, "button") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "padding", "4px 10px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "border-color", "#555") &&
               decls_set(out, "background-color", "#d8d8d8") &&
               decls_set(out, "color", "#000");
    }

    /* ---- 列表：结构 + 缩进（符号自己写，见上面的说明） ---- */
    if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "margin", "8px 0") &&
               decls_set(out, "padding-left", "18px");
    }
    if (strcmp(tag, "li") == 0)
        return ua_set_block(out);

    /* ---- <hr>：块级细线。不写 background-color 就是黑的，所以必须显式给 ---- */
    if (strcmp(tag, "hr") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "height", "1px") &&
               decls_set(out, "background-color", "#8A8A8A") &&
               decls_set(out, "margin", "8px 0");
    }
    /*
     * <br>：没有生成内容，也没法在一个文本节点里插换行，所以做成
     * "一个高度为 0 的块" —— 它会结束当前行内行，于是下一段从新的一行开始。
     * 视觉上等价于换行，代价是它占一个节点。
     */
    if (strcmp(tag, "br") == 0) {
        return decls_set(out, "display", "block") && decls_set(out, "height", "0");
    }

    /* ---- 其余结构容器 / section / figure / figcaption / blockquote / main … ---- */
    return ua_set_block(out);
}

typedef struct {
    const Rule *r;
    int         ids, cls, tags, order;
} MatchRef;

static int matchref_cmp(const void *a, const void *b)
{
    const MatchRef *x = (const MatchRef *)a;
    const MatchRef *y = (const MatchRef *)b;
    if (x->ids != y->ids)
        return (x->ids < y->ids) ? -1 : 1;
    if (x->cls != y->cls)
        return (x->cls < y->cls) ? -1 : 1;
    if (x->tags != y->tags)
        return (x->tags < y->tags) ? -1 : 1;
    if (x->order != y->order)
        return (x->order < y->order) ? -1 : 1;
    return 0;
}

/* 按 (specificity, order) 覆盖出某个状态组合下的声明集合 */
static int collect_decls(const PNode *n, const Rules *rs, int states_active,
                         Decls *out)
{
    if (!ua_defaults(n->tag, out))
        return 0;

    MatchRef *refs = (MatchRef *)xalloc((size_t)(rs->n > 0 ? rs->n : 1) *
                                        sizeof(MatchRef));
    if (refs == NULL)
        return 0;
    int nm = 0;
    for (int i = 0; i < rs->n; i++) {
        const Rule *r = &rs->r[i];
        if ((r->states & ~states_active) != 0)
            continue;
        if (!rule_matches(n, r))
            continue;
        refs[nm].r = r;
        refs[nm].ids = r->spec_ids;
        refs[nm].cls = r->spec_cls;
        refs[nm].tags = r->spec_tags;
        refs[nm].order = r->order;
        nm++;
    }
    if (nm > 1)
        qsort(refs, (size_t)nm, sizeof(MatchRef), matchref_cmp);
    for (int i = 0; i < nm; i++) {
        const Decls *d = &refs[i].r->decls;
        for (int j = 0; j < d->n; j++)
            if (!decls_set(out, d->d[j].key, d->d[j].val)) {
                ql_mem_free(refs);
                return 0;
            }
    }
    ql_mem_free(refs);
    return 1;
}

/* 行内 style 也是一条"规则"，单独展开后**覆盖**规则级声明 */
static int apply_inline_style(const char *style, Decls *decls)
{
    Decls inline_d;
    memset(&inline_d, 0, sizeof(inline_d));

    size_t len = strlen(style);
    size_t pos = 0;
    while (pos <= len) {
        size_t end = pos;
        while (end < len && style[end] != ';')
            end++;
        const char *piece = style + pos;
        size_t plen = end - pos;
        while (plen > 0 && is_space(*piece)) {
            piece++;
            plen--;
        }
        while (plen > 0 && is_space(piece[plen - 1]))
            plen--;
        if (plen > 0) {
            const char *colon = memchr(piece, ':', plen);
            if (colon == NULL) {
                decls_free(&inline_d);
                set_error("行内 style 缺少冒号");
                return 0;
            }
            char prop[64];
            size_t klen = (size_t)(colon - piece);
            if (klen >= sizeof(prop)) {
                decls_free(&inline_d);
                return 0;
            }
            for (size_t i = 0; i < klen; i++) {
                char c = piece[i];
                prop[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            prop[klen] = '\0';
            const char *val = colon + 1;
            size_t vlen = plen - klen - 1u;
            while (vlen > 0 && is_space(*val)) {
                val++;
                vlen--;
            }
            while (vlen > 0 && is_space(val[vlen - 1]))
                vlen--;
            char *vs = xstrndup(val, vlen);
            if (vs == NULL) {
                decls_free(&inline_d);
                return 0;
            }
            if (!in_supported_props(prop)) {
                set_error("行内 style 不支持属性 '%s'", prop);
                ql_mem_free(vs);
                decls_free(&inline_d);
                return 0;
            }
            int ok = decls_set(&inline_d, prop, vs);
            ql_mem_free(vs);
            if (!ok) {
                decls_free(&inline_d);
                return 0;
            }
        }
        if (end >= len)
            break;
        pos = end + 1;
    }

    if (!expand_shorthands(&inline_d)) {
        decls_free(&inline_d);
        return 0;
    }
    for (int i = 0; i < inline_d.n; i++)
        if (!decls_set(decls, inline_d.d[i].key, inline_d.d[i].val)) {
            decls_free(&inline_d);
            return 0;
        }
    decls_free(&inline_d);
    return 1;
}

/* 继承 + 行内 → 最终声明集合 */
static int finalize_style(Decls *decls, const PNode *n, const Decls *inherited,
                          Decls *out)
{
    if (n->style_attr != NULL && !apply_inline_style(n->style_attr, decls))
        return 0;
    if (inherited != NULL && !decls_copy(out, inherited))
        return 0;
    for (int i = 0; i < decls->n; i++)
        if (!decls_set(out, decls->d[i].key, decls->d[i].val))
            return 0;
    return 1;
}

/* ============================== ⑦ 编译成 QLUIStyle ============================== */

typedef struct {
    int  *v;
    int   n;
    int   cap;
} IntList;

static int intlist_add(IntList *l, int v)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == v)
            return i;
    if (l->n >= (int)QL_CFG_UI_MAX_FONT_SIZES) {
        set_error("font-size 种类过多（上限 %d）", (int)QL_CFG_UI_MAX_FONT_SIZES);
        return -1;
    }
    if (l->n == l->cap) {
        int cap = (l->cap > 0) ? l->cap * 2 : 8;
        int *nv = (int *)ql_mem_realloc(l->v, (size_t)cap * sizeof(int));
        if (nv == NULL)
            return -1;
        l->v = nv;
        l->cap = cap;
    }
    l->v[l->n] = v;
    return l->n++;
}

static int display_value(const char *s)
{
    if (strcmp(s, "block") == 0)
        return QL_UI_DISPLAY_BLOCK;
    if (strcmp(s, "inline") == 0)
        return QL_UI_DISPLAY_INLINE;
    if (strcmp(s, "inline-block") == 0)
        return QL_UI_DISPLAY_INLINE_BLOCK;
    if (strcmp(s, "none") == 0)
        return QL_UI_DISPLAY_NONE;
    if (strcmp(s, "flex") == 0)
        return QL_UI_DISPLAY_FLEX;
    if (strcmp(s, "inline-flex") == 0)
        return QL_UI_DISPLAY_INLINE_FLEX;
    return -1;
}

static int flex_dir_value(const char *s)
{
    if (strcmp(s, "row") == 0)
        return QL_UI_FLEX_ROW;
    if (strcmp(s, "row-reverse") == 0)
        return QL_UI_FLEX_ROW_REVERSE;
    if (strcmp(s, "column") == 0)
        return QL_UI_FLEX_COLUMN;
    if (strcmp(s, "column-reverse") == 0)
        return QL_UI_FLEX_COLUMN_REVERSE;
    return -1;
}

static int justify_value(const char *s)
{
    if (strcmp(s, "flex-start") == 0 || strcmp(s, "start") == 0)
        return QL_UI_JUSTIFY_START;
    if (strcmp(s, "center") == 0)
        return QL_UI_JUSTIFY_CENTER;
    if (strcmp(s, "flex-end") == 0 || strcmp(s, "end") == 0)
        return QL_UI_JUSTIFY_END;
    if (strcmp(s, "space-between") == 0)
        return QL_UI_JUSTIFY_SPACE_BETWEEN;
    if (strcmp(s, "space-around") == 0)
        return QL_UI_JUSTIFY_SPACE_AROUND;
    if (strcmp(s, "space-evenly") == 0)
        return QL_UI_JUSTIFY_SPACE_EVENLY;
    return -1;
}

static int align_value(const char *s)
{
    if (strcmp(s, "stretch") == 0)
        return QL_UI_ALIGN_STRETCH;
    if (strcmp(s, "flex-start") == 0 || strcmp(s, "start") == 0)
        return QL_UI_ALIGN_START;
    if (strcmp(s, "center") == 0)
        return QL_UI_ALIGN_CENTER;
    if (strcmp(s, "flex-end") == 0 || strcmp(s, "end") == 0)
        return QL_UI_ALIGN_END;
    return -1;
}

static int position_value(const char *s)
{
    if (strcmp(s, "static") == 0)
        return QL_UI_POSITION_STATIC;
    if (strcmp(s, "relative") == 0)
        return QL_UI_POSITION_RELATIVE;
    if (strcmp(s, "absolute") == 0)
        return QL_UI_POSITION_ABSOLUTE;
    if (strcmp(s, "fixed") == 0)
        return QL_UI_POSITION_FIXED;
    return -1;
}

static int overflow_value(const char *s)
{
    if (strcmp(s, "visible") == 0)
        return QL_UI_OVERFLOW_VISIBLE;
    if (strcmp(s, "hidden") == 0)
        return QL_UI_OVERFLOW_HIDDEN;
    if (strcmp(s, "auto") == 0 || strcmp(s, "scroll") == 0)
        return QL_UI_OVERFLOW_AUTO;
    return -1;
}

static int text_align_value(const char *s)
{
    if (strcmp(s, "left") == 0)
        return QL_TEXT_ALIGN_LEFT;
    if (strcmp(s, "center") == 0)
        return QL_TEXT_ALIGN_CENTER;
    if (strcmp(s, "right") == 0)
        return QL_TEXT_ALIGN_RIGHT;
    return -1;
}

/* 关键帧表（所有段摊平） */
typedef struct {
    QLUIKeyframe *f;
    int           n;
    int           cap;
} KfTable;

typedef struct {
    const char *name;
    int         first;
    int         count;
} KfRange;

typedef struct {
    KfRange *r;
    int      n;
    int      cap;
} KfRanges;

typedef struct {
    QLUIAnimation *a;
    int            n;
    int            cap;
} AnimTable;

/* 关键帧的属性位（与 Python 的 compile_keyframes 一一对应） */
#define KF_BG     0x01
#define KF_ALPHA  0x02
#define KF_TRANS  0x04
#define KF_TEXT   0x08
#define KF_BORDER 0x10
#define KF_RADIUS 0x20
#define KF_CLIP   0x40
#define KF_BASE   0x80

static int kf_compile(const KfDefs *kfs, KfTable *tab, KfRanges *ranges)
{
    for (int di = 0; di < kfs->n; di++) {
        const KfDef *def = &kfs->d[di];
        char where[96];
        snprintf(where, sizeof(where), "@keyframes %s", def->name);

        /* 重复百分比 */
        for (int i = 0; i < def->n; i++)
            for (int j = i + 1; j < def->n; j++)
                if (def->f[i].offset == def->f[j].offset) {
                    set_error("%s: 有重复的百分比", where);
                    return 0;
                }

        /* 各帧的属性集合必须一致 */
        int props = -1;
        for (int i = 0; i < def->n; i++) {
            const Decls *d = &def->f[i].decls;
            int p = 0;
            if (decls_get(d, "background") != NULL ||
                decls_get(d, "background-color") != NULL ||
                decls_get(d, "background-image") != NULL)
                p |= KF_BG;
            if (decls_get(d, "opacity") != NULL)
                p |= KF_ALPHA;
            if (decls_get(d, "transform") != NULL)
                p |= KF_TRANS;
            if (decls_get(d, "color") != NULL)
                p |= KF_TEXT;
            if (decls_get(d, "border-color") != NULL)
                p |= KF_BORDER;
            if (decls_get(d, "border-radius") != NULL)
                p |= KF_RADIUS;
            if (decls_get(d, "clip-path") != NULL)
                p |= KF_CLIP;
            if (p == 0) {
                set_error("%s: %d%% 这一帧没有可动画的属性", where,
                          def->f[i].offset / 10);
                return 0;
            }
            if (props < 0)
                props = p;
            else if (p != props) {
                set_error("%s: 各帧写的属性不一致（%d%% 这帧少了别的帧有的属性）", where,
                          def->f[i].offset / 10);
                return 0;
            }
        }

        /* 按 offset 排序（稳定插入排序，与 Python 的 sorted 一致） */
        int n = def->n;
        int *idx = (int *)xalloc((size_t)n * sizeof(int));
        if (idx == NULL)
            return 0;
        for (int i = 0; i < n; i++)
            idx[i] = i;
        for (int i = 1; i < n; i++) {
            int key = idx[i];
            int j = i - 1;
            while (j >= 0 && def->f[idx[j]].offset > def->f[key].offset) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = key;
        }

        /* 缺 0% / 100% 时补"取基础样式"的隐式帧 */
        int need_first = (def->f[idx[0]].offset != 0);
        int need_last = (def->f[idx[n - 1]].offset != 1000);

        const int first_frame = tab->n;
        int count = 0;

        /* 0% 隐式帧 */
        if (need_first) {
            if (tab->n >= (int)QL_CFG_UI_MAX_KEYFRAMES) {
                set_error("%s: 关键帧数超过上限", where);
                ql_mem_free(idx);
                return 0;
            }
            if (tab->n == tab->cap) {
                int cap = (tab->cap > 0) ? tab->cap * 2 : 32;
                QLUIKeyframe *nf = (QLUIKeyframe *)ql_mem_realloc(
                    tab->f, (size_t)cap * sizeof(QLUIKeyframe));
                if (nf == NULL) {
                    ql_mem_free(idx);
                    return 0;
                }
                tab->f = nf;
                tab->cap = cap;
            }
            QLUIKeyframe *k = &tab->f[tab->n++];
            memset(k, 0, sizeof(*k));
            k->offset = 0;
            k->ease = QL_UI_KF_EASE_INHERIT;
            k->props = (uint8_t)(props | KF_BASE);
            k->sc_x = -1;
            k->sc_y = -1;
            k->radius = -1;
            k->clip_r = -1;
            count++;
        }

        for (int i = 0; i < n; i++) {
            const KfRaw *raw = &def->f[idx[i]];
            const Decls *d = &raw->decls;
            uint32_t bg = 0, bg2 = 0;
            int alpha = 255;
            int tfx = 0, tfy = 0, scx = -1, scy = -1;
            uint32_t text_c = 0, border_c = 0;
            int clipr = -1, rad = -1;

            if ((props & KF_TRANS) != 0) {
                if (!parse_transform(decls_get(d, "transform"), where, &tfx, &tfy, &scx,
                                     &scy)) {
                    ql_mem_free(idx);
                    return 0;
                }
            }
            if ((props & KF_TEXT) != 0) {
                uint8_t a = 0;
                if (!parse_color(decls_get(d, "color"), where, &text_c, &a)) {
                    ql_mem_free(idx);
                    return 0;
                }
            }
            if ((props & KF_BORDER) != 0) {
                uint8_t a = 0;
                if (!parse_color(decls_get(d, "border-color"), where, &border_c, &a)) {
                    ql_mem_free(idx);
                    return 0;
                }
            }
            if ((props & KF_CLIP) != 0) {
                int kind = 0;
                if (!parse_clip_path(decls_get(d, "clip-path"), where, &kind, &clipr)) {
                    ql_mem_free(idx);
                    return 0;
                }
            }
            if ((props & KF_RADIUS) != 0) {
                uint8_t unit;
                int v;
                if (!parse_length(decls_get(d, "border-radius"), where, 1, &unit, &v)) {
                    ql_mem_free(idx);
                    return 0;
                }
                if (v < 0 || v > 32767) {
                    set_error("%s: border-radius 超出 0~32767px", where);
                    ql_mem_free(idx);
                    return 0;
                }
                rad = v;
            }
            if ((props & KF_BG) != 0) {
                const char *gi = decls_get(d, "background-image");
                if (gi != NULL) {
                    int dir = 0;
                    if (!parse_gradient(gi, where, &bg, &bg2, &dir)) {
                        ql_mem_free(idx);
                        return 0;
                    }
                } else {
                    const char *txt = decls_get(d, "background-color");
                    if (txt == NULL)
                        txt = decls_get(d, "background");
                    if (txt == NULL)
                        txt = "transparent";
                    uint8_t a = 0;
                    if (!parse_color(txt, where, &bg, &a)) {
                        ql_mem_free(idx);
                        return 0;
                    }
                    alpha = a;
                    bg2 = bg;
                }
            }
            if ((props & KF_ALPHA) != 0) {
                const char *op = decls_get(d, "opacity");
                double v = 0;
                const char *end = NULL;
                if (op == NULL || !match_number(skip_space(op), &end, &v) ||
                    *skip_space(end) != '\0') {
                    set_error("%s: opacity 必须是 0~1 的数字", where);
                    ql_mem_free(idx);
                    return 0;
                }
                if (v < 0.0 || v > 1.0) {
                    set_error("%s: opacity 超出 0~1", where);
                    ql_mem_free(idx);
                    return 0;
                }
                alpha = (int)py_round(v * 255.0);
            }

            int ease = QL_UI_KF_EASE_INHERIT;
            const char *tf = decls_get(d, "animation-timing-function");
            if (tf != NULL) {
                char low[32];
                trim_lower(tf, low, sizeof(low));
                int ev = 0;
                if (!easing_value(low, &ev)) {
                    set_error("%s: 暂不支持的 animation-timing-function '%s'", where,
                              tf);
                    ql_mem_free(idx);
                    return 0;
                }
                ease = ev;
            }

            if (tab->n >= (int)QL_CFG_UI_MAX_KEYFRAMES) {
                set_error("%s: 关键帧数超过上限", where);
                ql_mem_free(idx);
                return 0;
            }
            if (tab->n == tab->cap) {
                int cap = (tab->cap > 0) ? tab->cap * 2 : 32;
                QLUIKeyframe *nf = (QLUIKeyframe *)ql_mem_realloc(
                    tab->f, (size_t)cap * sizeof(QLUIKeyframe));
                if (nf == NULL) {
                    ql_mem_free(idx);
                    return 0;
                }
                tab->f = nf;
                tab->cap = cap;
            }
            QLUIKeyframe *k = &tab->f[tab->n++];
            memset(k, 0, sizeof(*k));
            k->offset = (uint16_t)raw->offset;
            k->ease = (uint8_t)ease;
            k->props = (uint8_t)props;
            k->bg_color = bg;
            k->bg_color2 = bg2;
            k->text_color = text_c;
            k->border_color = border_c;
            k->tf_x = (int16_t)tfx;
            k->tf_y = (int16_t)tfy;
            k->sc_x = (int16_t)scx;
            k->sc_y = (int16_t)scy;
            k->radius = (int16_t)rad;
            k->clip_r = (int16_t)clipr;
            k->bg_alpha = (uint8_t)alpha;
            count++;
        }

        if (need_last) {
            if (tab->n >= (int)QL_CFG_UI_MAX_KEYFRAMES) {
                set_error("%s: 关键帧数超过上限", where);
                ql_mem_free(idx);
                return 0;
            }
            if (tab->n == tab->cap) {
                int cap = (tab->cap > 0) ? tab->cap * 2 : 32;
                QLUIKeyframe *nf = (QLUIKeyframe *)ql_mem_realloc(
                    tab->f, (size_t)cap * sizeof(QLUIKeyframe));
                if (nf == NULL) {
                    ql_mem_free(idx);
                    return 0;
                }
                tab->f = nf;
                tab->cap = cap;
            }
            QLUIKeyframe *k = &tab->f[tab->n++];
            memset(k, 0, sizeof(*k));
            k->offset = 1000;
            k->ease = QL_UI_KF_EASE_INHERIT;
            k->props = (uint8_t)(props | KF_BASE);
            k->sc_x = -1;
            k->sc_y = -1;
            k->radius = -1;
            k->clip_r = -1;
            count++;
        }

        ql_mem_free(idx);

        if (ranges->n == ranges->cap) {
            int cap = (ranges->cap > 0) ? ranges->cap * 2 : 8;
            KfRange *nr = (KfRange *)ql_mem_realloc(ranges->r,
                                                    (size_t)cap * sizeof(KfRange));
            if (nr == NULL)
                return 0;
            ranges->r = nr;
            ranges->cap = cap;
        }
        ranges->r[ranges->n].name = def->name;
        ranges->r[ranges->n].first = first_frame;
        ranges->r[ranges->n].count = count;
        ranges->n++;
    }
    return 1;
}

static const KfRange *kf_range_find(const KfRanges *r, const char *name)
{
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->r[i].name, name) == 0)
            return &r->r[i];
    return NULL;
}

/* 样式表身份：**排序后的 (属性, 值) 集合**。
 * 必须与 Python 侧的 `repr(sorted(st.items()))` 等价（同一个等价类划分），
 * 否则样式表条数与顺序会不同，blob 就对不上。 */
static int cmp_str_ptr(const void *a, const void *b)
{
    const char *const *x = (const char *const *)a;
    const char *const *y = (const char *const *)b;
    return strcmp(*x, *y);
}

static char *style_signature(const Decls *st, size_t *out_len)
{
    int n = st->n;
    const char **keys = (const char **)xalloc((size_t)(n > 0 ? n : 1) *
                                              sizeof(const char *));
    if (keys == NULL)
        return NULL;
    for (int i = 0; i < n; i++)
        keys[i] = st->d[i].key;
    qsort(keys, (size_t)n, sizeof(const char *), cmp_str_ptr);

    SB sb = { 0 };
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        /* 按键找值（keys 已排序，值按原顺序找） */
        const char *val = NULL;
        for (int j = 0; j < st->n; j++)
            if (strcmp(st->d[j].key, keys[i]) == 0) {
                val = st->d[j].val;
                break;
            }
        if (val == NULL) {
            ok = 0;
            break;
        }
        ok = sb_adds(&sb, keys[i]) && sb_addc(&sb, '\x01') && sb_adds(&sb, val) &&
             sb_addc(&sb, '\x02');
    }
    if (!ok) {
        sb_free(&sb);
        ql_mem_free(keys);
        return NULL;
    }
    ql_mem_free(keys);
    if (sb.p == NULL) {
        sb.p = xstrdup("");
        if (sb.p == NULL)
            return NULL;
        sb.len = 0;
    }
    *out_len = sb.len;
    return sb.p;
}

/* 一条正在播的动画（去重前的原始参数） */
static int anim_index_of(const Decls *st, const char *where, const KfRanges *ranges,
                         const KfTable *kftab, AnimTable *anims, int *out_idx)
{
    *out_idx = 0xFFFF;

    const char *an = decls_get(st, "animation-name");
    char name[64] = "none";
    if (an != NULL)
        trim_lower(an, name, sizeof(name));
    if (strcmp(name, "none") == 0 || name[0] == '\0' || strcmp(name, "initial") == 0)
        return 1;

    const KfRange *rng = kf_range_find(ranges, name);
    if (rng == NULL) {
        set_error("%s: animation-name '%s' 没有对应的 @keyframes", where, name);
        return 0;
    }

    const char *dur_txt = decls_get(st, "animation-duration");
    if (dur_txt == NULL) {
        set_error("%s: 有 animation-name 但没写 animation-duration", where);
        return 0;
    }
    int dur = 0;
    if (!parse_duration(dur_txt, where, "animation-duration", &dur))
        return 0;
    if (dur == 0) {
        set_error("%s: animation-duration 不能为 0（那等于不播）", where);
        return 0;
    }

    const char *tf = decls_get(st, "animation-timing-function");
    char tf_low[32] = "ease";
    if (tf != NULL)
        trim_lower(tf, tf_low, sizeof(tf_low));
    int ease = 0;
    if (!easing_value(tf_low, &ease)) {
        set_error("%s: 暂不支持的 animation-timing-function '%s'", where, tf_low);
        return 0;
    }

    const char *ic = decls_get(st, "animation-iteration-count");
    char ic_low[32] = "1";
    if (ic != NULL)
        trim_lower(ic, ic_low, sizeof(ic_low));
    int iters = 0;
    if (strcmp(ic_low, "infinite") == 0) {
        iters = 0;
    } else {
        if (!is_all_digits(ic_low)) {
            set_error("%s: animation-iteration-count 必须是整数或 infinite", where);
            return 0;
        }
        long v = strtol(ic_low, NULL, 10);
        if (v < 1 || v > 255) {
            set_error("%s: animation-iteration-count 超出 1~255", where);
            return 0;
        }
        iters = (int)v;
    }

    const char *dr = decls_get(st, "animation-direction");
    char dr_low[32] = "normal";
    if (dr != NULL)
        trim_lower(dr, dr_low, sizeof(dr_low));
    int direction = QL_UI_ANIM_NORMAL;
    if (strcmp(dr_low, "reverse") == 0)
        direction = QL_UI_ANIM_REVERSE;
    else if (strcmp(dr_low, "alternate") == 0)
        direction = QL_UI_ANIM_ALTERNATE;
    else if (strcmp(dr_low, "alternate-reverse") == 0)
        direction = QL_UI_ANIM_ALTERNATE_REVERSE;
    else if (strcmp(dr_low, "normal") != 0) {
        set_error("%s: 不支持的 animation-direction '%s'", where, dr_low);
        return 0;
    }

    const char *fl = decls_get(st, "animation-fill-mode");
    char fl_low[32] = "none";
    if (fl != NULL)
        trim_lower(fl, fl_low, sizeof(fl_low));
    int fill = QL_UI_ANIM_FILL_NONE;
    if (strcmp(fl_low, "forwards") == 0)
        fill = QL_UI_ANIM_FILL_FORWARDS;
    else if (strcmp(fl_low, "backwards") == 0)
        fill = QL_UI_ANIM_FILL_BACKWARDS;
    else if (strcmp(fl_low, "both") == 0)
        fill = QL_UI_ANIM_FILL_BOTH;
    else if (strcmp(fl_low, "none") != 0) {
        set_error("%s: 不支持的 animation-fill-mode '%s'", where, fl_low);
        return 0;
    }

    int delay = 0;
    const char *dly = decls_get(st, "animation-delay");
    if (dly != NULL && !parse_duration(dly, where, "animation-delay", &delay))
        return 0;

    /* 去重：(name, dur, ease, iters, direction, fill, delay) */
    for (int i = 0; i < anims->n; i++) {
        const QLUIAnimation *a = &anims->a[i];
        if (a->first_frame == (uint16_t)rng->first && a->dur_ms == (uint16_t)dur &&
            a->ease == (uint8_t)ease && a->iterations == (uint8_t)iters &&
            a->direction == (uint8_t)direction && a->fill == (uint8_t)fill &&
            a->delay_ms == (uint16_t)delay) {
            *out_idx = i;
            return 1;
        }
    }
    (void)kftab;

    if (anims->n >= (int)QL_CFG_UI_MAX_ANIMS) {
        set_error("%s: 动画段数超过上限 %d", where, (int)QL_CFG_UI_MAX_ANIMS);
        return 0;
    }
    if (anims->n == anims->cap) {
        int cap = (anims->cap > 0) ? anims->cap * 2 : 8;
        QLUIAnimation *na = (QLUIAnimation *)ql_mem_realloc(
            anims->a, (size_t)cap * sizeof(QLUIAnimation));
        if (na == NULL)
            return 0;
        anims->a = na;
        anims->cap = cap;
    }
    QLUIAnimation *a = &anims->a[anims->n];
    memset(a, 0, sizeof(*a));
    a->first_frame = (uint16_t)rng->first;
    a->frame_count = (uint16_t)rng->count;
    a->dur_ms = (uint16_t)dur;
    a->ease = (uint8_t)ease;
    a->iterations = (uint8_t)iters;
    a->direction = (uint8_t)direction;
    a->fill = (uint8_t)fill;
    a->delay_ms = (uint16_t)delay;
    *out_idx = anims->n++;
    return 1;
}

/* 把一份声明集合编译成 QLUIStyle。检查顺序与 Python 侧一致（第一个错就停） */
static int compile_style(const Decls *st, const char *where, const KfRanges *ranges,
                         const KfTable *kftab, AnimTable *anims, IntList *fonts,
                         QLUIStyle *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t bg = 0, fg = 0, bd = 0;
    uint8_t bg_a = 0, a = 0;
    const char *v = NULL;

    if (!parse_color((v = decls_get(st, "background-color")) != NULL ? v : "transparent",
                     where, &bg, &bg_a))
        return 0;
    if (!parse_color((v = decls_get(st, "color")) != NULL ? v : "black", where, &fg, &a))
        return 0;
    if (!parse_color((v = decls_get(st, "border-color")) != NULL ? v : "black", where,
                     &bd, &a))
        return 0;

    uint32_t bg2 = bg;
    int grad_dir = 0;
    if ((v = decls_get(st, "background-image")) != NULL) {
        uint32_t g0 = 0, g1 = 0;
        if (!parse_gradient(v, where, &g0, &g1, &grad_dir))
            return 0;
        bg = g0;
        bg2 = g1;
        if (bg_a == 0)
            bg_a = 255;
    }

    int sh_dx = 0, sh_dy = 0, sh_blur = 0;
    uint32_t sh_color = 0;
    uint8_t sh_a = 0;
    if ((v = decls_get(st, "box-shadow")) != NULL &&
        !parse_shadow(v, where, &sh_dx, &sh_dy, &sh_blur, &sh_color, &sh_a))
        return 0;

    uint8_t wu = QL_UI_UNIT_AUTO, hu = QL_UI_UNIT_AUTO;
    int wv = 0, hv = 0;
    if ((v = decls_get(st, "width")) != NULL && !parse_length(v, where, 1, &wu, &wv))
        return 0;
    if ((v = decls_get(st, "height")) != NULL && !parse_length(v, where, 1, &hu, &hv))
        return 0;

    int margin[4] = { 0, 0, 0, 0 }, padding[4] = { 0, 0, 0, 0 };
    if ((v = decls_get(st, "margin")) != NULL && !parse_edges(v, where, margin))
        return 0;
    if ((v = decls_get(st, "padding")) != NULL && !parse_edges(v, where, padding))
        return 0;
    {
        static const char *const sides[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) {
            char key[32];
            snprintf(key, sizeof(key), "margin-%s", sides[i]);
            if ((v = decls_get(st, key)) != NULL) {
                uint8_t unit;
                int num;
                if (!parse_length(v, where, 0, &unit, &num))
                    return 0;
                if (unit != QL_UI_UNIT_PX) {
                    set_error("%s: %s 只支持 px", where, key);
                    return 0;
                }
                margin[i] = num;
            }
            snprintf(key, sizeof(key), "padding-%s", sides[i]);
            if ((v = decls_get(st, key)) != NULL) {
                uint8_t unit;
                int num;
                if (!parse_length(v, where, 0, &unit, &num))
                    return 0;
                if (unit != QL_UI_UNIT_PX) {
                    set_error("%s: %s 只支持 px", where, key);
                    return 0;
                }
                padding[i] = num;
            }
        }
    }

    int disp = display_value((v = decls_get(st, "display")) != NULL ? v : "block");
    if (disp < 0) {
        set_error("%s: 不支持的 display '%s'", where,
                  (v != NULL) ? v : "block");
        return 0;
    }
    int align = text_align_value((v = decls_get(st, "text-align")) != NULL ? v : "left");
    if (align < 0) {
        set_error("%s: 不支持的 text-align '%s'", where, v);
        return 0;
    }

    int font_px = 16;
    {
        uint8_t unit;
        int num;
        if (!parse_length((v = decls_get(st, "font-size")) != NULL ? v : "16px", where,
                          1, &unit, &num))
            return 0;
        font_px = num;
    }
    int font_id = intlist_add(fonts, font_px);
    if (font_id < 0)
        return 0;

    /*
     * 字体变体 + 装饰线。三者都**合成**实现（不换字体文件），见 QL_UI_FONT_*。
     *
     * font-weight：支持 normal(400)/bold(700)/100..900 的数字写法。
     *   >= 600 视为加粗 —— 和 CSS 的"相对 bolder"简化版一致，够用且可预测。
     * font-style ：只支持 normal / italic（oblique 等价于 italic）。
     * text-decoration：none / underline / line-through（combo 用空格分隔）。
     */
    uint8_t font_flags = 0;
    {
        const char *fv = decls_get(st, "font-weight");
        if (fv != NULL) {
            char wt[32];
            trim_lower(fv, wt, sizeof(wt));
            if (strcmp(wt, "normal") == 0) {
                /* 400：不加粗 */
            } else if (strcmp(wt, "bold") == 0) {
                font_flags |= QL_UI_FONT_BOLD;
            } else {
                const char *end = NULL;
                double w = 0;
                if (!match_number(wt, &end, &w) || *end != '\0' || w < 1 || w > 1000) {
                    set_error("%s: 不支持的 font-weight '%s'"
                              "（normal / bold / 100~900）", where, fv);
                    return 0;
                }
                if (w >= 600)
                    font_flags |= QL_UI_FONT_BOLD;
            }
        }
        fv = decls_get(st, "font-style");
        if (fv != NULL) {
            char stl[32];
            trim_lower(fv, stl, sizeof(stl));
            if (strcmp(stl, "normal") == 0) {
                /* 不斜 */
            } else if (strcmp(stl, "italic") == 0 || strcmp(stl, "oblique") == 0) {
                font_flags |= QL_UI_FONT_ITALIC;
            } else {
                set_error("%s: 不支持的 font-style '%s'（normal / italic）", where, fv);
                return 0;
            }
        }
        fv = decls_get(st, "text-decoration");
        if (fv != NULL) {
            char td[64];
            trim_lower(fv, td, sizeof(td));
            if (strcmp(td, "none") == 0) {
                /* 无 */
            } else {
                /* 允许 "underline line-through" 这种组合 */
                const char *p = td;
                while (*p != '\0') {
                    while (*p == ' ') p++;
                    const char *b = p;
                    while (*p != '\0' && *p != ' ') p++;
                    char word[32];
                    size_t wl = (size_t)(p - b);
                    if (wl >= sizeof(word)) wl = sizeof(word) - 1u;
                    memcpy(word, b, wl);
                    word[wl] = '\0';
                    if (wl == 0)
                        continue;
                    if (strcmp(word, "underline") == 0) {
                        font_flags |= QL_UI_FONT_UNDERLINE;
                    } else if (strcmp(word, "line-through") == 0) {
                        font_flags |= QL_UI_FONT_LINE_THROUGH;
                    } else if (strcmp(word, "overline") == 0) {
                        set_error("%s: text-decoration 暂不支持 overline（本库没有上划线）",
                                  where);
                        return 0;
                    } else {
                        set_error("%s: 不支持的 text-decoration '%s'"
                                  "（none / underline / line-through）", where, word);
                        return 0;
                    }
                }
            }
        }
    }

    /*
     * white-space：默认 nowrap（与浏览器的**故意差异**，见 qlui.h 里该字段的注释）。
     * 只支持 nowrap / normal —— pre* 系列需要保留空白与制表符语义，本库不做。
     */
    int white_space = 0;
    {
        const char *ws = decls_get(st, "white-space");
        if (ws != NULL) {
            char wt[64];
            trim_lower(ws, wt, sizeof(wt));
            if (strcmp(wt, "normal") == 0) {
                white_space = 1;
            } else if (strcmp(wt, "nowrap") == 0) {
                white_space = 0;
            } else {
                set_error("%s: 不支持的 white-space '%s'（只支持 nowrap / normal）",
                          where, ws);
                return 0;
            }
        }
    }

    /*
     * line-height：0 = normal（半行距 0，与旧行为一致）。
     * 三写法：`normal` / `<N>px` / 无单位倍数（按当前 font-size 折算成 px）。
     * 单行垂直居中就靠它：`height: 26px; line-height: 26px`。
     */
    int line_h = 0;
    {
        const char *lh = decls_get(st, "line-height");
        if (lh != NULL) {
            char lt[128];
            trim_lower(lh, lt, sizeof(lt));
            if (strcmp(lt, "normal") == 0) {
                line_h = 0;
            } else {
                const char *end = NULL;
                double mult = 0;
                if (!match_number(lt, &end, &mult)) {
                    set_error("%s: 不支持的 line-height '%s'（normal / <N>px / 无单位倍数）",
                              where, lh);
                    return 0;
                }
                double px;
                if (*end == '\0') {
                    px = mult * (double)font_px;      /* 无单位 = 字体大小的倍数 */
                } else if (strcmp(end, "px") == 0) {
                    px = mult;
                } else {
                    set_error("%s: 不支持的 line-height '%s'（normal / <N>px / 无单位倍数）",
                              where, lh);
                    return 0;
                }
                line_h = (int)py_round(px);
                if (line_h < 1 || line_h > 32767) {
                    set_error("%s: line-height 必须在 1~32767px（收到 '%s'）", where, lh);
                    return 0;
                }
            }
        }
    }

    int bw = 0, radius = 0;
    {
        uint8_t unit;
        int num;
        if (!parse_length((v = decls_get(st, "border-width")) != NULL ? v : "0", where,
                          1, &unit, &num))
            return 0;
        bw = num;
        if (!parse_length((v = decls_get(st, "border-radius")) != NULL ? v : "0", where,
                          1, &unit, &num))
            return 0;
        radius = num;
    }

    int box_sizing = 0;
    v = decls_get(st, "box-sizing");
    if (v != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        if (strcmp(low, "border-box") == 0)
            box_sizing = 1;
        else if (strcmp(low, "content-box") != 0) {
            set_error("%s: 不支持的 box-sizing '%s'", where, v);
            return 0;
        }
    }

    /* flex 容器 */
    int fdir = flex_dir_value((v = decls_get(st, "flex-direction")) != NULL ? v : "row");
    if (fdir < 0) {
        set_error("%s: 不支持的 flex-direction '%s'", where, v);
        return 0;
    }
    int jc = justify_value((v = decls_get(st, "justify-content")) != NULL ? v
                                                                          : "flex-start");
    if (jc < 0) {
        set_error("%s: 不支持的 justify-content '%s'", where, v);
        return 0;
    }
    int ai = align_value((v = decls_get(st, "align-items")) != NULL ? v : "stretch");
    if (ai < 0) {
        set_error("%s: 不支持的 align-items '%s'", where, v);
        return 0;
    }
    int wrap = 0;
    v = decls_get(st, "flex-wrap");
    if (v != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        if (strcmp(low, "wrap") == 0)
            wrap = 1;
        else if (strcmp(low, "nowrap") != 0) {
            set_error("%s: 不支持的 flex-wrap '%s'（只支持 nowrap/wrap）", where, v);
            return 0;
        }
    }
    int gap_r = 0, gap_c = 0;
    {
        int kind = 0;
        double num = 0;
        if ((v = decls_get(st, "row-gap")) != NULL) {
            if (!parse_number(v, where, 1, 1, &kind, &num))
                return 0;
            gap_r = (int)num;
        }
        if ((v = decls_get(st, "column-gap")) != NULL) {
            if (!parse_number(v, where, 1, 1, &kind, &num))
                return 0;
            gap_c = (int)num;
        }
    }

    /* flex 项 */
    int grow = 0, shrink = 100;
    {
        const char *grow_txt = decls_get(st, "flex-grow");
        const char *shrink_txt = decls_get(st, "flex-shrink");
        double d = 0;
        const char *end = NULL;
        if (grow_txt != NULL) {
            if (!match_number(skip_space(grow_txt), &end, &d) ||
                *skip_space(end) != '\0') {
                set_error("%s: flex-grow/flex-shrink 必须是数字", where);
                return 0;
            }
            grow = (int)py_round(d * 100.0);
        }
        if (shrink_txt != NULL) {
            if (!match_number(skip_space(shrink_txt), &end, &d) ||
                *skip_space(end) != '\0') {
                set_error("%s: flex-grow/flex-shrink 必须是数字", where);
                return 0;
            }
            shrink = (int)py_round(d * 100.0);
        }
        if (grow < 0 || shrink < 0) {
            set_error("%s: flex-grow/flex-shrink 不能为负", where);
            return 0;
        }
    }

    int basis_px = -1, basis_pct = 0;
    {
        int kind = 0;
        double bv = 0;
        if (!parse_number((v = decls_get(st, "flex-basis")) != NULL ? v : "auto", where,
                          1, 1, &kind, &bv))
            return 0;
        if (kind == NUM_AUTO) {
            basis_px = -1;
            basis_pct = 0;
        } else if (kind == NUM_PCT) {
            basis_px = 0;
            basis_pct = ((int)bv > 1) ? (int)bv : 1;
        } else {
            basis_px = (int)bv;
            basis_pct = 0;
        }
    }

    int pos = position_value((v = decls_get(st, "position")) != NULL ? v : "static");
    if (pos < 0) {
        set_error("%s: 不支持的 position '%s'", where, v);
        return 0;
    }
    int ov = overflow_value((v = decls_get(st, "overflow")) != NULL ? v : "visible");
    if (ov < 0) {
        set_error("%s: 不支持的 overflow '%s'", where, v);
        return 0;
    }
    int zidx = 0;
    v = decls_get(st, "z-index");
    if (v != NULL) {
        const char *end = NULL;
        double d = 0;
        if (!match_number(skip_space(v), &end, &d) || *skip_space(end) != '\0') {
            set_error("%s: z-index 必须是整数", where);
            return 0;
        }
        if (d < -32768 || d > 32767) {
            set_error("%s: z-index 超出 int16 范围", where);
            return 0;
        }
        zidx = (int)d;
    }

    QLUILength inset[4];
    {
        static const char *const sides[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) {
            int kind = 0;
            double num = 0;
            if (!parse_number((v = decls_get(st, sides[i])) != NULL ? v : "auto", where,
                              1, 1, &kind, &num))
                return 0;
            if (kind == NUM_AUTO) {
                inset[i].value = 0;
                inset[i].unit = QL_UI_UNIT_AUTO;
            } else if (kind == NUM_PCT) {
                inset[i].value = (int16_t)((num > 0) ? num : 0);
                inset[i].unit = QL_UI_UNIT_PERCENT;
            } else {
                inset[i].value = (int16_t)num;
                inset[i].unit = QL_UI_UNIT_PX;
            }
            inset[i].reserved = 0;
        }
    }

    /* 过渡 */
    int trans_ms = 0, trans_props = 0, trans_ease = QL_EASE_LINEAR;
    if ((v = decls_get(st, "transition-duration")) != NULL) {
        if (!parse_duration(v, where, "transition-duration", &trans_ms))
            return 0;
        const char *tp = decls_get(st, "transition-property");
        char low[32] = "all";
        if (tp != NULL)
            trim_lower(tp, low, sizeof(low));
        if (strcmp(low, "all") == 0)
            trans_props = 127;
        else if (strcmp(low, "background-color") == 0)
            trans_props = 1;
        else if (strcmp(low, "opacity") == 0)
            trans_props = 2;
        else if (strcmp(low, "color") == 0)
            trans_props = 4;
        else if (strcmp(low, "border-color") == 0)
            trans_props = 8;
        else if (strcmp(low, "border-radius") == 0)
            trans_props = 16;
        else if (strcmp(low, "transform") == 0)
            trans_props = 32;
        else if (strcmp(low, "clip-path") == 0)
            trans_props = 64;
        else {
            set_error("%s: 暂不支持的 transition-property '%s'", where, low);
            return 0;
        }
        const char *ttf = decls_get(st, "transition-timing-function");
        char tlow[32] = "ease";
        if (ttf != NULL)
            trim_lower(ttf, tlow, sizeof(tlow));
        if (!easing_value(tlow, &trans_ease)) {
            set_error("%s: 暂不支持的 transition-timing-function '%s'", where, tlow);
            return 0;
        }
    }

    /* 毛玻璃 */
    int backdrop = 0;
    if ((v = decls_get(st, "backdrop-filter")) != NULL) {
        char low[64];
        size_t k = 0;
        int prev = 0;
        const char *s = skip_space(v);
        for (; *s != '\0' && k + 1u < sizeof(low); s++) {
            if (is_space(*s)) {
                if (!prev) {
                    low[k++] = ' ';
                    prev = 1;
                }
            } else {
                char c = *s;
                low[k++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
                prev = 0;
            }
        }
        while (k > 0 && low[k - 1] == ' ')
            k--;
        low[k] = '\0';
        if (strcmp(low, "none") != 0) {
            /* blur(<num>px) */
            if (strncmp(low, "blur(", 5) != 0 || low[k - 1] != ')') {
                set_error("%s: backdrop-filter 只支持 blur(<N>px)（收到 '%s'）", where,
                          v);
                return 0;
            }
            char num[64];
            size_t n = k - 6u;      /* 去掉 "blur(" 与 ")" */
            memcpy(num, low + 5, n);
            num[n] = '\0';
            size_t nn = strlen(num);
            if (nn <= 2 || strcmp(num + nn - 2u, "px") != 0) {
                set_error("%s: backdrop-filter 只支持 blur(<N>px)（收到 '%s'）", where,
                          v);
                return 0;
            }
            num[nn - 2u] = '\0';
            double d = 0;
            const char *end = NULL;
            if (!match_number(num, &end, &d) || *end != '\0') {
                set_error("%s: backdrop-filter 只支持 blur(<N>px)（收到 '%s'）", where,
                          v);
                return 0;
            }
            backdrop = (int)py_round(d);
            if (backdrop < 1 || backdrop > 64) {
                set_error("%s: backdrop-filter: blur 半径必须在 1~64px", where);
                return 0;
            }
        }
    }

    int clip_kind = 0, clip_r = 0;
    if (!parse_clip_path((v = decls_get(st, "clip-path")) != NULL ? v : "none", where,
                         &clip_kind, &clip_r))
        return 0;

    int tf_x = 0, tf_y = 0, sc_x = 1000, sc_y = 1000;
    if (!parse_transform((v = decls_get(st, "transform")) != NULL ? v : "none", where,
                         &tf_x, &tf_y, &sc_x, &sc_y))
        return 0;

    int anim_idx = 0xFFFF;
    if (!anim_index_of(st, where, ranges, kftab, anims, &anim_idx))
        return 0;

    /* 动画里动 clip-path 时，基础样式也要把 clip_kind 打开 */
    if (clip_kind == 0 && anim_idx != 0xFFFF) {
        const QLUIAnimation *an = &anims->a[anim_idx];
        for (int k = an->first_frame;
             k < (int)an->first_frame + (int)an->frame_count; k++) {
            if ((kftab->f[k].props & KF_CLIP) != 0) {
                clip_kind = 1;
                clip_r = (kftab->f[k].clip_r > 0) ? kftab->f[k].clip_r : 1000;
                break;
            }
        }
    }

    out->bg_color = bg;
    out->text_color = fg;
    out->border_color = bd;
    out->bg_color2 = bg2;
    out->shadow_color = sh_color;
    out->width.value = (int16_t)wv;
    out->width.unit = wu;
    out->width.reserved = 0;
    out->height.value = (int16_t)hv;
    out->height.unit = hu;
    out->height.reserved = 0;
    out->margin.top = (int16_t)margin[0];
    out->margin.right = (int16_t)margin[1];
    out->margin.bottom = (int16_t)margin[2];
    out->margin.left = (int16_t)margin[3];
    out->padding.top = (int16_t)padding[0];
    out->padding.right = (int16_t)padding[1];
    out->padding.bottom = (int16_t)padding[2];
    out->padding.left = (int16_t)padding[3];
    out->shadow_dx = (int16_t)sh_dx;
    out->shadow_dy = (int16_t)sh_dy;
    out->display = (uint8_t)disp;
    out->bg_alpha = bg_a;
    out->border_width = (uint8_t)bw;
    out->border_radius = (int16_t)radius;
    out->text_align = (uint8_t)align;
    out->font_id = (uint8_t)font_id;
    out->box_sizing = (uint8_t)box_sizing;
    out->bg_grad_dir = (uint8_t)grad_dir;
    out->shadow_blur = (uint8_t)sh_blur;
    out->shadow_alpha = sh_a;
    out->flex_direction = (uint8_t)fdir;
    out->justify_content = (uint8_t)jc;
    out->align_items = (uint8_t)ai;
    out->flex_wrap = (uint8_t)wrap;
    out->gap_row = (int16_t)gap_r;
    out->gap_col = (int16_t)gap_c;
    out->flex_grow = (uint16_t)grow;
    out->flex_shrink = (uint16_t)shrink;
    out->flex_basis = (int16_t)basis_px;
    out->flex_basis_pct = (uint16_t)basis_pct;
    out->position = (uint8_t)pos;
    out->overflow = (uint8_t)ov;
    out->z_index = (int16_t)zidx;
    for (int i = 0; i < 4; i++)
        out->inset[i] = inset[i];
    out->trans_ms = (uint16_t)trans_ms;
    out->trans_ease = (uint8_t)trans_ease;
    out->trans_props = (uint8_t)trans_props;
    out->clip_kind = (uint8_t)clip_kind;
    out->clip_r = (uint16_t)clip_r;
    out->tf_x = (int16_t)tf_x;
    out->tf_y = (int16_t)tf_y;
    out->sc_x = (int16_t)sc_x;
    out->sc_y = (int16_t)sc_y;
    out->anim = (uint16_t)anim_idx;
    out->backdrop_blur = (uint8_t)backdrop;
    out->line_h = (int16_t)line_h;
    out->white_space = (uint8_t)white_space;
    out->font_flags = font_flags;
    return 1;
}

/* ============================== ⑧ 建表 ============================== */

typedef struct {
    QLUIStyle st;
    char     *sig;
    size_t    siglen;
} StyleEnt;

typedef struct {
    StyleEnt *e;
    int       n;
    int       cap;
} StyleTab;

typedef struct {
    char    *pool;
    size_t   len;
    size_t   cap;
    uint16_t *ofs;
    int       n;
    int       ofs_cap;
} TextPool;

/* 样式表：按 (属性, 值) 集合去重。首次出现的顺序即下标 */
static int style_tab_add(StyleTab *tab, const Decls *decls, const char *where,
                         const KfRanges *ranges, const KfTable *kftab,
                         AnimTable *anims, IntList *fonts, uint16_t *out)
{
    size_t siglen = 0;
    char *sig = style_signature(decls, &siglen);
    if (sig == NULL)
        return 0;

    for (int i = 0; i < tab->n; i++) {
        if (tab->e[i].siglen == siglen && memcmp(tab->e[i].sig, sig, siglen) == 0) {
            ql_mem_free(sig);
            *out = (uint16_t)i;
            return 1;
        }
    }

    if (tab->n >= (int)QL_CFG_UI_MAX_STYLES) {
        set_error("样式表超过上限 %d", (int)QL_CFG_UI_MAX_STYLES);
        ql_mem_free(sig);
        return 0;
    }
    if (tab->n == tab->cap) {
        int cap = (tab->cap > 0) ? tab->cap * 2 : 16;
        StyleEnt *ne = (StyleEnt *)ql_mem_realloc(tab->e, (size_t)cap * sizeof(StyleEnt));
        if (ne == NULL) {
            ql_mem_free(sig);
            return 0;
        }
        tab->e = ne;
        tab->cap = cap;
    }
    StyleEnt *e = &tab->e[tab->n];
    memset(e, 0, sizeof(*e));
    if (!compile_style(decls, where, ranges, kftab, anims, fonts, &e->st)) {
        ql_mem_free(sig);
        return 0;
    }
    e->sig = sig;
    e->siglen = siglen;
    *out = (uint16_t)tab->n++;
    return 1;
}

static int text_pool_intern(TextPool *tp, const char *text, uint16_t *out)
{
    if (text == NULL || text[0] == '\0') {
        *out = 0xFFFF;
        return 1;
    }
    for (int i = 0; i < tp->n; i++) {
        if (strcmp(tp->pool + tp->ofs[i], text) == 0) {
            *out = (uint16_t)i;
            return 1;
        }
    }
    size_t len = strlen(text);
    if (tp->n >= (int)QL_CFG_UI_MAX_TEXT_NODES) {
        set_error("文本条数超过上限 %d", (int)QL_CFG_UI_MAX_TEXT_NODES);
        return 0;
    }
    if (tp->len + len + 1u > (size_t)QL_CFG_UI_MAX_TEXT_BYTES) {
        set_error("文本池超过上限 %d 字节", (int)QL_CFG_UI_MAX_TEXT_BYTES);
        return 0;
    }
    if (tp->len + len + 1u > tp->cap) {
        size_t cap = (tp->cap > 0) ? tp->cap : 256u;
        while (cap < tp->len + len + 1u)
            cap *= 2u;
        char *np = (char *)ql_mem_realloc(tp->pool, cap);
        if (np == NULL)
            return 0;
        tp->pool = np;
        tp->cap = cap;
    }
    if (tp->n == tp->ofs_cap) {
        int cap = (tp->ofs_cap > 0) ? tp->ofs_cap * 2 : 16;
        uint16_t *no = (uint16_t *)ql_mem_realloc(tp->ofs,
                                                  (size_t)cap * sizeof(uint16_t));
        if (no == NULL)
            return 0;
        tp->ofs = no;
        tp->ofs_cap = cap;
    }
    /* 偏移是 uint16：超过 65535 就没法表示（与 blob 的限制一致） */
    if (tp->len > 0xFFFFu) {
        set_error("文本池偏移超出 16 位（上限 %d 字节）", 0xFFFF);
        return 0;
    }
    tp->ofs[tp->n] = (uint16_t)tp->len;
    memcpy(tp->pool + tp->len, text, len);
    tp->len += len;
    tp->pool[tp->len++] = '\0';
    *out = (uint16_t)tp->n++;
    return 1;
}

/* 扁平化后的一项 */
typedef struct {
    const PNode *node;
    int          parent;
    int          first_child;
    int          next_sibling;
    int          style_index;
    int          state[3];        /* hover / active / focus */
    int          text_index;
    Decls        final_decls;     /* 供子节点继承 */
} Flat;

static char *g_doc_name;

void ql_ui_text_set_name(const char *name)
{
    ql_mem_free(g_doc_name);
    g_doc_name = (name != NULL) ? xstrdup(name) : NULL;
}

/* 深度优先展开（保证 parent 下标 < 子节点下标），与 Python 的 walk() 一致。
 * 递归深度已由解析期的 QL_CFG_UI_MAX_DEPTH 保证（<= 32 层），不会爆栈。 */
typedef struct {
    Flat *v;
    int   n;
    int   cap;
} FlatVec;

static int walk_flat(FlatVec *fv, const PNode *n, int parent)
{
    if (fv->n >= (int)QL_CFG_UI_MAX_NODES) {
        set_error("节点数超过上限 %d", (int)QL_CFG_UI_MAX_NODES);
        return -1;
    }
    if (fv->n == fv->cap) {
        int cap = (fv->cap > 0) ? fv->cap * 2 : 64;
        Flat *nv = (Flat *)ql_mem_realloc(fv->v, (size_t)cap * sizeof(Flat));
        if (nv == NULL)
            return -1;
        fv->v = nv;
        fv->cap = cap;
    }
    const int my = fv->n++;
    memset(&fv->v[my], 0, sizeof(Flat));
    fv->v[my].node = n;
    fv->v[my].parent = parent;
    fv->v[my].first_child = -1;
    fv->v[my].next_sibling = -1;
    fv->v[my].style_index = -1;
    fv->v[my].state[0] = fv->v[my].state[1] = fv->v[my].state[2] = -1;
    fv->v[my].text_index = -1;

    int prev = -1;
    for (const PNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        const int ci = walk_flat(fv, c, my);
        if (ci < 0)
            return -1;
        if (fv->v[my].first_child < 0)
            fv->v[my].first_child = ci;
        if (prev >= 0)
            fv->v[prev].next_sibling = ci;
        prev = ci;
    }
    return my;
}

static int flatten_tree(const PNode *root, Flat **out, int *out_n)
{
    if (root->first_child == NULL || root->first_child->next_sibling != NULL) {
        int n = 0;
        for (const PNode *c = root->first_child; c != NULL; c = c->next_sibling)
            n++;
        set_error("文档必须有且只有一个根元素，当前 %d 个", n);
        return 0;
    }

    FlatVec fv;
    memset(&fv, 0, sizeof(fv));
    if (walk_flat(&fv, root->first_child, -1) < 0) {
        for (int i = 0; i < fv.n; i++)
            decls_free(&fv.v[i].final_decls);
        ql_mem_free(fv.v);
        return 0;
    }
    *out = fv.v;
    *out_n = fv.n;
    return 1;
}

/* 继承只带走这三个属性（与 Python 的 INHERITED 一致） */
static const char *const k_inherited[] = { "color", "font-size", "text-align",
                                           "line-height", "white-space",
                                           "font-weight", "font-style",
                                           "text-decoration" };

static int collect_inherited(const Decls *parent, Decls *out)
{
    for (size_t i = 0; i < sizeof(k_inherited) / sizeof(k_inherited[0]); i++) {
        const char *v = decls_get(parent, k_inherited[i]);
        if (v != NULL && !decls_set(out, k_inherited[i], v))
            return 0;
    }
    return 1;
}

QLUIDoc *ql_ui_load_text(const char *html, const char *css)
{
    s_error[0] = '\0';
    s_degraded_tags = 0;
    if (html == NULL)
        html = "";
    if (css == NULL)
        css = "";

    PNode *root = NULL;
    Rules rules;
    KfDefs kfs;
    KfTable kftab;
    KfRanges kfranges;
    AnimTable anims;
    StyleTab styles;
    TextPool pool;
    IntList fonts;
    Flat *flat = NULL;
    int nflat = 0;
    QLUIDoc *doc = NULL;
    void *arena = NULL;

    memset(&rules, 0, sizeof(rules));
    memset(&kfs, 0, sizeof(kfs));
    memset(&kftab, 0, sizeof(kftab));
    memset(&kfranges, 0, sizeof(kfranges));
    memset(&anims, 0, sizeof(anims));
    memset(&styles, 0, sizeof(styles));
    memset(&pool, 0, sizeof(pool));
    memset(&fonts, 0, sizeof(fonts));

    root = parse_html(html);
    if (root == NULL)
        goto done;
    if (!parse_css(css, &rules, &kfs))
        goto done;
    if (!kf_compile(&kfs, &kftab, &kfranges))
        goto done;
    if (!flatten_tree(root, &flat, &nflat))
        goto done;

    /* 逐节点算样式（DFS 顺序天然父先子后，继承直接取父的最终声明） */
    for (int i = 0; i < nflat; i++) {
        Flat *e = &flat[i];
        const PNode *n = e->node;

        Decls inherited;
        memset(&inherited, 0, sizeof(inherited));
        if (e->parent >= 0 && !collect_inherited(&flat[e->parent].final_decls,
                                                 &inherited)) {
            decls_free(&inherited);
            goto done;
        }

        Decls base;
        memset(&base, 0, sizeof(base));
        if (!collect_decls(n, &rules, 0, &base)) {
            decls_free(&base);
            decls_free(&inherited);
            goto done;
        }
        /* 错误定位：元素在扁平表里的下标 + 标签名。
         * 拿不到 CSS 行号——级联之后"这条声明来自哪条规则"已经不存在了。 */
        char where[96];
        snprintf(where, sizeof(where), "<%s #%d>",
                 n->tag != NULL ? n->tag : "?", i);
        if (!finalize_style(&base, n, &inherited, &e->final_decls)) {
            decls_free(&base);
            decls_free(&inherited);
            goto done;
        }
        decls_free(&base);
        decls_free(&inherited);

        uint16_t idx = 0xFFFF;
        if (!style_tab_add(&styles, &e->final_decls, where, &kfranges, &kftab, &anims,
                           &fonts, &idx))
            goto done;
        e->style_index = idx;

        /* 状态变体：只在真的不同时才建（没写 :hover 的节点省 3 个下标） */
        char *base_sig = NULL;
        size_t base_siglen = 0;
        base_sig = style_signature(&e->final_decls, &base_siglen);
        if (base_sig == NULL)
            goto done;

        static const int bits[3] = { 1, 2, 4 };
        for (int bi = 0; bi < 3; bi++) {
            Decls vdecl;
            memset(&vdecl, 0, sizeof(vdecl));
            Decls vinherited;
            memset(&vinherited, 0, sizeof(vinherited));
            Decls vbase;
            memset(&vbase, 0, sizeof(vbase));
            Decls vfinal;
            memset(&vfinal, 0, sizeof(vfinal));

            if (e->parent >= 0 &&
                !collect_inherited(&flat[e->parent].final_decls, &vinherited)) {
                decls_free(&vbase);
                decls_free(&vinherited);
                ql_mem_free(base_sig);
                goto done;
            }
            if (!collect_decls(n, &rules, bits[bi], &vbase)) {
                decls_free(&vbase);
                decls_free(&vinherited);
                ql_mem_free(base_sig);
                goto done;
            }
            if (!finalize_style(&vbase, n, &vinherited, &vfinal)) {
                decls_free(&vbase);
                decls_free(&vinherited);
                decls_free(&vfinal);
                ql_mem_free(base_sig);
                goto done;
            }

            /* 动画在 ql_ui_build 时按基础样式起播，写在伪类里永远不会触发 →
             * 与其静默失效，不如明确报错 */
            const char *van = decls_get(&vfinal, "animation-name");
            const char *ban = decls_get(&e->final_decls, "animation-name");
            if (van != NULL && strcmp(van, "none") != 0 &&
                (ban == NULL || strcmp(van, ban) != 0)) {
                set_error("<%s> 的状态规则里不支持 animation：关键帧动画只按基础样式起播",
                          n->tag);
                decls_free(&vbase);
                decls_free(&vinherited);
                decls_free(&vfinal);
                ql_mem_free(base_sig);
                goto done;
            }

            size_t vlen = 0;
            char *vsig = style_signature(&vfinal, &vlen);
            if (vsig == NULL) {
                decls_free(&vbase);
                decls_free(&vinherited);
                decls_free(&vfinal);
                ql_mem_free(base_sig);
                goto done;
            }
            if (vlen == base_siglen && memcmp(vsig, base_sig, vlen) == 0) {
                e->state[bi] = 0xFFFF;
            } else {
                uint16_t vidx = 0xFFFF;
                if (!style_tab_add(&styles, &vfinal, where, &kfranges, &kftab, &anims,
                                   &fonts, &vidx)) {
                    ql_mem_free(vsig);
                    decls_free(&vbase);
                    decls_free(&vinherited);
                    decls_free(&vfinal);
                    ql_mem_free(base_sig);
                    goto done;
                }
                e->state[bi] = vidx;
            }
            ql_mem_free(vsig);
            decls_free(&vbase);
            decls_free(&vinherited);
            decls_free(&vfinal);
            (void)vdecl;
        }
        ql_mem_free(base_sig);

        uint16_t tidx = 0xFFFF;
        if (!text_pool_intern(&pool, n->text, &tidx))
            goto done;
        e->text_index = tidx;
    }

    /* ------------------------------ 一次分配 arena ------------------------------ */
    {
        const char *nm = (g_doc_name != NULL) ? g_doc_name : "text";
        const size_t name_len = strlen(nm) + 1u;

        size_t off = 0;
        const size_t off_nodes = off;
        off += QL_UI_ALIGN8((size_t)nflat * sizeof(QLUINode));
        const size_t off_styles = off;
        off += QL_UI_ALIGN8((size_t)styles.n * sizeof(QLUIStyle));
        const size_t off_pool = off;
        off += QL_UI_ALIGN8(pool.len + 1u);
        const size_t off_tofs = off;
        off += QL_UI_ALIGN8((size_t)((pool.n > 0) ? pool.n : 1) * sizeof(uint16_t));
        const size_t off_fonts = off;
        off += QL_UI_ALIGN8((size_t)fonts.n * sizeof(uint16_t));
        const size_t off_kf = off;
        off += QL_UI_ALIGN8((size_t)kftab.n * sizeof(QLUIKeyframe));
        const size_t off_anims = off;
        off += QL_UI_ALIGN8((size_t)anims.n * sizeof(QLUIAnimation));
        const size_t off_name = off;
        off += QL_UI_ALIGN8(name_len);
        const size_t total = off;

        arena = ql_mem_calloc(1, total);
        if (arena == NULL) {
            set_error("out of memory");
            goto done;
        }
        uint8_t *base = (uint8_t *)arena;

        doc = (QLUIDoc *)ql_mem_calloc(1, sizeof(QLUIDoc));
        if (doc == NULL) {
            set_error("out of memory");
            ql_mem_free(arena);
            arena = NULL;
            goto done;
        }

        QLUINode *nodes = (QLUINode *)(void *)(base + off_nodes);
        for (int i = 0; i < nflat; i++) {
            QLUINode *nd = &nodes[i];
            nd->tag = (uint16_t)tag_value(flat[i].node->tag);
            nd->parent = (uint16_t)((flat[i].parent >= 0) ? flat[i].parent : 0xFFFF);
            nd->first_child =
                (uint16_t)((flat[i].first_child >= 0) ? flat[i].first_child : 0xFFFF);
            nd->next_sibling = (uint16_t)((flat[i].next_sibling >= 0)
                                              ? flat[i].next_sibling
                                              : 0xFFFF);
            nd->style = (uint16_t)flat[i].style_index;
            nd->text = (uint16_t)flat[i].text_index;
            nd->style_hover = (uint16_t)flat[i].state[0];
            nd->style_active = (uint16_t)flat[i].state[1];
            nd->style_focus = (uint16_t)flat[i].state[2];
            nd->flags = flat[i].node->flags;
        }

        QLUIStyle *stl = (QLUIStyle *)(void *)(base + off_styles);
        for (int i = 0; i < styles.n; i++)
            stl[i] = styles.e[i].st;

        char *pool_dst = (char *)(base + off_pool);
        if (pool.len > 0)
            memcpy(pool_dst, pool.pool, pool.len);

        uint16_t *tofs = (uint16_t *)(void *)(base + off_tofs);
        for (int i = 0; i < pool.n; i++)
            tofs[i] = pool.ofs[i];

        uint16_t *fnt = (uint16_t *)(void *)(base + off_fonts);
        for (int i = 0; i < fonts.n; i++)
            fnt[i] = (uint16_t)fonts.v[i];

        QLUIKeyframe *kfd = (QLUIKeyframe *)(void *)(base + off_kf);
        for (int i = 0; i < kftab.n; i++)
            kfd[i] = kftab.f[i];

        QLUIAnimation *and = (QLUIAnimation *)(void *)(base + off_anims);
        for (int i = 0; i < anims.n; i++)
            and[i] = anims.a[i];

        char *nmd = (char *)(base + off_name);
        memcpy(nmd, nm, name_len);

        /*
         * 空的文本池也要占一条：构建期工具在"没有文本"时生成
         * `text_ofs[] = { 0 }` 占位（C 不允许零长数组），于是 text_count == 1、
         * 池里是一个空字符串。这里必须照抄，否则 blob 头就不同。
         */
        if (pool.n == 0) {
            pool_dst[0] = '\0';
            tofs[0] = 0;
        }

        doc->name = nmd;
        doc->node_count = (uint16_t)nflat;
        doc->style_count = (uint16_t)styles.n;
        doc->text_count = (uint16_t)((pool.n > 0) ? pool.n : 1);
        doc->font_size_count = (uint16_t)fonts.n;
        doc->anim_count = (uint16_t)anims.n;
        doc->keyframe_count = (uint16_t)kftab.n;
        doc->degraded_tags = s_degraded_tags;
        doc->nodes = (nflat > 0) ? nodes : NULL;
        doc->styles = (styles.n > 0) ? stl : NULL;
        doc->text_pool = pool_dst;
        doc->text_offsets = tofs;
        doc->font_sizes = (fonts.n > 0) ? fnt : NULL;
        doc->anims = (anims.n > 0) ? and : NULL;
        doc->keyframes = (kftab.n > 0) ? kfd : NULL;
        doc->arena = arena;
        arena = NULL;         /* 所有权交给 doc */
    }

done:
    if (flat != NULL) {
        for (int i = 0; i < nflat; i++)
            decls_free(&flat[i].final_decls);
        ql_mem_free(flat);
    }
    if (root != NULL)
        pnode_free(root);
    for (int i = 0; i < styles.n; i++) {
        ql_mem_free(styles.e[i].sig);
    }
    ql_mem_free(styles.e);
    ql_mem_free(kftab.f);
    ql_mem_free(anims.a);
    ql_mem_free(kfranges.r);
    ql_mem_free(pool.pool);
    ql_mem_free(pool.ofs);
    ql_mem_free(fonts.v);
    kfdefs_free(&kfs);
    rules_free(&rules);
    if (arena != NULL)
        ql_mem_free(arena);

    return doc;
}

