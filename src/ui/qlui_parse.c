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
        if (n == 8) {
            for (int i = 0; i < 8; i++) {
                if (hex_value(h[i]) < 0) {
                    set_error("%s: 非法颜色 '%s'", where, text);
                    return 0;
                }
            }
            char rgb6[7];
            memcpy(rgb6, h, 6);
            rgb6[6] = '\0';
            *rgb = (uint32_t)strtoul(rgb6, NULL, 16);
            char a2[3] = { h[6], h[7], '\0' };
            *alpha = (uint8_t)strtoul(a2, NULL, 16);
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

    /* `currentColor` / `inherit` / `initial`：由调用方在解析期替换成具体值，
       这里只把它们标成"特殊值"，用 alpha=0xFF 且 rgb=0xFFFFFFFF 表示（不可能是真色） */
    if (strcmp(t, "currentcolor") == 0) {
        *rgb = 0xFFFFFFFFu;
        *alpha = 255u;
        return 1;
    }
    if (strcmp(t, "inherit") == 0 || strcmp(t, "initial") == 0 ||
        strcmp(t, "unset") == 0) {
        /* `inherit` 对继承属性本来就是默认行为；对非继承属性等同 `initial`。
           两种都归到 "special"，由 finalize 决定取父值还是取初值。 */
        *rgb = (strcmp(t, "inherit") == 0) ? 0xFFFFFFFEu : 0xFFFFFFFDu;
        *alpha = 255u;
        return 1;
    }

    /* hsl(h, s%, l%) / hsla(h, s%, l%, a) */
    if (strncmp(t, "hsl", 3) == 0 && (t[3] == '(' || t[3] == 'a')) {
        int is_hsla = (t[3] == 'a');
        const char *open = strchr(t, '(');
        const char *close = strrchr(t, ')');
        if (open == NULL || close == NULL || close < open)
            goto bad_hsl;
        double v[4] = { 0, 0, 0, 0 };
        int nv = 0;
        const char *p = open + 1;
        while (p < close && nv < 4) {
            p = skip_space(p);
            const char *end = NULL;
            double num = 0;
            if (!match_number(p, &end, &num))
                break;
            p = skip_space(end);
            if (*p == '%')
                p++;                  /* s / l 的百分号 */
            else if (*p == 'd' && p[1] == 'e' && p[2] == 'g')
                p += 3;               /* h 的 deg */
            v[nv++] = num;
            p = skip_space(p);
            if (p < close && *p == ',')
                p++;
            else
                break;
        }
        p = skip_space(p);
        if (nv != (is_hsla ? 4 : 3) || p != close)
            goto bad_hsl;
        if (is_hsla && (v[3] < 0 || v[3] > 1))
            goto bad_hsl;
        if (v[1] < 0 || v[1] > 100 || v[2] < 0 || v[2] > 100)
            goto bad_hsl;
        /* HSL → RGB（标准算法，h 取模 360） */
        double hh = v[0];
        while (hh < 0)
            hh += 360.0;
        while (hh >= 360.0)
            hh -= 360.0;
        const double sN = v[1] / 100.0, lN = v[2] / 100.0;
        const double cc = (1.0 - fabs(2.0 * lN - 1.0)) * sN;
        const double hp = hh / 60.0;
        const double x = cc * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
        double r1 = 0, g1 = 0, b1 = 0;
        if (hp < 1)      { r1 = cc; g1 = x; }
        else if (hp < 2) { r1 = x;  g1 = cc; }
        else if (hp < 3) { g1 = cc; b1 = x; }
        else if (hp < 4) { g1 = x;  b1 = cc; }
        else if (hp < 5) { r1 = x;  b1 = cc; }
        else             { r1 = cc; b1 = x; }
        const double m = lN - cc / 2.0;
        const int ri = (int)py_round((r1 + m) * 255.0);
        const int gi = (int)py_round((g1 + m) * 255.0);
        const int bi = (int)py_round((b1 + m) * 255.0);
        *rgb = ((uint32_t)(ri & 0xFF) << 16) | ((uint32_t)(gi & 0xFF) << 8) |
               (uint32_t)(bi & 0xFF);
        *alpha = is_hsla ? (uint8_t)py_round(v[3] * 255.0) : 255u;
        return 1;
    bad_hsl:
        set_error("%s: 不支持的颜色写法 '%s'（hsl 需要 hsl(h, s%%, l%%) / hsla(h, s%%, l%%, a)）",
                  where, text);
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

/* ===================== calc() / min() / max() / clamp() ===================== */

/*
 * 产出是一段**后缀字节码**（见 `QLUICalcOp`）。不在构建期直接把值算出来的原因
 * 很简单：表达式里可能有 `%` / `vw` / `vh`，这些只有在布局期才知道基准。
 *
 * 构建期做的是**常量折叠**：标量乘除、纯数字加减、`min(1,2)` 这类全数字子式都
 * 在编译时折掉，所以字节码里剩下的操作数永远是"带单位的长度"。
 */

typedef struct {
    QLUICalcOp *ops;
    int         n;
    int         cap;
} CalcPool;

#define QL_CALC_MAX_NODES 48   /* 一条表达式最多几个节点 */
#define QL_CALC_MAX_ARGS  8    /* min()/max() 最多几个参数 */
#define QL_CALC_MAX_DEPTH 16   /* 求值栈深度上限（与 qlui.c 的 QL_CALC_STACK_MAX 一致） */

enum {
    CN_LEN = 0,   /* 操作数：`unit` + `v`（与 QLUILength 的存储约定一致） */
    CN_NUM,       /* 纯数字（只在构建期存在，不进字节码） */
    CN_ADD,
    CN_SUB,
    CN_MUL,
    CN_DIV,
    CN_MIN,
    CN_MAX,
    CN_CLAMP,
};

typedef struct {
    uint8_t kind;
    uint8_t unit;
    int16_t v;      /* CN_LEN：px / % / em*100 … */
    double  num;    /* CN_NUM */
    int     a, b;   /* 二元运算的子节点 */
    int     args[QL_CALC_MAX_ARGS];
    int     nargs;
} CNode;

typedef struct {
    const char *p;
    const char *where;
    const char *src;   /* 原始文本，报错时回显 */
    int         allow_percent;
    CNode       nodes[QL_CALC_MAX_NODES];
    int         nn;
} CalcParse;

static int calc_fail(CalcParse *cp, const char *msg)
{
    set_error("%s: %s — '%s'", cp->where, msg, cp->src);
    return 0;
}

static int calc_new_node(CalcParse *cp, uint8_t kind)
{
    if (cp->nn >= QL_CALC_MAX_NODES)
        return -1;
    CNode *n = &cp->nodes[cp->nn];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->a = n->b = -1;
    return cp->nn++;
}

/* 读一个操作数：<number><unit>；没写单位就是纯数字 */
static int calc_parse_operand(CalcParse *cp, int *out)
{
    const char *p = skip_space(cp->p);
    const char *end = NULL;
    double num = 0;
    if (!match_number(p, &end, &num)) {
        calc_fail(cp, "这里需要一个数字");
        return 0;
    }
    const char *u = end;
    while ((*u >= 'a' && *u <= 'z') || *u == '%')
        u++;
    const size_t ulen = (size_t)(u - end);
    int unit = -1;              /* -1 = 纯数字 */
    int16_t v = 0;

    if (ulen == 0) {
        unit = -1;
    } else if (ulen == 2 && memcmp(end, "px", 2) == 0) {
        unit = QL_UI_UNIT_PX;
    } else if (ulen == 1 && end[0] == '%') {
        unit = QL_UI_UNIT_PERCENT;
    } else if (ulen == 2 && memcmp(end, "em", 2) == 0) {
        unit = QL_UI_UNIT_EM;
    } else if (ulen == 3 && memcmp(end, "rem", 3) == 0) {
        unit = QL_UI_UNIT_REM;
    } else if (ulen == 2 && memcmp(end, "vw", 2) == 0) {
        unit = QL_UI_UNIT_VW;
    } else if (ulen == 2 && memcmp(end, "vh", 2) == 0) {
        unit = QL_UI_UNIT_VH;
    } else if (ulen == 4 && memcmp(end, "vmin", 4) == 0) {
        unit = QL_UI_UNIT_VMIN;
    } else if (ulen == 4 && memcmp(end, "vmax", 4) == 0) {
        unit = QL_UI_UNIT_VMAX;
    } else {
        calc_fail(cp, "calc() 里不支持这个单位（支持 px / % / em / rem / vw / vh / vmin / vmax）");
        return 0;
    }

    if (unit != -1 && unit != QL_UI_UNIT_PX && unit != QL_UI_UNIT_EM &&
        unit != QL_UI_UNIT_REM && !cp->allow_percent) {
        calc_fail(cp, "此处不支持百分比 / 视口单位");
        return 0;
    }
    /* em / rem 存百分之一 em（与 parse_length 同一套约定） */
    if (unit == QL_UI_UNIT_EM || unit == QL_UI_UNIT_REM)
        v = (int16_t)py_round(num * 100.0);
    else if (unit != -1)
        v = (int16_t)py_round(num);

    const int idx = calc_new_node(cp, (uint8_t)((unit < 0) ? CN_NUM : CN_LEN));
    if (idx < 0) {
        calc_fail(cp, "calc() 表达式太长");
        return 0;
    }
    cp->nodes[idx].unit = (uint8_t)((unit < 0) ? 0 : (unsigned)unit);
    cp->nodes[idx].v = v;
    cp->nodes[idx].num = num;
    cp->p = u;
    *out = idx;
    return 1;
}

static int calc_parse_expr(CalcParse *cp, int *out);

/* 括号、嵌套函数、操作数 */
static int calc_parse_primary(CalcParse *cp, int *out)
{
    const char *p = skip_space(cp->p);
    if (*p == '(') {
        cp->p = p + 1;
        if (!calc_parse_expr(cp, out))
            return 0;
        cp->p = skip_space(cp->p);
        if (*cp->p != ')') {
            calc_fail(cp, "缺少 ')'");
            return 0;
        }
        cp->p++;
        return 1;
    }
    if (strncmp(p, "calc(", 5) == 0) {
        cp->p = p + 5;
        if (!calc_parse_expr(cp, out))
            return 0;
        cp->p = skip_space(cp->p);
        if (*cp->p != ')') {
            calc_fail(cp, "缺少 ')'");
            return 0;
        }
        cp->p++;
        return 1;
    }

    uint8_t kind = 0xFF;
    if (strncmp(p, "min(", 4) == 0) {
        kind = CN_MIN;
        cp->p = p + 4;
    } else if (strncmp(p, "max(", 4) == 0) {
        kind = CN_MAX;
        cp->p = p + 4;
    } else if (strncmp(p, "clamp(", 6) == 0) {
        kind = CN_CLAMP;
        cp->p = p + 6;
    }
    if (kind != 0xFF) {
        const int idx = calc_new_node(cp, kind);
        if (idx < 0) {
            calc_fail(cp, "calc() 表达式太长");
            return 0;
        }
        for (;;) {
            if (cp->nodes[idx].nargs >= QL_CALC_MAX_ARGS) {
                calc_fail(cp, "min()/max() 的参数太多");
                return 0;
            }
            int arg = -1;
            if (!calc_parse_expr(cp, &arg))
                return 0;
            cp->nodes[idx].args[cp->nodes[idx].nargs++] = arg;
            cp->p = skip_space(cp->p);
            if (*cp->p != ',')
                break;
            cp->p++;
        }
        if (*cp->p != ')') {
            calc_fail(cp, "缺少 ')'");
            return 0;
        }
        cp->p++;
        if (kind == CN_CLAMP && cp->nodes[idx].nargs != 3) {
            calc_fail(cp, "clamp() 需要 3 个参数");
            return 0;
        }
        *out = idx;
        return 1;
    }

    return calc_parse_operand(cp, out);
}

/* 乘除 */
static int calc_parse_term(CalcParse *cp, int *out)
{
    int left = -1;
    if (!calc_parse_primary(cp, &left))
        return 0;
    for (;;) {
        const char *p = skip_space(cp->p);
        if (*p != '*' && *p != '/')
            break;
        const uint8_t kind = (*p == '*') ? CN_MUL : CN_DIV;
        cp->p = p + 1;
        int right = -1;
        if (!calc_parse_primary(cp, &right))
            return 0;
        const int idx = calc_new_node(cp, kind);
        if (idx < 0) {
            calc_fail(cp, "calc() 表达式太长");
            return 0;
        }
        cp->nodes[idx].a = left;
        cp->nodes[idx].b = right;
        left = idx;
    }
    *out = left;
    return 1;
}

/* 加减 */
static int calc_parse_expr(CalcParse *cp, int *out)
{
    int left = -1;
    if (!calc_parse_term(cp, &left))
        return 0;
    for (;;) {
        const char *p = skip_space(cp->p);
        if (*p != '+' && *p != '-')
            break;
        const uint8_t kind = (*p == '+') ? CN_ADD : CN_SUB;
        cp->p = p + 1;
        int right = -1;
        if (!calc_parse_term(cp, &right))
            return 0;
        const int idx = calc_new_node(cp, kind);
        if (idx < 0) {
            calc_fail(cp, "calc() 表达式太长");
            return 0;
        }
        cp->nodes[idx].a = left;
        cp->nodes[idx].b = right;
        left = idx;
    }
    *out = left;
    return 1;
}

/* 标量乘除的构建期折叠：把 num/den 乘进子树里的每个操作数 */
static int calc_scale(CalcParse *cp, int i, double num, double den)
{
    CNode *n = &cp->nodes[i];
    if (n->kind == CN_NUM) {
        n->num = n->num * num / den;
        return 1;
    }
    if (n->kind == CN_LEN) {
        const double v = (double)n->v * num / den;
        if (v > 32767.0 || v < -32768.0) {
            calc_fail(cp, "calc() 的操作数超出范围");
            return 0;
        }
        n->v = (int16_t)py_round(v);
        return 1;
    }
    if (n->kind == CN_MIN || n->kind == CN_MAX || n->kind == CN_CLAMP) {
        if (num < 0) {
            calc_fail(cp, "负数只能乘单个操作数（min()/max()/clamp() 的大小关系会被翻转）");
            return 0;
        }
        for (int k = 0; k < n->nargs; k++)
            if (!calc_scale(cp, n->args[k], num, den))
                return 0;
        return 1;
    }
    if (!calc_scale(cp, n->a, num, den))
        return 0;
    return calc_scale(cp, n->b, num, den);
}

/* 把一个节点变成纯数字（折叠掉） */
static void calc_as_num(CalcParse *cp, int i, double v)
{
    CNode *n = &cp->nodes[i];
    n->kind = CN_NUM;
    n->num = v;
    n->nargs = 0;
    n->a = n->b = -1;
}

/*
 * 常量折叠。返回 1 表示这棵子树是**纯数字**（值写进 `*num`），0 表示它是长度表达式。
 * 失败时返回 -1（错误已设）。
 */
static int calc_fold(CalcParse *cp, int i, double *num)
{
    CNode *n = &cp->nodes[i];
    switch (n->kind) {
    case CN_NUM:
        *num = n->num;
        return 1;
    case CN_LEN:
        return 0;
    case CN_ADD:
    case CN_SUB: {
        const uint8_t kind = n->kind;
        double a = 0, b = 0;
        const int fa = calc_fold(cp, n->a, &a);
        if (fa < 0)
            return -1;
        const int fb = calc_fold(cp, n->b, &b);
        if (fb < 0)
            return -1;
        if (fa && fb) {
            calc_as_num(cp, i, (kind == CN_ADD) ? (a + b) : (a - b));
            *num = cp->nodes[i].num;
            return 1;
        }
        if (fa || fb) {
            calc_fail(cp, "calc() 不能把纯数字与长度相加减");
            return -1;
        }
        /* 同单位的两个操作数直接折成一个（`calc(10px + 6px)` → 16px），
         * 这样"只放得下 px 的字段"（font-size / border-*）也能用 calc() */
        {
            const CNode *la = &cp->nodes[n->a];
            const CNode *lb = &cp->nodes[n->b];
            if (la->kind == CN_LEN && lb->kind == CN_LEN && la->unit == lb->unit) {
                const int v = (kind == CN_ADD) ? ((int)la->v + (int)lb->v)
                                               : ((int)la->v - (int)lb->v);
                if (v > 32767 || v < -32768) {
                    calc_fail(cp, "calc() 的操作数超出范围");
                    return -1;
                }
                cp->nodes[i].kind = CN_LEN;
                cp->nodes[i].unit = la->unit;
                cp->nodes[i].v = (int16_t)v;
                cp->nodes[i].nargs = 0;
                return 0;
            }
        }
        return 0;
    }
    case CN_MUL:
    case CN_DIV: {
        const uint8_t kind = n->kind;
        double a = 0, b = 0;
        const int fa = calc_fold(cp, n->a, &a);
        if (fa < 0)
            return -1;
        const int fb = calc_fold(cp, n->b, &b);
        if (fb < 0)
            return -1;
        if (fa && fb) {
            if (kind == CN_DIV) {
                if (b == 0) {
                    calc_fail(cp, "calc() 里除以 0");
                    return -1;
                }
                calc_as_num(cp, i, a / b);
            } else {
                calc_as_num(cp, i, a * b);
            }
            *num = cp->nodes[i].num;
            return 1;
        }
        /* 长度 ×/÷ 长度 没有意义 */
        if (!fa && !fb) {
            calc_fail(cp, "calc() 不支持长度相乘/相除（只能乘除纯数字）");
            return -1;
        }
        const int lhs = fa ? n->b : n->a;     /* 长度那一侧 */
        const double s = fa ? a : b;
        if (kind == CN_DIV && fa) {
            calc_fail(cp, "calc() 不支持用长度去除纯数字");
            return -1;
        }
        const double mul = (kind == CN_MUL) ? s : 1.0;
        const double div = (kind == CN_DIV) ? s : 1.0;
        if (div == 0) {
            calc_fail(cp, "calc() 里除以 0");
            return -1;
        }
        if (!calc_scale(cp, lhs, mul, div))
            return -1;
        /* 结果就是被缩放的那棵子树：把父节点"变成"它 */
        const CNode sub = cp->nodes[lhs];
        cp->nodes[i] = sub;
        return 0;
    }
    case CN_MIN:
    case CN_MAX:
    case CN_CLAMP: {
        double vals[QL_CALC_MAX_ARGS];
        int nlen = 0, nnum = 0;
        for (int k = 0; k < n->nargs; k++) {
            double v = 0;
            const int r = calc_fold(cp, n->args[k], &v);
            if (r < 0)
                return -1;
            if (r)
                nnum++;
            else
                nlen++;
            vals[k] = v;
        }
        if (nnum > 0 && nlen > 0) {
            calc_fail(cp, "min()/max()/clamp() 的参数必须都是长度（不能混纯数字）");
            return -1;
        }
        if (nnum > 0) {
            double v = vals[0];
            for (int k = 1; k < n->nargs; k++) {
                if (n->kind == CN_CLAMP) {
                    if (k == 1) {
                        v = vals[0];
                        continue;
                    }
                }
                if (n->kind == CN_MIN)
                    v = (vals[k] < v) ? vals[k] : v;
                else if (n->kind == CN_MAX)
                    v = (vals[k] > v) ? vals[k] : v;
            }
            if (n->kind == CN_CLAMP) {
                v = vals[1];
                if (v < vals[0])
                    v = vals[0];
                if (v > vals[2])
                    v = vals[2];
            }
            calc_as_num(cp, i, v);
            *num = v;
            return 1;
        }
        /* 参数全是同单位的操作数 → 直接折成一个（`min(10px, 4px)` → 4px） */
        {
            const uint8_t u0 = cp->nodes[n->args[0]].unit;
            int all_same = 1;
            for (int k = 0; k < n->nargs; k++) {
                const CNode *a = &cp->nodes[n->args[k]];
                if (a->kind != CN_LEN || a->unit != u0) {
                    all_same = 0;
                    break;
                }
            }
            if (all_same) {
                int v = cp->nodes[n->args[0]].v;
                for (int k = 1; k < n->nargs; k++) {
                    const int c = cp->nodes[n->args[k]].v;
                    if (n->kind == CN_MIN)
                        v = (c < v) ? c : v;
                    else if (n->kind == CN_MAX)
                        v = (c > v) ? c : v;
                }
                if (n->kind == CN_CLAMP) {
                    const int lo = cp->nodes[n->args[0]].v;
                    const int val = cp->nodes[n->args[1]].v;
                    const int hi = cp->nodes[n->args[2]].v;
                    v = (val < lo) ? lo : ((val > hi) ? hi : val);
                }
                cp->nodes[i].kind = CN_LEN;
                cp->nodes[i].unit = u0;
                cp->nodes[i].v = (int16_t)v;
                cp->nodes[i].nargs = 0;
                return 0;
            }
        }
        return 0;
    }
    default:
        break;
    }
    calc_fail(cp, "calc() 表达式非法");
    return -1;
}

/* 把折叠后的树写成后缀字节码；返回新的栈深度，-1 表示出错 */
static int calc_emit(CalcParse *cp, int i, CalcPool *pool, int depth)
{
    const CNode *n = &cp->nodes[i];
    switch (n->kind) {
    case CN_LEN: {
        if (depth >= QL_CALC_MAX_DEPTH) {
            calc_fail(cp, "calc() 嵌套太深");
            return -1;
        }
        if (pool->n >= pool->cap) {
            calc_fail(cp, "calc() 指令池已满（QL_CFG_UI_MAX_CALC_OPS）");
            return -1;
        }
        QLUICalcOp *op = &pool->ops[pool->n++];
        op->op = QL_UI_CALC_TERM;
        op->unit = n->unit;
        op->value = n->v;
        return depth + 1;
    }
    case CN_ADD:
    case CN_SUB: {
        int d = calc_emit(cp, n->a, pool, depth);
        if (d < 0)
            return -1;
        d = calc_emit(cp, n->b, pool, d);
        if (d < 0)
            return -1;
        if (pool->n >= pool->cap) {
            calc_fail(cp, "calc() 指令池已满（QL_CFG_UI_MAX_CALC_OPS）");
            return -1;
        }
        QLUICalcOp *op = &pool->ops[pool->n++];
        op->op = (n->kind == CN_ADD) ? QL_UI_CALC_ADD : QL_UI_CALC_SUB;
        op->unit = 0;
        op->value = 0;
        return d - 1;
    }
    case CN_MIN:
    case CN_MAX:
    case CN_CLAMP: {
        int d = depth;
        for (int k = 0; k < n->nargs; k++) {
            d = calc_emit(cp, n->args[k], pool, d);
            if (d < 0)
                return -1;
        }
        if (pool->n >= pool->cap) {
            calc_fail(cp, "calc() 指令池已满（QL_CFG_UI_MAX_CALC_OPS）");
            return -1;
        }
        QLUICalcOp *op = &pool->ops[pool->n++];
        op->op = (n->kind == CN_MIN) ? QL_UI_CALC_MIN
                 : (n->kind == CN_MAX) ? QL_UI_CALC_MAX
                                       : QL_UI_CALC_CLAMP;
        op->unit = (uint8_t)n->nargs;
        op->value = 0;
        return d - (n->nargs - 1);
    }
    default:
        break;
    }
    calc_fail(cp, "calc() 表达式里还残留构建期节点");
    return -1;
}

/*
 * 入口：`text` 必须是**已转小写**的整段声明值（`calc(...)` / `min(...)` / …）。
 * 成功时写 `*unit`（`QL_UI_UNIT_CALC` 或退化成 `QL_UI_UNIT_PX`）与 `*value`
 * （池下标）。
 */
static int calc_compile(const char *text, const char *where, int allow_percent,
                        CalcPool *pool, uint8_t *unit, int *value)
{
    CalcParse cp;
    memset(&cp, 0, sizeof(cp));
    cp.where = where;
    cp.src = text;
    cp.allow_percent = allow_percent;

    if (pool == NULL || QL_CFG_UI_MAX_CALC_OPS == 0) {
        set_error("%s: calc()/min()/max()/clamp() 已被裁掉（QL_CFG_UI_MAX_CALC_OPS = 0）",
                  where);
        return 0;
    }

    const char *p = skip_space(text);
    int root = -1;
    const int is_calc = (strncmp(p, "calc(", 5) == 0);
    if (is_calc) {
        cp.p = p + 5;
        if (!calc_parse_expr(&cp, &root))
            return 0;
    } else if (strncmp(p, "min(", 4) == 0 || strncmp(p, "max(", 4) == 0 ||
               strncmp(p, "clamp(", 6) == 0) {
        /* 顶层就是 min()/max()/clamp()：primary 只管到函数自己的 ')' */
        cp.p = p;
        if (!calc_parse_primary(&cp, &root))
            return 0;
    } else {
        calc_fail(&cp, "不是 calc()/min()/max()/clamp() 表达式");
        return 0;
    }
    cp.p = skip_space(cp.p);
    if (is_calc) {
        if (*cp.p != ')') {
            calc_fail(&cp, "缺少 ')'");
            return 0;
        }
        cp.p++;
        cp.p = skip_space(cp.p);
    }
    if (*cp.p != '\0') {
        calc_fail(&cp, "表达式后面还有多余内容");
        return 0;
    }

    double v = 0;
    const int scalar = calc_fold(&cp, root, &v);
    if (scalar < 0)
        return 0;
    if (scalar == 0 && cp.nodes[root].kind == CN_LEN &&
        cp.nodes[root].unit == QL_UI_UNIT_PX) {
        /* 折到只剩一个 px 操作数（`calc(10px + 6px)`）：不占池 */
        *unit = QL_UI_UNIT_PX;
        *value = cp.nodes[root].v;
        return 1;
    }
    if (scalar) {
        /* 全常量的表达式（`calc(2 * 3)`）：直接折成 px，不占池 */
        if (v > 32767.0 || v < -32768.0) {
            calc_fail(&cp, "calc() 的结果超出范围");
            return 0;
        }
        *unit = QL_UI_UNIT_PX;
        *value = (int)py_round(v);
        return 1;
    }

    /* 先在临时缓冲里生成，成功后才并进池（失败不能留下半条表达式） */
    QLUICalcOp tmp[64];
    CalcPool scratch;
    scratch.ops = tmp;
    scratch.n = 0;
    scratch.cap = (int)(sizeof(tmp) / sizeof(tmp[0]));
    if (calc_emit(&cp, root, &scratch, 0) != 1) {
        if (s_error[0] == '\0')
            calc_fail(&cp, "calc() 表达式非法");
        return 0;
    }
    tmp[scratch.n].op = QL_UI_CALC_END;
    tmp[scratch.n].unit = 0;
    tmp[scratch.n].value = 0;
    const int need = scratch.n + 1;

    if (pool->n + need > (int)QL_CFG_UI_MAX_CALC_OPS) {
        set_error("%s: calc() 指令池超过上限 %d 条（QL_CFG_UI_MAX_CALC_OPS）", where,
                  (int)QL_CFG_UI_MAX_CALC_OPS);
        return 0;
    }
    if (pool->n + need > pool->cap) {
        int cap = (pool->cap > 0) ? pool->cap : 32;
        while (cap < pool->n + need)
            cap *= 2;
        if (cap > (int)QL_CFG_UI_MAX_CALC_OPS)
            cap = (int)QL_CFG_UI_MAX_CALC_OPS;
        QLUICalcOp *np = (QLUICalcOp *)ql_mem_realloc(
            pool->ops, (size_t)cap * sizeof(QLUICalcOp));
        if (np == NULL) {
            set_error("%s: out of memory", where);
            return 0;
        }
        pool->ops = np;
        pool->cap = cap;
    }
    const int start = pool->n;
    for (int k = 0; k < need; k++)
        pool->ops[pool->n++] = tmp[k];
    *unit = QL_UI_UNIT_CALC;
    *value = start;
    return 1;
}

/* 长度：auto / <num>px / <num>%。unit 用 QL_UI_UNIT_* */
static int parse_length(const char *text, const char *where, int allow_percent,
                        CalcPool *calc, uint8_t *unit, int *value)
{
    char t[128];
    trim_lower(text, t, sizeof(t));

    if (strncmp(t, "calc(", 5) == 0 || strncmp(t, "min(", 4) == 0 ||
        strncmp(t, "max(", 4) == 0 || strncmp(t, "clamp(", 6) == 0)
        return calc_compile(t, where, allow_percent, calc, unit, value);

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

    /* 相对单位：em / rem 存"百分之一 em"，vw / vh 存整数百分比 */
    if (n > 2 && (strcmp(t + n - 2u, "em") == 0)) {
        char num[128];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *unit = QL_UI_UNIT_EM;
            *value = (int)py_round(v * 100.0);
            return 1;
        }
    }
    if (n > 3 && strcmp(t + n - 3u, "rem") == 0) {
        char num[128];
        memcpy(num, t, n - 3u);
        num[n - 3u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            *unit = QL_UI_UNIT_REM;
            *value = (int)py_round(v * 100.0);
            return 1;
        }
    }
    if (n > 4 && (strcmp(t + n - 4u, "vmin") == 0 || strcmp(t + n - 4u, "vmax") == 0)) {
        char num[128];
        memcpy(num, t, n - 4u);
        num[n - 4u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            if (!allow_percent) {
                set_error("%s: 此处不支持视口单位 '%s'", where, text);
                return 0;
            }
            /* "vmin" 的倒数第二个字符是 'i'，"vmax" 是 'a' */
            *unit = (t[n - 2u] == 'i') ? QL_UI_UNIT_VMIN : QL_UI_UNIT_VMAX;
            *value = (int)py_round(v);
            return 1;
        }
    }
    if (n > 2 && (strcmp(t + n - 2u, "vw") == 0 || strcmp(t + n - 2u, "vh") == 0)) {
        char num[128];
        memcpy(num, t, n - 2u);
        num[n - 2u] = '\0';
        const char *end = NULL;
        double v = 0;
        if (match_number(num, &end, &v) && *end == '\0') {
            if (!allow_percent) {
                set_error("%s: 此处不支持视口单位 '%s'", where, text);
                return 0;
            }
            *unit = (t[n - 1u] == 'w') ? QL_UI_UNIT_VW : QL_UI_UNIT_VH;
            *value = (int)py_round(v);
            return 1;
        }
    }

    set_error("%s: 不支持的长度 '%s'（支持 px / %% / em / rem / vw / vh）", where, text);
    return 0;
}

/*
 * 有些字段只放得下 int16 数值（font-size / border-* / letter-spacing …），
 * 没有位置存表达式下标 —— 这类字段只接受**构建期就能折成 px** 的 calc()。
 */
static int length_wants_const(uint8_t unit, const char *where, const char *prop)
{
    if (unit != QL_UI_UNIT_CALC)
        return 1;
    set_error("%s: %s 只接受构建期就能算成 px 的值（calc() 里不能带 %%/em/rem/vw/vh）",
              where, prop);
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
 * margin/padding 的 1/2/4 值语法。输出顺序写 (上,右,下,左)。
 *
 * 接受 px / em / rem；`auto` 只对 margin 有效（CSS 里 padding 不能 auto）。
 * `%` 与 `vw/vh` 在这里被拒绝：边距的百分比相对的是**包含块宽度**，而本库的
 * 布局在算边距时还没有那个基准 —— 与其算个大概，不如明确报错。
 */
static int parse_edges(const char *text, const char *where, int allow_auto,
                       CalcPool *calc, QLUILength out[4])
{
    QLUILength vals[4];
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
        uint8_t unit = QL_UI_UNIT_PX;
        int v = 0;
        if (!parse_length(tok, where, 0, calc, &unit, &v))
            return 0;
        if (!length_wants_const(unit, where, "margin/padding"))
            return 0;
        if (unit == QL_UI_UNIT_AUTO && !allow_auto) {
            set_error("%s: padding 不支持 auto", where);
            return 0;
        }
        if (unit == QL_UI_UNIT_PERCENT || unit == QL_UI_UNIT_VW ||
            unit == QL_UI_UNIT_VH || unit == QL_UI_UNIT_VMIN ||
            unit == QL_UI_UNIT_VMAX) {
            set_error("%s: margin/padding 暂不支持 %% / vw / vh / vmin / vmax"
                      "（用 px / em / rem）",
                      where);
            return 0;
        }
        vals[n].value = (int16_t)v;
        vals[n].unit = unit;
        vals[n].reserved = 0;
        n++;
    }
    if (n == 1) {
        for (int i = 0; i < 4; i++)
            out[i] = vals[0];
    } else if (n == 2) {
        out[0] = vals[0];
        out[2] = vals[0];
        out[1] = vals[1];
        out[3] = vals[1];
    } else if (n == 4) {
        for (int i = 0; i < 4; i++)
            out[i] = vals[i];
    } else {
        set_error("%s: margin/padding 需要 1/2/4 个值", where);
        return 0;
    }
    return 1;
}

/* 2 位/边：0=px 1=em 2=rem 3=auto */
static uint8_t edges_unit_bits(const QLUILength e[4])
{
    uint8_t bits = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t u = 0;
        if (e[i].unit == QL_UI_UNIT_EM)
            u = 1;
        else if (e[i].unit == QL_UI_UNIT_REM)
            u = 2;
        else if (e[i].unit == QL_UI_UNIT_AUTO)
            u = 3;
        bits = (uint8_t)(bits | (uint8_t)(u << (2 * i)));
    }
    return bits;
}

/*
 * 边距里的 em / rem 折成 px（值存的是百分之一 em）。`auto` 原样留着，
 * 由布局按"剩余空间"分配。
 */
static void resolve_edge_units(QLUIEdges *e, uint8_t *bits, int font_px, int root_px)
{
    int16_t *v[4] = { &e->top, &e->right, &e->bottom, &e->left };
    for (int i = 0; i < 4; i++) {
        const uint8_t u = (uint8_t)((*bits >> (2 * i)) & 0x3u);
        if (u == 1 && font_px > 0) {
            *v[i] = (int16_t)(((int32_t)*v[i] * font_px) / 100);
            *bits = (uint8_t)(*bits & (uint8_t)~(uint8_t)(0x3u << (2 * i)));
        } else if (u == 2 && root_px > 0) {
            *v[i] = (int16_t)(((int32_t)*v[i] * root_px) / 100);
            *bits = (uint8_t)(*bits & (uint8_t)~(uint8_t)(0x3u << (2 * i)));
        }
    }
}

/*
 * 把 `em` / `rem` 折成 px（就地改，单位换成 PX）。
 *
 * `em` 相对**元素自己的字号**，`rem` 相对**根元素**字号 —— 与 CSS 一致。
 * `value` 存的是百分之一 em，所以这里除以 100。
 *
 * 两趟是刻意的：`em` 在编译每个样式时就能折（字号已知）；`rem` 要等整棵树走完
 * 才知道根元素用的是哪个字号（根的 `font-size` 本身也可能是 em/px），所以留到
 * 最后统一折一遍。已经是 px 的字段直接跳过，零成本。
 */
static void resolve_length_units(QLUIStyle *s, int font_px, int root_px,
                                 CalcPool *calc)
{
    QLUILength *all[10];
    all[0] = &s->width;
    all[1] = &s->height;
    all[2] = &s->min_width;
    all[3] = &s->max_width;
    all[4] = &s->min_height;
    all[5] = &s->max_height;
    for (int i = 0; i < 4; i++)
        all[6 + i] = &s->inset[i];
    for (int i = 0; i < 10; i++) {
        QLUILength *l = all[i];
        if (l->unit == QL_UI_UNIT_EM && font_px > 0) {
            l->value = (int16_t)(((int32_t)l->value * font_px) / 100);
            l->unit = QL_UI_UNIT_PX;
        } else if (l->unit == QL_UI_UNIT_REM && root_px > 0) {
            l->value = (int16_t)(((int32_t)l->value * root_px) / 100);
            l->unit = QL_UI_UNIT_PX;
        } else if (l->unit == QL_UI_UNIT_CALC) {
            /* 表达式里的 em / rem 也要折 —— 池是文档级共享的，就地改 */
            if (calc != NULL && calc->ops != NULL) {
                for (int k = l->value; k >= 0 && k < calc->n; k++) {
                    QLUICalcOp *op = &calc->ops[k];
                    if (op->op == QL_UI_CALC_END)
                        break;
                    if (op->op != QL_UI_CALC_TERM)
                        continue;
                    if (op->unit == QL_UI_UNIT_EM && font_px > 0) {
                        op->value = (int16_t)(((int32_t)op->value * font_px) / 100);
                        op->unit = QL_UI_UNIT_PX;
                    } else if (op->unit == QL_UI_UNIT_REM && root_px > 0) {
                        op->value = (int16_t)(((int32_t)op->value * root_px) / 100);
                        op->unit = QL_UI_UNIT_PX;
                    }
                }
            }
        }
    }
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
    /*
     * 允许**负值**：`scaleX(-1)` 就是水平镜像，翻转动画的后半程靠它。
     * 0 也合法（压成一条线）。范围按 int16 卡在 ±32.767。
     */
    if (sc < -32767 || sc > 32767) {
        set_error("%s: scale 倍数必须在 -32.767~32.767（收到 '%s'）", where, fn);
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
    uint8_t important;    /* 1 = 带 !important（级联时最后覆盖） */
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
            ds->d[i].important = 0;
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
    ds->d[ds->n].important = 0;
    ds->n++;
    return 1;
}

/* ---------------------- CSS 变量（`--x` / `var()`） ---------------------- */

/*
 * 构建期替换 `var()`。
 *
 * 为什么能在构建期做干净：本库的样式本来就是**编译产物**，`--x` 没有"运行期改值"
 * 这条路（也没有 JS），所以整个替换只是"级联之后再查一次表"。做完这一步，
 * 每个元素手里的声明值都是字面量，`compile_style()` 完全不知道变量的存在。
 *
 * 三条规则：
 *   - `--x` **继承**（已由 collect_inherited 带下来）；
 *   - `var(--x)` 找不到且没有 fallback → **报错**（不静默留成字面量）；
 *   - 允许嵌套与 `var(--x, <fallback>)`；靠深度上限挡环（`--a: var(--b); --b: var(--a)`）。
 */
#define QL_VAR_MAX_DEPTH 8

/* 在 [s, s+len) 里找 `var(` 的位置；找不到返回 NULL */
static const char *find_var_call(const char *s)
{
    const char *p = s;
    while ((p = strstr(p, "var(")) != NULL) {
        /* 必须是"独立的" var(：前一个字符不能是标识符的一部分 */
        if (p == s)
            return p;
        const char c = p[-1];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return p;
        p += 4;
    }
    return NULL;
}

/*
 * 把一段值里的 `var()` 全部替换掉，返回新分配的字符串。
 * `scope` 里放的是这个元素最终可见的声明（含继承来的 `--x`）。
 * 出错时 set_error 并返回 NULL。
 */
static char *subst_vars(const char *val, const Decls *scope, const char *where,
                        int depth)
{
    if (val == NULL)
        return NULL;
    const char *hit = find_var_call(val);
    if (hit == NULL)
        return xstrdup(val);
    if (depth > QL_VAR_MAX_DEPTH) {
        set_error("%s: var() 嵌套太深或出现循环引用（'%s'）", where, val);
        return NULL;
    }

    /* 解析 `var(` 之后的内容：名字到 ',' 或 ')'，可选 fallback */
    const char *p = hit + 4;
    while (is_space(*p))
        p++;
    const char *name_start = p;
    while (*p != '\0' && *p != ',' && *p != ')')
        p++;
    const char *name_end = p;
    while (name_end > name_start && is_space(name_end[-1]))
        name_end--;

    char name[64];
    const size_t nlen = (size_t)(name_end - name_start);
    if (nlen == 0 || nlen >= sizeof(name)) {
        set_error("%s: var() 里的变量名不合法（'%s'）", where, val);
        return NULL;
    }
    for (size_t i = 0; i < nlen; i++) {
        const char c = name_start[i];
        name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    name[nlen] = '\0';
    if (name[0] != '-' || name[1] != '-') {
        set_error("%s: var() 里必须是自定义属性名（形如 `--x`，收到 '%s'）", where,
                  name);
        return NULL;
    }

    /* 找 fallback（可能含括号，要配对） */
    const char *fallback = NULL;
    const char *call_end = p;
    if (*p == ',') {
        fallback = p + 1;
        int nest = 1;
        const char *q = p;
        while (*q != '\0' && nest > 0) {
            if (*q == '(')
                nest++;
            else if (*q == ')')
                nest--;
            q++;
        }
        if (nest != 0) {
            set_error("%s: var() 的括号没配对（'%s'）", where, val);
            return NULL;
        }
        call_end = q;                    /* 指向 ')' 之后 */
        while (fallback < call_end - 1 && is_space(*fallback))
            fallback++;
        const char *fb_end = call_end - 1;      /* ')' 的位置 */
        while (fb_end > fallback && is_space(fb_end[-1]))
            fb_end--;
        /* 记进临时缓冲，下面统一替换 */
        char *fb = xstrndup(fallback, (size_t)(fb_end - fallback));
        if (fb == NULL)
            return NULL;
        char *repl = (decls_get(scope, name) != NULL)
                         ? subst_vars(decls_get(scope, name), scope, where, depth + 1)
                         : subst_vars(fb, scope, where, depth + 1);
        ql_mem_free(fb);
        if (repl == NULL)
            return NULL;
        /* 拼：前半 + repl + 后半 */
        const size_t before = (size_t)(hit - val);
        const size_t after = strlen(call_end);
        char *out = (char *)xalloc(before + strlen(repl) + after + 1u);
        if (out == NULL) {
            ql_mem_free(repl);
            return NULL;
        }
        memcpy(out, val, before);
        memcpy(out + before, repl, strlen(repl));
        memcpy(out + before + strlen(repl), call_end, after + 1u);
        ql_mem_free(repl);
        char *again = subst_vars(out, scope, where, depth + 1);
        ql_mem_free(out);
        return again;
    }

    if (*p != ')') {
        set_error("%s: var() 写法不对（'%s'）", where, val);
        return NULL;
    }
    call_end = p + 1;

    const char *raw = decls_get(scope, name);
    if (raw == NULL) {
        set_error("%s: var(%s) 没有定义（也没有 fallback）", where, name);
        return NULL;
    }
    char *repl = subst_vars(raw, scope, where, depth + 1);
    if (repl == NULL)
        return NULL;
    const size_t before = (size_t)(hit - val);
    const size_t after = strlen(call_end);
    char *out = (char *)xalloc(before + strlen(repl) + after + 1u);
    if (out == NULL) {
        ql_mem_free(repl);
        return NULL;
    }
    memcpy(out, val, before);
    memcpy(out + before, repl, strlen(repl));
    memcpy(out + before + strlen(repl), call_end, after + 1u);
    ql_mem_free(repl);
    char *again = subst_vars(out, scope, where, depth + 1);
    ql_mem_free(out);
    return again;
}

/*
 * 把一个元素的最终声明表里所有 `var()` 替换成字面量（就地改）。
 * 顺序：先解析 `--x` 自己的值（它可能引用别的 `--x`），再替换普通属性。
 */
static int resolve_custom_props(Decls *ds, const char *where)
{
    int has_var = 0;
    for (int i = 0; i < ds->n && !has_var; i++)
        if (strstr(ds->d[i].val, "var(") != NULL)
            has_var = 1;
    if (!has_var)
        return 1;                        /* 常见情况：一个变量都没有，零成本 */

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ds->n; i++) {
            const int is_custom = (ds->d[i].key[0] == '-');
            if (is_custom != (pass == 0))
                continue;                /* 第一趟只处理 `--x`，第二趟处理其余 */
            if (strstr(ds->d[i].val, "var(") == NULL)
                continue;
            char *nv = subst_vars(ds->d[i].val, ds, where, 0);
            if (nv == NULL)
                return 0;
            ql_mem_free(ds->d[i].val);
            ds->d[i].val = nv;
        }
    }
    return 1;
}

/* 带 `!important` 的版本：其余逻辑与 decls_set 完全一致 */
static int decls_set_imp(Decls *ds, const char *key, const char *val, int important)
{
    if (!decls_set(ds, key, val))
        return 0;
    if (important) {
        for (int i = 0; i < ds->n; i++)
            if (strcmp(ds->d[i].key, key) == 0) {
                ds->d[i].important = 1;
                break;
            }
    }
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
/*
 * 可过渡属性的名字 → 位图。`all` 单独处理（= 全开）。
 * 简写和长写都查这张表，避免两处各写一份字符串比较。
 */
static const struct {
    const char *name;
    unsigned    bit;
} k_trans_props[] = {
    { "background-color", QL_UI_TRANS_BG },
    { "opacity",          QL_UI_TRANS_ALPHA },
    { "color",            QL_UI_TRANS_TEXT },
    { "border-color",     QL_UI_TRANS_BORDER },
    { "border-radius",    QL_UI_TRANS_RADIUS },
    { "transform",        QL_UI_TRANS_XFORM },
    { "clip-path",        QL_UI_TRANS_CLIP },
};
#define K_TRANS_PROPS_N (sizeof(k_trans_props) / sizeof(k_trans_props[0]))

/* 名字 → 位；`all` → QL_UI_TRANS_ALL；不认识返回 -1 */
static int trans_prop_bit(const char *name)
{
    if (strcmp(name, "all") == 0)
        return (int)QL_UI_TRANS_ALL;
    for (size_t i = 0; i < K_TRANS_PROPS_N; i++)
        if (strcmp(name, k_trans_props[i].name) == 0)
            return (int)k_trans_props[i].bit;
    return -1;
}

/* 位图 → 逗号分隔的名字表（`all` 直接写 all） */
static int trans_props_to_names(unsigned bits, char *out, size_t cap)
{
    if ((bits & QL_UI_TRANS_ALL) == QL_UI_TRANS_ALL)
        return snprintf(out, cap, "all") > 0;
    size_t k = 0;
    out[0] = '\0';
    for (size_t i = 0; i < K_TRANS_PROPS_N; i++) {
        if ((bits & k_trans_props[i].bit) == 0)
            continue;
        const int n = snprintf(out + k, cap - k, "%s%s", (k > 0) ? ", " : "",
                               k_trans_props[i].name);
        if (n < 0 || (size_t)n >= cap - k)
            return 0;
        k += (size_t)n;
    }
    return 1;
}

/* ======================= 简写展开（**必须在单条规则内**做） ======================= */
/*
 * 级联合并之后再展开会覆盖后出现的长写：
 *   `.a { animation: pulse 1s }` 与 `.b { animation-name: other }` 是两条规则，
 * 合并后 animation-name 已是 other，这时再展开 shorthand 会把它改回 pulse ——
 * 10 张卡片全部退化成同一段关键帧（真踩过）。所以只在 parse_decls 里展开。
 *
 * `transition` 的逗号写法（`background-color 200ms, opacity 200ms`）是**多组**，
 * 每组各写一遍属性/时长/缓动。库里只有一组过渡参数
 * （`QLUIStyle.trans_ms` / `trans_ease` 各一个），所以：
 *   - 属性取并集（位图本来就能存多个）；
 *   - 时长与缓动各组必须一致，不一致就报错 —— 不静默取其中一个。
 */
static int expand_transition(Decls *ds)
{
    const char *v = decls_get(ds, "transition");
    if (v == NULL)
        return 1;

    unsigned props = 0;
    char dur[128] = "";        /* 各组合并后的时长（各组必须相同） */
    char tf[128] = "";         /* 同上：缓动 */
    const char *p = v;

    for (int gi = 0;; gi++) {
        while (is_space(*p))
            p++;
        const char *gstart = p;
        while (*p != '\0' && *p != ',')
            p++;
        const char *gend = p;
        while (gend > gstart && is_space(gend[-1]))
            gend--;
        const int had_comma = (*p == ',');
        const char *next = had_comma ? p + 1 : p;

        if (gend == gstart) {
            set_error("transition: 第 %d 项是空的（多余的逗号？）", gi + 1);
            return 0;
        }

        /* 一组：属性 / 时长 / 缓动，顺序任意、各最多一个 */
        char gdur[128] = "";
        char gtf[128] = "";
        int group_has_prop = 0;
        const char *q = gstart;
        while (q < gend) {
            while (q < gend && is_space(*q))
                q++;
            if (q >= gend)
                break;
            const char *ts = q;
            while (q < gend && !is_space(*q))
                q++;
            char tok[128], low[128];
            size_t len = (size_t)(q - ts);
            if (len >= sizeof(tok))
                len = sizeof(tok) - 1u;
            memcpy(tok, ts, len);
            tok[len] = '\0';
            trim_lower(tok, low, sizeof(low));

            const int bit = trans_prop_bit(low);
            if (bit >= 0) {
                props |= (unsigned)bit;
                group_has_prop = 1;
            } else if (str_in(low, k_easings, 5)) {
                snprintf(gtf, sizeof(gtf), "%s", low);
            } else if (looks_like_duration(low)) {
                if (gdur[0] != '\0') {
                    /* CSS 里第二个时长是 delay —— 本库不实现，别静默忽略 */
                    set_error("transition: 暂不支持 delay（'%s'）", tok);
                    return 0;
                }
                snprintf(gdur, sizeof(gdur), "%s", low);
            } else {
                set_error("transition: 无法识别的部分 '%s'", tok);
                return 0;
            }
        }

        /*
         * 跨组合并。`transition: background-color 200ms, opacity 400ms` 这种
         * "每个属性一个时长"的写法本库表达不了 —— 只有一组 trans_ms。
         * 报错并给出解法，比悄悄按第一个时长插值所有属性好。
         */
        if (gdur[0] != '\0') {
            if (dur[0] == '\0')
                snprintf(dur, sizeof(dur), "%s", gdur);
            else if (strcmp(dur, gdur) != 0) {
                set_error("transition: 各段的时长必须相同（'%s' 与 '%s'）—— "
                          "本库只有一组过渡参数；要不同时长请拆成两层元素",
                          dur, gdur);
                return 0;
            }
        }
        if (gtf[0] != '\0') {
            if (tf[0] == '\0')
                snprintf(tf, sizeof(tf), "%s", gtf);
            else if (strcmp(tf, gtf) != 0) {
                set_error("transition: 各段的缓动必须相同（'%s' 与 '%s'）—— "
                          "本库只有一组过渡参数", tf, gtf);
                return 0;
            }
        }

        /* 没写属性的那一组等于 `all`（与 CSS 一致） */
        if (!group_has_prop)
            props |= QL_UI_TRANS_ALL;

        p = next;
        if (*p == '\0') {
            if (had_comma) {                 /* 末尾多一个逗号 = 空项 */
                set_error("transition: 第 %d 项是空的（多余的逗号？）", gi + 2);
                return 0;
            }
            break;
        }
    }

    if (props != 0) {
        char plist[192];
        if (!trans_props_to_names(props, plist, sizeof(plist))) {
            set_error("transition: 可过渡属性太多，写不下");
            return 0;
        }
        if (!decls_set(ds, "transition-property", plist))
            return 0;
    }
    if (dur[0] != '\0' && !decls_set(ds, "transition-duration", dur))
        return 0;
    if (tf[0] != '\0' && !decls_set(ds, "transition-timing-function", tf))
        return 0;
    (void)decls_remove(ds, "transition");
    return 1;
}

static int expand_shorthands(Decls *ds)
{
    const char *v = NULL;

    if (!expand_transition(ds))
        return 0;

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

    if ((v = decls_get(ds, "flex-flow")) != NULL) {
        /* `<flex-direction> || <flex-wrap>`，两个 token 任意顺序 */
        char buf[96];
        trim_lower(v, buf, sizeof(buf));
        const char *p = buf;
        int have_dir = 0, have_wrap = 0;
        while (*p != '\0') {
            while (is_space(*p))
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && !is_space(*p))
                p++;
            char tok[32];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(tok)) {
                set_error("flex-flow: 值太长（'%s'）", v);
                return 0;
            }
            memcpy(tok, start, len);
            tok[len] = '\0';
            if (strcmp(tok, "row") == 0 || strcmp(tok, "row-reverse") == 0 ||
                strcmp(tok, "column") == 0 || strcmp(tok, "column-reverse") == 0) {
                if (!decls_set(ds, "flex-direction", tok))
                    return 0;
                have_dir = 1;
            } else if (strcmp(tok, "nowrap") == 0 || strcmp(tok, "wrap") == 0) {
                if (!decls_set(ds, "flex-wrap", tok))
                    return 0;
                have_wrap = 1;
            } else {
                set_error("flex-flow: 不支持的取值 '%s'（只支持 <direction> || <wrap>）",
                          tok);
                return 0;
            }
        }
        if (!have_dir && !have_wrap) {
            set_error("flex-flow: 值是空的");
            return 0;
        }
        (void)decls_remove(ds, "flex-flow");
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

    if ((v = decls_get(ds, "inset")) != NULL) {
        /* `inset: <t> <r> <b> <l>`（1/2/4 值，与 margin 同一套语法） */
        QLUILength e[4];
        if (!parse_edges(v, "inset", 1, NULL, e))
            return 0;
        static const char *const names[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) {
            char nb[32];
            switch (e[i].unit) {
            case QL_UI_UNIT_AUTO:
                snprintf(nb, sizeof(nb), "auto");
                break;
            case QL_UI_UNIT_PERCENT:
                snprintf(nb, sizeof(nb), "%d%%", (int)e[i].value);
                break;
            case QL_UI_UNIT_EM:
                snprintf(nb, sizeof(nb), "%gem", (double)e[i].value / 100.0);
                break;
            case QL_UI_UNIT_REM:
                snprintf(nb, sizeof(nb), "%grem", (double)e[i].value / 100.0);
                break;
            default:
                snprintf(nb, sizeof(nb), "%dpx", (int)e[i].value);
                break;
            }
            if (!decls_set(ds, names[i], nb))
                return 0;
        }
        (void)decls_remove(ds, "inset");
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
                if (!decls_set(ds, "border-style", low))
                    return 0;
            } else {
                if (!decls_set(ds, "border-color", low))
                    return 0;
            }
        }
    }

    /*
     * `outline` 简写：`2px solid #f00` / `2px #f00` / `none`，三段任意顺序。
     * 与 `border` 共用同一套 token 判别（px / 关键字 / 其它当颜色）。
     */
    if ((v = decls_get(ds, "outline")) != NULL) {
        char buf[128];
        size_t n = strlen(v);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1u;
        memcpy(buf, v, n);
        buf[n] = '\0';
        (void)decls_remove(ds, "outline");

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
                if (!decls_set(ds, "outline-width", low))
                    return 0;
            } else if (strcmp(low, "solid") == 0 || strcmp(low, "none") == 0 ||
                       strcmp(low, "dashed") == 0 || strcmp(low, "dotted") == 0) {
                if (!decls_set(ds, "outline-style", low))
                    return 0;
            } else {
                if (!decls_set(ds, "outline-color", low))
                    return 0;
            }
        }
    }

    return 1;
}

/* ============================== ④ CSS 词法 ============================== */

static const char *const k_supported_props[] = {
    "display", "width", "height", "margin", "padding", "border", "border-width",
    "border-color", "border-radius", "border-style",
    "outline", "outline-width", "outline-color", "outline-style", "outline-offset",
    "background", "background-color", "color",
    "font-size", "text-align", "line-height", "white-space", "box-sizing",
    "vertical-align", "font-family",
    "object-fit",
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
    "min-width", "max-width", "min-height", "max-height",
    "opacity",
    "aspect-ratio",
    "letter-spacing", "text-indent", "text-transform", "text-overflow", "word-break",
    "overflow-wrap",
    "visibility", "pointer-events", "align-self", "order", "flex-flow",
    "align-content", "overflow-x", "overflow-y", "inset",
    "cursor", "user-select", "scroll-behavior",
    "content",          /* 只能用在 ::before / ::after 上（见 expand_pseudo） */
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

            /*
             * `!important`：从值尾部剥掉，记一个标记。
             * 级联时**普通声明先全应用、important 声明再应用一遍**，
             * 于是"带 important 的永远赢"而不必给每个声明加优先级排序。
             */
            int important = 0;
            {
                static const char k_imp[] = "!important";
                const size_t il = sizeof(k_imp) - 1u;
                if (vlen > il) {
                    size_t cut = vlen - il;
                    while (cut > 0 && is_space(val[cut - 1]))
                        cut--;              /* `red  !important` 也认 */
                    char tail[16];
                    size_t tl = vlen - (vlen - il);
                    if (tl < sizeof(tail)) {
                        memcpy(tail, val + vlen - il, il);
                        tail[il] = '\0';
                        for (size_t i = 0; i < il; i++)
                            if (tail[i] >= 'A' && tail[i] <= 'Z')
                                tail[i] = (char)(tail[i] - 'A' + 'a');
                        if (strcmp(tail, k_imp) == 0) {
                            important = 1;
                            vlen = cut;
                        }
                    }
                }
            }
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
                ok = decls_set_imp(out, prop, vs, important);
            } else {
                /*
                 * `--x` 是**自定义属性**（CSS 变量），构建期在级联之后替换掉
                 * `var(--x)`。它不是真属性，所以不查 `k_supported_props`。
                 */
                if (prop[0] != '-' && !in_supported_props(prop)) {
                    set_error("%s: 不支持的属性 '%s'", where, prop);
                    ql_mem_free(vs);
                    return 0;
                }
                ok = decls_set_imp(out, prop, vs, important);
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
#define SEL_COMB_ADJACENT   2   /* a + b：紧邻的前一个元素兄弟 */
#define SEL_COMB_SIBLING    3   /* a ~ b：前面任一元素兄弟 */

/* 结构伪类 / 属性选择器（构建期求值，运行期零成本） */
#define SELP_FIRST_CHILD      0x0001
#define SELP_LAST_CHILD       0x0002
#define SELP_ONLY_CHILD       0x0004
#define SELP_NTH_CHILD        0x0008
#define SELP_NTH_LAST_CHILD   0x0010
#define SELP_FIRST_OF_TYPE    0x0020
#define SELP_LAST_OF_TYPE     0x0040
#define SELP_ONLY_OF_TYPE     0x0080
#define SELP_NTH_OF_TYPE      0x0100
#define SELP_NTH_LAST_OF_TYPE 0x0200
#define SELP_ROOT             0x0400   /* `:root`：元素树最外层第一个元素 */

#define SEL_ATTR_EXISTS  0
#define SEL_ATTR_EQ      1
#define SEL_ATTR_PREFIX  2   /* ^= */
#define SEL_ATTR_SUFFIX  3   /* $= */
#define SEL_ATTR_SUBSTR  4   /* *= */
#define SEL_ATTR_WORD    5   /* ~= */

typedef struct {
    char *name;
    char *value;      /* NULL = 只看有没有 */
    int   op;
} SelAttr;

typedef struct SelPart SelPart;

struct SelPart {
    int    comb;          /* 与**左边**那一段的组合符 */
    char  *tag;           /* NULL = 不限定 */
    char  *id;            /* NULL = 不限定 */
    char **classes;
    int    nclasses;
    int    states;
    int    universal;     /* `*`：显式"任意元素"（与"什么都没写"区分，用于优先级） */
    int    pseudo;        /* SELP_* 位图 */
    int    nth_a, nth_b;  /* `:nth-child(an+b)`；`odd` = 2n+1，`even` = 2n */
    SelAttr *attrs;       /* `[attr]` 系列 */
    int      nattrs;
    SelPart *not_p;       /* `:not(<简单选择器>)`；不支持嵌套 */
    int      pseudo_elem; /* 0 = 无；1 = ::before；2 = ::after */
    int      allow_pseudo; /* 1 = 这条内部规则可以匹配合成节点 */
};

typedef struct {
    SelPart *parts;
    int      nparts;
    int      spec_ids;
    int      spec_cls;
    int      spec_tags;
    int      states;
    Decls    decls;
    int      order;
    /*
     * `@media` 断点下标；-1 = 不在任何媒体块里（永远生效）。
     * 一个元素在某个断点下的样式 = "media == -1 的规则 + 该断点匹配的规则"。
     */
    int      media;
} Rule;

/* 一个 `@media` 条件（视口宽的闭区间；hi == 0xFFFF 表示没有上界） */
typedef struct {
    uint16_t lo;
    uint16_t hi;
} MediaCond;

typedef struct {
    MediaCond *v;
    int        n;
    int        cap;
} MediaConds;

/* 当前正在编译哪个断点：`collect_decls` 用它跳过不该生效的媒体规则。
 * （解析/建表是单线程的，和 `s_error` / `pseudo_counter` 同一套做法。） */
static const uint8_t *s_media_on;

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
    for (int i = 0; i < p->nattrs; i++) {
        ql_mem_free(p->attrs[i].name);
        ql_mem_free(p->attrs[i].value);
    }
    ql_mem_free(p->attrs);
    if (p->not_p != NULL) {
        selpart_free(p->not_p);
        ql_mem_free(p->not_p);
    }
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
/*
 * `:nth-child(...)` 的式子：`N` / `odd` / `even` / `an+b` / `-n+b`。
 * 参数是**1 基**的位置：命中条件 `idx == a*k + b`（k >= 0）。
 */
static int parse_nth(const char *s, size_t len, int *out_a, int *out_b)
{
    char buf[64];
    if (len == 0 || len >= sizeof(buf))
        return 0;
    size_t w = 0;
    int sign = 1;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t')
            continue;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        buf[w++] = c;
    }
    buf[w] = '\0';
    if (strcmp(buf, "odd") == 0) {
        *out_a = 2;
        *out_b = 1;
        return 1;
    }
    if (strcmp(buf, "even") == 0) {
        *out_a = 2;
        *out_b = 0;
        return 1;
    }
    /* 纯数字 */
    const char *p = buf;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    const char *n_start = p;
    while (*p >= '0' && *p <= '9')
        p++;
    const int has_num = (p != n_start);
    int num = 0;
    if (has_num) {
        for (const char *q = n_start; q < p; q++)
            num = num * 10 + (*q - '0');
        num *= sign;
    }
    if (*p == '\0') {
        if (!has_num)
            return 0;
        *out_a = 0;
        *out_b = num;
        return 1;
    }
    if (*p != 'n')
        return 0;
    p++;
    const int a = has_num ? num : 1;
    int b = 0;
    if (*p != '\0') {
        int bsign = 1;
        if (*p == '+') {
            p++;
        } else if (*p == '-') {
            bsign = -1;
            p++;
        } else {
            return 0;
        }
        const char *b_start = p;
        while (*p >= '0' && *p <= '9')
            p++;
        if (p == b_start || *p != '\0')
            return 0;
        for (const char *q = b_start; q < p; q++)
            b = b * 10 + (*q - '0');
        b *= bsign;
    }
    *out_a = a;
    *out_b = b;
    return 1;
}

static int parse_simple_selector(const char *tok, SelPart *p, int *spec_ids,
                                 int *spec_cls, int *spec_tags)
{
    const char *s = tok;
    int anything = 0;

    /* `*`：任意元素。与"什么都没写"不冲突 —— 单独一段时它就是"全匹配" */
    if (*s == '*') {
        s++;
        p->universal = 1;
        anything = 1;
    }

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

    /* 属性选择器：`[a]` `[a=v]` `[a^=v]` `[a$=v]` `[a*=v]` `[a~=v]` */
    while (*s == '[') {
        s++;
        const char *nstart = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == ':')
            s++;
        if (s == nstart)
            return 0;
        char aname[64];
        size_t alen = (size_t)(s - nstart);
        if (alen >= sizeof(aname))
            return 0;
        for (size_t i = 0; i < alen; i++) {
            char c = nstart[i];
            aname[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        aname[alen] = '\0';

        int op = SEL_ATTR_EXISTS;
        char *aval = NULL;
        if (*s == ']') {
            s++;
        } else {
            if (*s == '=') {
                op = SEL_ATTR_EQ;
                s++;
            } else if ((*s == '^' || *s == '$' || *s == '*' || *s == '~') &&
                       s[1] == '=') {
                op = (*s == '^') ? SEL_ATTR_PREFIX
                     : (*s == '$') ? SEL_ATTR_SUFFIX
                     : (*s == '*') ? SEL_ATTR_SUBSTR : SEL_ATTR_WORD;
                s += 2;
            } else {
                set_error("不支持的属性选择器写法：'%s'（支持 [a] [a=v] [a^=v] [a$=v] [a*=v] [a~=v]）", tok);
                return 0;
            }
            /* 值：允许带引号，也允许裸值 */
            char q = 0;
            if (*s == '"' || *s == '\'') {
                q = *s;
                s++;
            }
            const char *vstart = s;
            while (*s != '\0' && (q != 0 ? *s != q : (*s != ']')))
                s++;
            aval = xstrndup(vstart, (size_t)(s - vstart));
            if (aval == NULL)
                return 0;
            if (q != 0 && *s == q)
                s++;
            if (*s != ']') {
                ql_mem_free(aval);
                return 0;
            }
            s++;
        }

        SelAttr *na = (SelAttr *)ql_mem_realloc(p->attrs,
                                               (size_t)(p->nattrs + 1) * sizeof(SelAttr));
        if (na == NULL) {
            ql_mem_free(aval);
            return 0;
        }
        p->attrs = na;
        p->attrs[p->nattrs].name = xstrdup(aname);
        p->attrs[p->nattrs].value = aval;
        p->attrs[p->nattrs].op = op;
        if (p->attrs[p->nattrs].name == NULL)
            return 0;
        p->nattrs++;
        (*spec_cls)++;
        anything = 1;
    }

    /* 伪类 / 伪元素：可重复，顺序任意（`:first-child::before` 两种都行） */
    while (*s == ':') {
        if (s[1] == ':') {
            /* 伪元素 */
            s += 2;
            const char *pstart = s;
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))
                s++;
            char pe[16];
            size_t plen = (size_t)(s - pstart);
            if (plen == 0 || plen >= sizeof(pe)) {
                set_error("伪元素名非法：'%s'", tok);
                return 0;
            }
            for (size_t i = 0; i < plen; i++) {
                char c = pstart[i];
                pe[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            pe[plen] = '\0';
            if (strcmp(pe, "before") == 0)
                p->pseudo_elem = 1;
            else if (strcmp(pe, "after") == 0)
                p->pseudo_elem = 2;
            else {
                set_error("不支持的伪元素 '::%s'（只支持 ::before / ::after）", pe);
                return 0;
            }
            (*spec_tags)++;      /* 规范里伪元素记在"元素"那一档 */
            anything = 1;
            continue;
        }
        s++;
        const char *start = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-')
            s++;
        if (s == start) {
            set_error("伪类名缺失：'%s'", tok);
            return 0;
        }
        char name[32];
        size_t len = (size_t)(s - start);
        if (len >= sizeof(name))
            return 0;
        for (size_t i = 0; i < len; i++) {
            char c = start[i];
            name[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        name[len] = '\0';
        if (strcmp(name, "hover") == 0) {
            p->states |= 1;
        } else if (strcmp(name, "active") == 0) {
            p->states |= 2;
        } else if (strcmp(name, "focus") == 0) {
            p->states |= 4;
        } else if (strcmp(name, "root") == 0) {
            p->pseudo |= SELP_ROOT;
        } else if (strcmp(name, "first-child") == 0) {
            p->pseudo |= SELP_FIRST_CHILD;
        } else if (strcmp(name, "last-child") == 0) {
            p->pseudo |= SELP_LAST_CHILD;
        } else if (strcmp(name, "only-child") == 0) {
            p->pseudo |= SELP_ONLY_CHILD;
        } else if (strcmp(name, "first-of-type") == 0) {
            p->pseudo |= SELP_FIRST_OF_TYPE;
        } else if (strcmp(name, "last-of-type") == 0) {
            p->pseudo |= SELP_LAST_OF_TYPE;
        } else if (strcmp(name, "only-of-type") == 0) {
            p->pseudo |= SELP_ONLY_OF_TYPE;
        } else if (strcmp(name, "nth-child") == 0 || strcmp(name, "nth-last-child") == 0 ||
                   strcmp(name, "nth-of-type") == 0 ||
                   strcmp(name, "nth-last-of-type") == 0) {
            if (*s != '(') {
                set_error("':%s' 必须带括号，例如 :%s(2n+1)", name, name);
                return 0;
            }
            s++;
            const char *astart = s;
            while (*s != '\0' && *s != ')')
                s++;
            if (*s != ')') {
                set_error("':%s(' 缺少右括号", name);
                return 0;
            }
            if (!parse_nth(astart, (size_t)(s - astart), &p->nth_a, &p->nth_b)) {
                set_error("':%s(%.*s)' 里的式子看不懂（支持 N / odd / even / an+b / -n+b）",
                          name, (int)(s - astart), astart);
                return 0;
            }
            s++;
            if (strcmp(name, "nth-child") == 0)
                p->pseudo |= SELP_NTH_CHILD;
            else if (strcmp(name, "nth-last-child") == 0)
                p->pseudo |= SELP_NTH_LAST_CHILD;
            else if (strcmp(name, "nth-of-type") == 0)
                p->pseudo |= SELP_NTH_OF_TYPE;
            else
                p->pseudo |= SELP_NTH_LAST_OF_TYPE;
        } else if (strcmp(name, "not") == 0) {
            if (*s != '(') {
                set_error("':not' 必须带括号，例如 :not(.a)");
                return 0;
            }
            s++;
            const char *nstart2 = s;
            int depth2 = 1;
            while (*s != '\0' && depth2 > 0) {
                if (*s == '(')
                    depth2++;
                else if (*s == ')')
                    depth2--;
                if (depth2 > 0)
                    s++;
            }
            if (depth2 != 0) {
                set_error("':not(' 缺少右括号");
                return 0;
            }
            char inner[128];
            size_t ilen = (size_t)(s - nstart2);
            if (ilen == 0 || ilen >= sizeof(inner)) {
                set_error("':not()' 里要写一个简单选择器");
                return 0;
            }
            memcpy(inner, nstart2, ilen);
            inner[ilen] = '\0';
            s++;                       /* 跳过 ')' */
            if (p->not_p != NULL) {
                set_error("不支持嵌套 ':not()'");
                return 0;
            }
            p->not_p = (SelPart *)ql_mem_calloc(1, sizeof(SelPart));
            if (p->not_p == NULL)
                return 0;
            int sc_i = 0, sc_c = 0, sc_t = 0;
            if (!parse_simple_selector(inner, p->not_p, &sc_i, &sc_c, &sc_t) ||
                p->not_p->pseudo != 0 || p->not_p->pseudo_elem != 0 ||
                p->not_p->universal) {
                set_error("':not()' 只支持 tag / .class / #id / [attr]");
                return 0;
            }
        } else {
            set_error("不支持的伪类 ':%s'（支持 :root :hover :active :focus "
                      ":first-child :last-child :only-child :nth-child() "
                      ":nth-last-child() :first-of-type :last-of-type :only-of-type "
                      ":nth-of-type() :nth-last-of-type() :not()）", name);
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
            if (r->nparts == 0) {
                set_error("选择器不能以组合符开头：'%s'", sel);
                return 0;
            }
            comb = (*p == '>') ? SEL_COMB_CHILD
                   : (*p == '+') ? SEL_COMB_ADJACENT : SEL_COMB_SIBLING;
            p++;
            continue;
        }

        /*
         * 一个简单选择器 token。**括号里的 `+` / `~` 不是组合符**
         * （`:nth-child(2n+1)`、`[a="b+c"]` 里的都得原样留着）。
         */
        const char *start = p;
        int depth = 0;
        while (*p != '\0') {
            if (*p == '(' || *p == '[')
                depth++;
            else if (*p == ')' || *p == ']')
                depth--;
            else if (depth <= 0 && (is_space(*p) || *p == '>' || *p == '+' || *p == '~'))
                break;
            p++;
        }
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

/*
 * `@media` 条件：只认 `(min-width: Npx)` / `(max-width: Npx)`（可用 and 串起来），
 * 前缀可以是 screen / all。`print` 整块跳过（永远不生效）。
 * 别的（orientation / prefers-color-scheme / 分辨率…）明确报错。
 */
static int media_cond_add(MediaConds *mc, uint16_t lo, uint16_t hi)
{
    for (int i = 0; i < mc->n; i++)
        if (mc->v[i].lo == lo && mc->v[i].hi == hi)
            return i;                        /* 同条件复用 */
    if (mc->n >= (int)QL_CFG_UI_MAX_MEDIA) {
        set_error("@media 断点超过上限 %d", (int)QL_CFG_UI_MAX_MEDIA);
        return -1;
    }
    if (mc->n == mc->cap) {
        int cap = (mc->cap > 0) ? mc->cap * 2 : 4;
        MediaCond *nv = (MediaCond *)ql_mem_realloc(mc->v,
                                                   (size_t)cap * sizeof(MediaCond));
        if (nv == NULL)
            return -1;
        mc->v = nv;
        mc->cap = cap;
    }
    mc->v[mc->n].lo = lo;
    mc->v[mc->n].hi = hi;
    return mc->n++;
}

/* 解析 `@media` 的 prelude。返回断点下标；-1 = 错误；-2 = 永不匹配（print） */
static int parse_media_prelude(const char *pre, const char *where, MediaConds *mc)
{
    uint32_t lo = 0, hi = 0xFFFFu;
    const char *p = pre;
    int saw_any = 0;

    while (*p != '\0') {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        if (*p == '(') {
            const char *close = strchr(p, ')');
            if (close == NULL) {
                set_error("%s: @media 条件缺少右括号", where);
                return -1;
            }
            char body[64];
            size_t bl = (size_t)(close - p - 1);
            if (bl >= sizeof(body))
                bl = sizeof(body) - 1u;
            memcpy(body, p + 1, bl);
            body[bl] = '\0';
            char low[64];
            trim_lower(body, low, sizeof(low));

            const char *colon = strchr(low, ':');
            if (colon == NULL) {
                set_error("%s: @media 只支持 (min-width: Npx) / (max-width: Npx)"
                          "（收到 '(%s)'）", where, body);
                return -1;
            }
            char key[32];
            size_t kl = (size_t)(colon - low);
            if (kl >= sizeof(key))
                kl = sizeof(key) - 1u;
            memcpy(key, low, kl);
            key[kl] = '\0';
            while (kl > 0 && is_space(key[kl - 1]))
                key[--kl] = '\0';

            /* 先认属性名，再解析值 —— 否则 `(orientation: landscape)` 会报成"只支持 px" */
            const int is_min = (strcmp(key, "min-width") == 0);
            const int is_max = (strcmp(key, "max-width") == 0);
            if (!is_min && !is_max) {
                set_error("%s: @media 只支持 min-width / max-width（收到 '%s'）", where,
                          key);
                return -1;
            }
            const char *val = skip_space(colon + 1);
            char *endp = NULL;
            const long px = strtol(val, &endp, 10);
            if (endp == NULL || px < 0) {
                set_error("%s: @media 的宽度必须是 px 数字（收到 '%s'）", where, val);
                return -1;
            }
            const char *unit = skip_space(endp);
            if (strncmp(unit, "px", 2) != 0) {
                set_error("%s: @media 的宽度只支持 px（收到 '%s'）", where, val);
                return -1;
            }
            if (is_min) {
                if ((uint32_t)px > lo)
                    lo = (uint32_t)px;
            } else {
                if ((uint32_t)px < hi)
                    hi = (uint32_t)px;
            }
            saw_any = 1;
            p = close + 1;
            continue;
        }
        /* 媒体类型 / and */
        const char *ts = p;
        while (*p != '\0' && !is_space(*p) && *p != '(')
            p++;
        char tok[32];
        size_t tl = (size_t)(p - ts);
        if (tl >= sizeof(tok))
            tl = sizeof(tok) - 1u;
        memcpy(tok, ts, tl);
        tok[tl] = '\0';
        char low[32];
        trim_lower(tok, low, sizeof(low));
        if (strcmp(low, "and") == 0)
            continue;
        if (strcmp(low, "print") == 0)
            return -2;                       /* 打印样式：整块跳过 */
        if (strcmp(low, "screen") == 0 || strcmp(low, "all") == 0)
            continue;
        set_error("%s: 不支持的媒体类型 '%s'（只支持 screen / all；print 整块跳过）",
                  where, low);
        return -1;
    }
    if (!saw_any) {
        set_error("%s: @media 里没有条件（只支持 (min-width: Npx) / (max-width: Npx)）",
                  where);
        return -1;
    }
    if (lo > hi) {
        set_error("%s: @media 的 min-width 大于 max-width，这个条件永远不成立", where);
        return -1;
    }
    return media_cond_add(mc, (uint16_t)lo, (uint16_t)hi);
}

/*
 * 解析一段规则（可以是整份 CSS，也可以是 `@media` 的块体）。
 * `media` = 这些规则所属的断点下标（-1 = 不在媒体块里）。
 */
static int parse_css_block(const char *text, size_t len, Rules *rules, KfDefs *kfs,
                           MediaConds *mc, int media, int *order_io)
{
    size_t pos = 0;
    int order = *order_io;
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
            const char *nm = prelude + 1;
            const char *kw = "keyframes";
            size_t kwlen = strlen(kw);

            /* @media <条件> { <规则…> }：递归解析块体，规则带上断点下标 */
            if (strncmp(nm, "media", 5) == 0 && is_space(nm[5])) {
                const int idx = parse_media_prelude(nm + 5, where, mc);
                if (idx == -1) {
                    ql_mem_free(prelude);
                    ok = 0;
                    break;
                }
                if (idx >= 0) {
                    if (!parse_css_block(body, blen, rules, kfs, mc, idx, &order)) {
                        ql_mem_free(prelude);
                        ok = 0;
                        break;
                    }
                }
                /* idx == -2（print）：整块跳过 */
                ql_mem_free(prelude);
                pos = (size_t)end + 1u;
                continue;
            }
            if (strncmp(nm, kw, kwlen) != 0) {
                set_error("%s: 不支持 at-rule '%s'（支持 @keyframes / @media）", where,
                          prelude);
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
                r->media = media;
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

    *order_io = order;
    return ok;
}

/* 解析整份 CSS：包一层 block 调用 */
static int parse_css(const char *css, Rules *rules, KfDefs *kfs, MediaConds *mc)
{
    char *text = strip_comments(css);
    if (text == NULL)
        return 0;
    int order = 0;
    const int ok = parse_css_block(text, strlen(text), rules, kfs, mc, -1, &order);
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

/* 一条 HTML 属性（`[attr]` 选择器要用；渲染不用） */
typedef struct {
    char *name;    /* 已转小写 */
    char *value;   /* NULL = 布尔属性 */
} PAttr;

struct PNode {
    char  *tag;
    char **classes;
    int    nclasses;
    char  *id;
    PAttr *attrs;            /* 全部属性（`[attr]` 选择器用） */
    int    nattrs;
    /*
     * 兄弟位置：级联期算一次，`:first-child` / `:nth-child` 直接用。
     * 只统计**元素**兄弟 —— `::before` 生成的合成节点不是元素，不参与计数。
     */
    int    sib_idx;
    int    sib_n;
    int    type_idx;         /* 同标签兄弟里的位置 */
    int    type_n;
    int    is_pseudo;        /* 1 = `::before` / `::after` 生成的合成节点 */
    /*
     * 1 = 为了和伪元素节点排队而给宿主文本套的匿名 span（见 expand_pseudo_rec）。
     * 文本本来不是元素，所以它也不参与 `:first-child` / `:nth-child` 计数 ——
     * 这与"文本留在宿主上"时的行为完全一样。
     */
    int    is_anon_text;
    char  *style_attr;
    uint8_t flags;           /* QL_UI_NODE_*（<input type="password">） */
    /* 图像热区（`<map>` / `<img usemap>` / `<area>`）：解析完就地解析成 doc->areas */
    char  *map_name;         /* <map name="m"> */
    char  *usemap;           /* <img usemap="#m"> */
    char  *coords;           /* <area coords="x0 y0 x1 y1"> */
    char  *href;             /* <area href> */
    char  *alt;              /* <area alt> */
    uint8_t area_shape;      /* QLUIAreaShape（<area shape> 的解析结果） */
    PNode *parent;
    PNode *first_child;
    PNode *next_sibling;
    char  *text;             /* 纯文本快路径 */
    Item  *items;            /* 文档顺序的内容（文本段 or 子元素） */
    int    nitems;
    int    capitems;
};

/* 不是"元素"的节点：伪元素合成节点、包裹宿主文本的匿名 span */
static int not_an_element(const PNode *n)
{
    return n->is_pseudo || n->is_anon_text;
}

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
    /* --- IMAGE 包（P3e）：响应式图片与图像热区 --- */
    "picture", "source", "map", "area",
    /* --- EMBED 包：永远只做占位（见 QLUITag 的注释） --- */
    "iframe", "embed", "object", "audio", "video", "track", "canvas",
    "script", "noscript", "slot", "template", "svg", "math",
    /* --- TEXT 包（P4） --- */
    "pre", "sub", "sup",
    /* --- CONTROL 包（P5） --- */
    "textarea",
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
    "sub", "sup",
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
           strcmp(tag, "img") == 0 || strcmp(tag, "source") == 0 ||
           strcmp(tag, "area") == 0 || strcmp(tag, "embed") == 0 ||
           strcmp(tag, "track") == 0;
}

/*
 * 这几个标签的**内容要整段跳过**，连解析都不做。两种原因，处理方式不同：
 *
 *   - `script`：内容是 **raw text**（里面有 `if (a<b)`、`</div>` 这种字符串），
 *     所以只能按"扫到 `</script>` 为止"处理；
 *   - `svg` / `math` / `template`：内容不属于 HTML（前两个是另一门标记语言，
 *     `template` 是惰性内容、永远不渲染）。按标签配平扫过去即可。
 *
 * 为什么不"解析进来再 display:none"：`<svg>` 里的 `<path>` / `<circle>` 会被
 * 当成不认识的标签直接报错，整页都编译不过 —— 那正好是"整页 HTML 贴进来"
 * 最需要避免的情况。
 */
static int tag_skips_content(const char *tag)
{
    return strcmp(tag, "script") == 0 || strcmp(tag, "svg") == 0 ||
           strcmp(tag, "math") == 0 || strcmp(tag, "template") == 0;
}

static int tag_content_is_raw_text(const char *tag)
{
    return strcmp(tag, "script") == 0;
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
    for (int i = 0; i < n->nattrs; i++) {
        ql_mem_free(n->attrs[i].name);
        ql_mem_free(n->attrs[i].value);
    }
    ql_mem_free(n->attrs);
    ql_mem_free(n->style_attr);
    ql_mem_free(n->text);
    ql_mem_free(n->map_name);
    ql_mem_free(n->usemap);
    ql_mem_free(n->coords);
    ql_mem_free(n->href);
    ql_mem_free(n->alt);
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

/* 去掉首尾空白（Unicode 空白也算），返回新的起点；尾部就地截断 */
static char *strip_ws_ptr(char *s)
{
    char *p = s;
    for (;;) {
        size_t wl = u8_ws_len(p);
        if (wl == 0)
            break;
        p += wl;
    }
    size_t len = strlen(p);
    while (len > 0) {
        /* 从尾部回退一个码点再判空白 */
        size_t back = 1;
        while (back < 4 && back < len && ((unsigned char)p[len - back] & 0xC0u) == 0x80u)
            back++;
        if (u8_ws_len(p + len - back) != back)
            break;
        len -= back;
    }
    p[len] = '\0';
    return p;
}

/*
 * `white-space: pre-line`：折叠空白，但 `\n` 保留成换行。
 * 等价于"按行切开，逐行 collapse_ws"。
 */
static char *collapse_ws_keep_nl(const char *chunk)
{
    size_t len = strlen(chunk);
    char *out = (char *)xalloc(len + 1u);
    if (out == NULL)
        return NULL;
    size_t k = 0;
    int prev_ws = 0;      /* 上一字符是折叠出来的空格 */
    for (size_t i = 0; i < len;) {
        if (chunk[i] == '\n') {
            out[k++] = '\n';
            prev_ws = 1;  /* pre-line 的行首空格也要去掉（与浏览器一致） */
            i++;
            continue;
        }
        size_t wl = u8_ws_len(chunk + i);
        if (wl > 0) {
            if (!prev_ws) {
                out[k++] = ' ';
                prev_ws = 1;
            }
            i += wl;
        } else {
            out[k++] = chunk[i];
            prev_ws = 0;
            i++;
        }
    }
    out[k] = '\0';
    return out;
}

/*
 * 保留空白（`pre` / `pre-wrap`）：原文照抄，只把 `\t` 展开成空格。
 *
 * 制表位按**列**算（`tab-size: 8`），列号按码点数而不是像素宽 ——
 * 与浏览器一致（浏览器也是按字符格数，不是按实际度量）。
 * 不做这一步的话 `\t` 会落到字体查不到的字形上，宽度 0，整块代码缩进消失。
 */
static char *preserve_ws_expand_tabs(const char *chunk)
{
    size_t len = strlen(chunk);
    /* 最坏情况：每个字节都变 8 个空格 */
    char *out = (char *)xalloc(len * 8u + 1u);
    if (out == NULL)
        return NULL;
    size_t k = 0;
    int col = 0;
    for (size_t i = 0; i < len;) {
        if (chunk[i] == '\t') {
            int n = 8 - (col % 8);
            for (int j = 0; j < n; j++)
                out[k++] = ' ';
            col += n;
            i++;
            continue;
        }
        if (chunk[i] == '\n') {
            out[k++] = '\n';
            col = 0;
            i++;
            continue;
        }
        /* 一个码点占一列（多字节字符按首字节判断长度，列数不按东亚宽度分） */
        unsigned char c = (unsigned char)chunk[i];
        size_t clen = (c < 0x80u) ? 1u : ((c >= 0xF0u) ? 4u : ((c >= 0xE0u) ? 3u : 2u));
        if (i + clen > len)
            clen = len - i;
        memcpy(out + k, chunk + i, clen);
        k += clen;
        col++;
        i += clen;
    }
    out[k] = '\0';
    return out;
}

static int html_add_text(PNode *n, const char *chunk)
{
    /*
     * **原样保留**，不在这里折叠。
     *
     * `white-space` 是级联之后才知道的，而折叠必须在知道它之后做（`pre*` 不折叠）。
     * 所以原始空白一路留到建表期（见 build 里 text_pool_intern 之前的转换），
     * 代价只是编译期的临时内存 —— 静态产物的字节不受影响。
     */
    const char *src = chunk;
    /*
     * 规范：`<pre>` / `<textarea>` 起始标签后紧接的**一个**换行要丢掉
     * （HTML 的 "ignore the first newline" 规则）。不做这一步，每个代码块
     * 开头都会多一个空行 —— 而 `<pre>\n  x\n</pre>` 是最自然的写法。
     */
    if (src[0] == '\n' && n->nitems == 0 && n->first_child == NULL &&
        n->tag != NULL &&
        (strcmp(n->tag, "pre") == 0 || strcmp(n->tag, "textarea") == 0))
        src++;

    char *raw = xstrdup(src);
    if (raw == NULL)
        return 0;
    if (raw[0] == '\0') {
        ql_mem_free(raw);
        return 1;
    }
    Item it;
    it.is_text = 1;
    it.text = raw;
    it.node = NULL;
    if (!pnode_add_item(n, it)) {
        ql_mem_free(raw);
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
 * 只留渲染真正用得上的那几个：`class` / `id` / `style`（样式与选择器用），
 * `value` / `type` / `max`（表单控件），`src`（`<img>`），
 * `name` / `usemap` / `shape` / `coords` / `href` / `alt`（图像热区）。
 * 其余属性（`data-*` / `aria-*` / `srcset` / `media` …）**静默忽略** ——
 * 它们不影响渲染，为它们报错只会让真实 HTML 片段跑不起来。
 */
typedef struct {
    char *cls;
    char *id;
    char *style;
    char *value;      /* <input value> / <progress value> */
    char *type;       /* <input type="text|password"> */
    char *max;        /* <progress max> / <meter max> */
    char *src;        /* <img src>：只是资源名，字节另找（见图像管线设计） */
    char *map_name;   /* <map name>：热区表的名字 */
    char *usemap;     /* <img usemap="#name">：指向哪个 <map> */
    char *coords;     /* <area coords="x0 y0 …">：图片像素坐标 */
    char *href;       /* <area href>：热区要跳哪去（本库不跳，交给回调） */
    char *alt;        /* <area alt> */
    char *shape;      /* <area shape="rect|circle|poly|default"> */
    uint8_t has_open; /* <details open> / <dialog open>：布尔属性，只需"有没有" */
    /*
     * 全部属性（含上表那些）：`[attr]` / `[attr=v]` 选择器要用。
     * 只在下一次 parse_attrs 前有效 —— 调用方立刻转交给 PNode。
     */
    PAttr *all;
    int    nall;
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
    ql_mem_free(a->map_name);
    ql_mem_free(a->usemap);
    ql_mem_free(a->coords);
    ql_mem_free(a->href);
    ql_mem_free(a->alt);
    ql_mem_free(a->shape);
    for (int i = 0; i < a->nall; i++) {
        ql_mem_free(a->all[i].name);
        ql_mem_free(a->all[i].value);
    }
    ql_mem_free(a->all);
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
/*
 * 把一条属性追加到 `a->all`。`val == NULL` = 布尔属性。
 * 值走和 typed 字段同一套实体解码（`data-tip="a&amp;b"` 要能匹配 `[data-tip="a&b"]`）。
 */
static int attr_all_add(Attrs *a, const char *name, const char *val, size_t vlen)
{
    PAttr *na = (PAttr *)ql_mem_realloc(a->all, (size_t)(a->nall + 1) * sizeof(PAttr));
    if (na == NULL)
        return 0;
    a->all = na;
    PAttr *slot = &a->all[a->nall];
    memset(slot, 0, sizeof(*slot));
    slot->name = xstrdup(name);
    if (slot->name == NULL)
        return 0;
    if (val != NULL) {
        SB sb = { 0 };
        if (!decode_entities(val, vlen, &sb)) {
            sb_free(&sb);
            return 0;
        }
        slot->value = (sb.p != NULL) ? sb.p : xstrdup("");
        if (slot->value == NULL)
            return 0;
    }
    a->nall++;
    return 1;
}

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

        /* 通用收集：给 `[attr]` 选择器用（值与 typed 字段各自 decode 一次，
           属性最多几个，这点重复不值得为了省它把逻辑绕在一起） */
        if (!attr_all_add(a, name, val, vlen))
            return NULL;

        if (val != NULL && (strcmp(name, "class") == 0 || strcmp(name, "id") == 0 ||
                            strcmp(name, "style") == 0 || strcmp(name, "value") == 0 ||
                            strcmp(name, "type") == 0 || strcmp(name, "max") == 0 ||
                            strcmp(name, "src") == 0 || strcmp(name, "name") == 0 ||
                            strcmp(name, "usemap") == 0 || strcmp(name, "shape") == 0 ||
                            strcmp(name, "coords") == 0 || strcmp(name, "href") == 0 ||
                            strcmp(name, "alt") == 0)) {
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
            } else if (strcmp(name, "name") == 0) {
                ql_mem_free(a->map_name);
                a->map_name = s;
            } else if (strcmp(name, "usemap") == 0) {
                ql_mem_free(a->usemap);
                a->usemap = s;
            } else if (strcmp(name, "shape") == 0) {
                ql_mem_free(a->shape);
                a->shape = s;
            } else if (strcmp(name, "coords") == 0) {
                ql_mem_free(a->coords);
                a->coords = s;
            } else if (strcmp(name, "href") == 0) {
                ql_mem_free(a->href);
                a->href = s;
            } else if (strcmp(name, "alt") == 0) {
                ql_mem_free(a->alt);
                a->alt = s;
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
        /*
         * 只 strip、**不折叠**。
         *
         * strip 是**结构性**的：它看的是"这段文字是不是元素内容的开头/结尾"，
         * 与样式无关，所以在解析期做。折叠是样式相关的（`pre*` 不折叠），
         * 推迟到建表期（见下面 text_pool_intern 之前）。
         *
         * 例外：`<pre>` / `<textarea>` 的行首空格是**内容**，不能 strip。
         * （CSS 里给别的标签写 `white-space: pre` 时，行首空格已经在解析期
         *  丢了 —— 想要保留请直接用 `<pre>`，这条写进了文档。）
         */
        if (n->tag != NULL &&
            (strcmp(n->tag, "pre") == 0 || strcmp(n->tag, "textarea") == 0 ||
             strcmp(n->tag, "#pe") == 0)) {
            n->text = sb.p;          /* 接管所有权（`#pe` 的 content 原样保留） */
        } else {
            char *body = strip_ws_ptr(sb.p);
            char *kept = xstrdup(body);
            sb_free(&sb);
            if (kept == NULL)
                return 0;
            n->text = (kept[0] != '\0') ? kept : (ql_mem_free(kept), NULL);
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
    const int preserve_tag =
        (n->tag != NULL && (strcmp(n->tag, "pre") == 0 ||
                            strcmp(n->tag, "textarea") == 0 ||
                            strcmp(n->tag, "#pe") == 0));

    PNode *prev = NULL;
    for (int i = 0; i < n->nitems; i++) {
        if (n->items[i].is_text) {
            char *txt = n->items[i].text;
            char *dup = xstrdup(txt);
            if (dup == NULL)
                return 0;
            /* 首段 lstrip、末段 rstrip（`pre` / `textarea` 里这些空格是内容） */
            char *t = dup;
            if (!preserve_tag) {
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

/* ---------------------- 图像热区（<map> / <area>） ---------------------- */

/*
 * `<area shape="…">` → 枚举。HTML 允许大小写混写（`Rect` / `RECT`），
 * 所以这里做一次小写化比较。缺省是 rect（与规范一致）。
 */
static int area_shape_of(const char *s, uint8_t *out)
{
    if (s == NULL || s[0] == '\0') {
        *out = QL_UI_AREA_RECT;
        return 1;
    }
    char buf[8];
    size_t i = 0;
    for (; s[i] != '\0' && i < sizeof(buf) - 1u; i++) {
        char c = s[i];
        buf[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    if (s[i] != '\0')
        return 0;
    buf[i] = '\0';
    if (strcmp(buf, "rect") == 0)
        *out = QL_UI_AREA_RECT;
    else if (strcmp(buf, "circle") == 0)
        *out = QL_UI_AREA_CIRCLE;
    else if (strcmp(buf, "poly") == 0)
        *out = QL_UI_AREA_POLY;
    else if (strcmp(buf, "default") == 0)
        *out = QL_UI_AREA_DEFAULT;
    else
        return 0;
    return 1;
}

/* 把属性值搬到 PNode 上（空值当作"没有"）。OOM 返回 0。 */
static int pnode_take(char **dst, const char *v)
{
    if (v == NULL || v[0] == '\0')
        return 1;
    *dst = xstrdup(v);
    return *dst != NULL;
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
            /*
             * 空元素（br/img/input/embed/track…）的结束标签是"解析错误"而不是
             * 语法歧义：浏览器的做法是**忽略**它。这里照做 —— 否则
             * `<embed></embed>` 这种看着很正常的写法会让整页编译不过，
             * 而错在哪儿用户根本看不出来。
             */
            if (tag_is_void(name)) {
                p = q + 1;
                continue;
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
                /*
                 * "一定会有人写"的几个标签给出比"不支持的标签"更有用的提示。
                 * 这里只管**当前还没实现**的那批（table 包 / textarea）；
                 * img / input / select 都已支持，别再往这里加。
                 */
                if (strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 ||
                         strcmp(name, "td") == 0 || strcmp(name, "th") == 0)
                    set_error("HTML: 不支持表格 <%s>：用嵌套的 flex 行来做", name);
                else if (strcmp(name, "ruby") == 0 || strcmp(name, "rt") == 0 ||
                         strcmp(name, "rp") == 0)
                    set_error("HTML: 不支持注音 <%s>：用两行文本自己排", name);
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
            /* 属性表整体转交给节点：`[attr]` 选择器要用 */
            if (at.nall > 0) {
                n->attrs = at.all;
                n->nattrs = at.nall;
                at.all = NULL;
                at.nall = 0;
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

            /*
             * 图像热区的三件套属性：`<map name>` / `<img usemap>` / `<area …>`。
             * 这里只把字符串搬到 PNode 上；"usemap ↔ map name" 的配对在解析完
             * 整棵树之后统一做（见 build_areas），因为 `<map>` 可能写在 `<img>` 后面。
             */
            {
                int ok = 1;
                if (strcmp(name, "map") == 0)
                    ok = pnode_take(&n->map_name, at.map_name);
                else if (strcmp(name, "img") == 0)
                    ok = pnode_take(&n->usemap, at.usemap);
                else if (strcmp(name, "area") == 0) {
                    ok = pnode_take(&n->coords, at.coords) &&
                         pnode_take(&n->href, at.href) &&
                         pnode_take(&n->alt, at.alt);
                    if (ok && !area_shape_of(at.shape, &n->area_shape)) {
                        set_error("HTML: <area shape=\"%.16s\"> 不支持"
                                  "（只支持 rect / circle / poly / default）",
                                  (at.shape != NULL) ? at.shape : "");
                        ok = 0;
                    }
                }
                if (!ok) {
                    attrs_free(&at);
                    pnode_free(n);
                    pnode_free(root);
                    return NULL;
                }
            }

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

            /*
             * 自闭合写法 <br/> 与空元素 <br> 等价：都不压栈。
             * `tag_skips_content` 的四个（script/svg/math/template）也不压栈 ——
             * 它们的内容整段跳过，节点本身留在树里（后面按 display:none 或占位盒处理）。
             */
            if (self_closing || tag_is_void(name) || tag_skips_content(name)) {
                if (tag_skips_content(name)) {
                    const char *next = tag_content_is_raw_text(name)
                                           ? skip_raw_text(after, name)
                                           : skip_element(after, name, self_closing);
                    if (next == NULL) {
                        pnode_free(root);
                        return NULL;
                    }
                    after = next;
                }
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

/* nth-child 的命中条件：位置 pos（1 基）能写成 a*k + b（k >= 0） */
static int nth_hits(int a, int b, int pos)
{
    if (a == 0)
        return pos == b;
    const int d = pos - b;
    if (a > 0)
        return d >= 0 && (d % a) == 0;
    /* a < 0：只匹配有限个（`-n+3` = 前三个） */
    return d <= 0 && (d % a) == 0;
}

static const PAttr *find_attr(const PNode *n, const char *name)
{
    for (int i = 0; i < n->nattrs; i++)
        if (strcmp(n->attrs[i].name, name) == 0)
            return &n->attrs[i];
    return NULL;
}

static int attr_matches(const PNode *n, const SelAttr *a)
{
    const PAttr *at = find_attr(n, a->name);
    if (at == NULL)
        return 0;
    if (a->op == SEL_ATTR_EXISTS)
        return 1;
    const char *v = (at->value != NULL) ? at->value : "";
    const char *want = (a->value != NULL) ? a->value : "";
    const size_t vl = strlen(v), wl = strlen(want);
    switch (a->op) {
    case SEL_ATTR_EQ:
        return strcmp(v, want) == 0;
    case SEL_ATTR_PREFIX:
        return wl > 0 && vl >= wl && strncmp(v, want, wl) == 0;
    case SEL_ATTR_SUFFIX:
        return wl > 0 && vl >= wl && strcmp(v + (vl - wl), want) == 0;
    case SEL_ATTR_SUBSTR:
        return wl > 0 && strstr(v, want) != NULL;
    case SEL_ATTR_WORD: {
        /* `~=`：按空白切成词，任一相等 */
        if (wl == 0)
            return 0;
        const char *p = v;
        while (*p != '\0') {
            while (*p != '\0' && (*p == ' ' || *p == '\t'))
                p++;
            const char *w0 = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            if ((size_t)(p - w0) == wl && strncmp(w0, want, wl) == 0)
                return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int simple_matches(const PNode *n, const SelPart *sp)
{
    /* 合成节点（`::before` / `::after`）只由它自己的那条内部规则匹配 */
    if (n->is_pseudo && !sp->allow_pseudo)
        return 0;

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
    for (int i = 0; i < sp->nattrs; i++)
        if (!attr_matches(n, &sp->attrs[i]))
            return 0;
    if (sp->not_p != NULL && simple_matches(n, sp->not_p))
        return 0;

    /*
     * 结构伪类：位置在 flatten 之后算好（1 基）。
     * 匿名文本节点不算元素，也就没有"位置" —— 不挡住的话它会顶着
     * `sib_idx` 的初值 0 冒充 `:first-child`。
     */
    if (sp->pseudo != 0) {
        if (n->is_anon_text)
            return 0;
        /*
         * `:root` = 元素树最外层第一个元素（`#document` 的第一个元素孩子）。
         * 变量通常就写在这里（Bootstrap 的 `:root { --bs-*: … }`），
         * 它继承给整棵树，所以这条必须能匹配上。
         */
        if ((sp->pseudo & SELP_ROOT) != 0) {
            if (n->parent == NULL || n->parent->tag == NULL ||
                strcmp(n->parent->tag, "#document") != 0 || n->sib_idx != 0)
                return 0;
        }
        const int pos = n->sib_idx + 1;
        const int tpos = n->type_idx + 1;
        if ((sp->pseudo & SELP_FIRST_CHILD) && pos != 1)
            return 0;
        if ((sp->pseudo & SELP_LAST_CHILD) && pos != n->sib_n)
            return 0;
        if ((sp->pseudo & SELP_ONLY_CHILD) && n->sib_n != 1)
            return 0;
        if ((sp->pseudo & SELP_FIRST_OF_TYPE) && tpos != 1)
            return 0;
        if ((sp->pseudo & SELP_LAST_OF_TYPE) && tpos != n->type_n)
            return 0;
        if ((sp->pseudo & SELP_ONLY_OF_TYPE) && n->type_n != 1)
            return 0;
        if ((sp->pseudo & SELP_NTH_CHILD) && !nth_hits(sp->nth_a, sp->nth_b, pos))
            return 0;
        if ((sp->pseudo & SELP_NTH_LAST_CHILD) &&
            !nth_hits(sp->nth_a, sp->nth_b, n->sib_n - pos + 1))
            return 0;
        if ((sp->pseudo & SELP_NTH_OF_TYPE) && !nth_hits(sp->nth_a, sp->nth_b, tpos))
            return 0;
        if ((sp->pseudo & SELP_NTH_LAST_OF_TYPE) &&
            !nth_hits(sp->nth_a, sp->nth_b, n->type_n - tpos + 1))
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
    if (comb == SEL_COMB_ADJACENT || comb == SEL_COMB_SIBLING) {
        /* 兄弟组合符：从**前一个元素兄弟**开始往左找（合成节点不算） */
        const PNode *p = n->parent;
        if (p == NULL)
            return 0;
        const PNode *prev = NULL;
        for (const PNode *c = p->first_child; c != NULL && c != n; c = c->next_sibling)
            if (!not_an_element(c))
                prev = c;
        if (comb == SEL_COMB_ADJACENT)
            return prev != NULL && match_rec(prev, r, idx - 1);
        for (const PNode *c = prev; c != NULL; c = prev) {
            if (match_rec(c, r, idx - 1))
                return 1;
            prev = NULL;
            for (const PNode *o = p->first_child; o != NULL && o != c; o = o->next_sibling)
                if (!not_an_element(o))
                    prev = o;
        }
        return 0;
    }

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
    /* `::before` / `::after` 生成的合成节点：行内，由内容撑开 */
    if (strcmp(tag, "#pe") == 0)
        return ua_set_inline(out);
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
        /*
     * `height` 是**边框盒**高度（UA 默认 box-sizing: border-box），
     * 所以按"内容 24 + 上下边框 2"写成 26 —— 不然内容盒只有 22px，
     * 而 line-height 是 24px，文字上下各被切掉一截（截图里"字只显示一半"）。
     */
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "box-sizing", "border-box") &&
               decls_set(out, "height", "26px") &&
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
        /*
     * `height` 是**边框盒**高度（UA 默认 box-sizing: border-box），
     * 所以按"内容 24 + 上下边框 2"写成 26 —— 不然内容盒只有 22px，
     * 而 line-height 是 24px，文字上下各被切掉一截（截图里"字只显示一半"）。
     */
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "box-sizing", "border-box") &&
               decls_set(out, "height", "26px") &&
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
     * <picture>：响应式图片容器。浏览器给的是 display:inline，但这里用
     * inline-block —— 它里面装的永远是行内块的 <img>，两者视觉一致，
     * 而 inline-block 让"收缩到内容宽"由布局自己算出来，不用赌
     * "行内元素里套行内块"的那条少见路径。
     */
    if (strcmp(tag, "picture") == 0)
        return decls_set(out, "display", "inline-block");

    /*
     * ---- EMBED 包：占位盒与"不产生盒子" ----
     *
     * 替换型元素（iframe/object/audio/video/canvas）在浏览器里是"一块由别人
     * 渲染的区域"。本库按红线不渲染外来内容，所以给一个**看得出是占位**的盒子：
     * 1px 实线边框 + 居中的标签名（绘制期画，见 qlui.c 的 draw_embed_placeholder）。
     *
     * 尺寸照浏览器的默认值取（iframe/canvas/video 300×150、audio 矮条），
     * 这样"忘了写宽高"时用户一眼能看出这里有个被漏掉的东西；想改就写 CSS。
     */
    if (strcmp(tag, "iframe") == 0 || strcmp(tag, "object") == 0 ||
        strcmp(tag, "canvas") == 0 || strcmp(tag, "video") == 0 ||
        strcmp(tag, "embed") == 0) {
        return decls_set(out, "display", "block") && decls_set(out, "width", "300px") &&
               decls_set(out, "height", "150px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-color", "#8A8A8A") &&
               decls_set(out, "color", "#8A8A8A") &&
               decls_set(out, "text-align", "center");
    }
    if (strcmp(tag, "audio") == 0) {
        return decls_set(out, "display", "block") && decls_set(out, "width", "300px") &&
               decls_set(out, "height", "32px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-color", "#8A8A8A") &&
               decls_set(out, "color", "#8A8A8A") &&
               decls_set(out, "text-align", "center");
    }
    /* <svg> 与 <math> 在浏览器里是行内元素，所以这里也是 inline-block */
    if (strcmp(tag, "svg") == 0 || strcmp(tag, "math") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "width", "120px") && decls_set(out, "height", "60px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-color", "#8A8A8A") &&
               decls_set(out, "color", "#8A8A8A") &&
               decls_set(out, "text-align", "center");
    }
    /*
     * 其余五个**不产生盒子**：
     *   script    —— 没有 JS；内容已在解析期整段跳过
     *   template  —— 惰性内容，永远不渲染（内容也跳过了）
     *   noscript  —— 没有 JS，所以脚本"总是不可用"；但这里仍然隐藏：
     *                这类内容在网页上通常是"请开启 JavaScript"式的降级提示，
     *                在 QLGL 里显示出来只会变成噪音。要显示就自己写
     *                `noscript { display: block }`。
     *   slot      —— 没有 shadow DOM
     *   track     —— 字幕轨元数据
     */
    if (strcmp(tag, "script") == 0 || strcmp(tag, "template") == 0 ||
        strcmp(tag, "noscript") == 0 || strcmp(tag, "slot") == 0 ||
        strcmp(tag, "track") == 0) {
        return decls_set(out, "display", "none");
    }

    /*
     * <source> 与 <map>/<area>：**不产生盒子**。
     * <source> 的候选选择（media / type / srcset）没做，<area> 的几何另存进
     * doc->areas（见 热区 API），两者都不该参与布局。
     */
    if (strcmp(tag, "source") == 0 || strcmp(tag, "map") == 0 ||
        strcmp(tag, "area") == 0) {
        return decls_set(out, "display", "none");
    }

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

    /*
     * <pre>：代码块。
     *   - display:block + 上下 margin（与 <p> 一致，块级之间才有视觉间隔）
     *   - white-space: pre —— 空白与换行**全部保留**、不自动折行
     * 等宽字体在 P4c 补（Qt/浏览器的 `font-family: monospace` 需要一个字体族维度）。
     * 不写 `overflow:auto`：本库的滚动要显式开，默认让长行横向撑出父盒更符合"看得见"。
     */
    if (strcmp(tag, "pre") == 0) {
        return decls_set(out, "display", "block") &&
               decls_set(out, "white-space", "pre") &&
               decls_set(out, "font-family", "monospace") &&
               decls_set(out, "margin", "8px 0");
    }

    /*
     * <sub> / <sup>：基线偏移 + 字号缩小。
     * 浏览器用的是 `vertical-align: sub/super` + `font-size: smaller`，
     * 这里两者都真的支持，所以继承来的字号会被正确地乘上 5/6。
     */
    if (strcmp(tag, "sub") == 0)
        return ua_set_inline(out) &&
               decls_set(out, "vertical-align", "sub") &&
               decls_set(out, "font-size", "smaller");
    if (strcmp(tag, "sup") == 0)
        return ua_set_inline(out) &&
               decls_set(out, "vertical-align", "super") &&
               decls_set(out, "font-size", "smaller");

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

    /*
     * <textarea>：多行输入框。
     *   - 默认尺寸按浏览器量级（cols≈20 字符、rows≈3 行 → 200×60）
     *   - `white-space: pre-wrap`：保留换行、按宽度折行（这就是多行编辑的渲染基础）
     *   - 行首空白也要保留（textarea 的内容就是纯文本）
     * `rows` / `cols` **属性不解析**：HTML 属性不能直接进样式，改尺寸请用 CSS。
     */
    if (strcmp(tag, "textarea") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "box-sizing", "border-box") &&
               decls_set(out, "width", "200px") &&
               decls_set(out, "height", "60px") &&
               decls_set(out, "white-space", "pre-wrap") &&
               decls_set(out, "font-family", "monospace") &&
               decls_set(out, "padding", "4px") &&
               decls_set(out, "border-width", "1px") &&
               decls_set(out, "border-radius", "4px") &&
               decls_set(out, "border-color", "#8A8A8A") &&
               decls_set(out, "background-color", "#FFFFFF");
    }

    /* 代码类标签用等宽（与浏览器一致；内建位图字体本来就是等宽的） */
    if (strcmp(tag, "code") == 0 || strcmp(tag, "kbd") == 0 ||
        strcmp(tag, "samp") == 0)
        return ua_set_inline(out) && decls_set(out, "font-family", "monospace");

    /* 剩下这些纯行内标签 UA 默认只有 display:inline（与 tag_inline_by_default 同表） */
    if (tag_inline_by_default(tag))
        return ua_set_inline(out);

    /* ---- 按钮：一行能看见的边框 + 内边距（用 line-height 撑不出来的高度） ---- */
    if (strcmp(tag, "button") == 0) {
        return decls_set(out, "display", "inline-block") &&
               decls_set(out, "box-sizing", "border-box") &&
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

/* ---------------------- ::before / ::after 展开 ---------------------- */

/*
 * `::before` / `::after` 在**构建期**变成真节点，一次到位：
 *   - 内容是 `content` 的字符串（只支持纯文本）；
 *   - 样式由一条内部规则 `.__pe_N` 提供（声明从原规则拷贝，去掉 content）；
 *   - 节点 tag 是 `#pe`：任何用户选择器都匹配不到它（标签名不可能以 # 开头），
 *     只有带 allow_pseudo 的内部规则能命中。
 * 这样运行期完全不知道有"伪元素"这回事 —— 零成本，与标签包裁剪也无关。
 */
static int pseudo_counter = 0;

/* `content: "x"` / `content: 'x'` → 解出来的文本（调用方负责 free） */
static char *pseudo_content_text(const Rule *r)
{
    const char *v = decls_get(&r->decls, "content");
    if (v == NULL)
        return NULL;
    if (strstr(v, "var(") != NULL) {
        set_error("content 不支持 var()（伪元素在级联之前展开，那时还没有自定义属性）");
        return NULL;
    }
    const char *p = skip_space(v);
    if (*p != '"' && *p != '\'')
        return NULL;                   /* `content: none` 之类：当作不生成 */
    const char q = *p++;
    const char *start = p;
    while (*p != '\0' && *p != q)
        p++;
    if (*p != q)
        return NULL;
    return xstrndup(start, (size_t)(p - start));
}

static int expand_pseudo_rec(PNode *n, const Rule *r, Rules *rs)
{
    for (PNode *c = n->first_child; c != NULL; c = c->next_sibling)
        if (!expand_pseudo_rec(c, r, rs))
            return 0;

    /* 输入框/下拉框是叶子：往里塞节点没有意义（也会破坏值池） */
    if (strcmp(n->tag, "input") == 0 || strcmp(n->tag, "select") == 0 ||
        strcmp(n->tag, "textarea") == 0 || strcmp(n->tag, "#pe") == 0)
        return 1;

    if (!rule_matches(n, r))
        return 1;

    char *text = pseudo_content_text(r);
    if (text == NULL)
        return 1;                      /* 没有 content：不生成（与浏览器一致） */

    char cls[32];
    snprintf(cls, sizeof(cls), "__pe_%d", pseudo_counter++);

    PNode *pe = pnode_new("#pe", NULL);
    if (pe == NULL) {
        ql_mem_free(text);
        return 0;
    }
    pe->is_pseudo = 1;
    pe->text = text;
    /* 给合成节点挂 class（直接建，不用 selpart 那套） */
    pe->classes = (char **)ql_mem_calloc(1, sizeof(char *));
    if (pe->classes == NULL) {
        pnode_free(pe);
        return 0;
    }
    pe->classes[0] = xstrdup(cls);
    if (pe->classes[0] == NULL) {
        pnode_free(pe);
        return 0;
    }
    pe->nclasses = 1;

    /*
     * 下面会给规则表追加一条内部规则 —— `realloc` 可能把 `rs->r` 整体搬走，
     * 传进来的 `r` 就成了野指针。所以先把这一趟要用到的字段抄下来。
     */
    const int is_before = (r->parts[r->nparts - 1].pseudo_elem == 1);
    const int spec_cls = r->spec_cls;
    const int spec_tags = r->spec_tags;
    const int spec_ids = r->spec_ids;
    const int order = r->order;
    const int rule_media = r->media;      /* 伪元素规则跟着源规则一起受断点约束 */
    Decl *const src_decls = r->decls.d;   /* 声明数组本身不随规则表搬家 */
    const int src_n = r->decls.n;

    /* 内部规则：`.__pe_N` + 原规则的全部声明（去掉 content） */
    if (rs->n >= rs->cap) {
        int ncap = (rs->cap == 0) ? 8 : rs->cap * 2;
        Rule *nr = (Rule *)ql_mem_realloc(rs->r, (size_t)ncap * sizeof(Rule));
        if (nr == NULL) {
            pnode_free(pe);
            return 0;
        }
        rs->r = nr;
        rs->cap = ncap;
    }
    Rule *nr = &rs->r[rs->n];
    memset(nr, 0, sizeof(*nr));
    nr->parts = (SelPart *)ql_mem_calloc(1, sizeof(SelPart));
    if (nr->parts == NULL) {
        pnode_free(pe);
        return 0;
    }
    nr->nparts = 1;
    nr->parts[0].allow_pseudo = 1;
    nr->parts[0].classes = (char **)ql_mem_calloc(1, sizeof(char *));
    if (nr->parts[0].classes == NULL) {
        ql_mem_free(nr->parts);
        pnode_free(pe);
        return 0;
    }
    nr->parts[0].classes[0] = xstrdup(cls);
    if (nr->parts[0].classes[0] == NULL) {
        pnode_free(pe);
        return 0;
    }
    nr->parts[0].nclasses = 1;
    nr->spec_cls = spec_cls + 1;      /* 伪元素在规范里"比元素更强" */
    nr->spec_tags = spec_tags;
    nr->spec_ids = spec_ids;
    nr->order = order;
    nr->media = rule_media;
    for (int i = 0; i < src_n; i++) {
        if (strcmp(src_decls[i].key, "content") == 0)
            continue;
        if (!decls_set(&nr->decls, src_decls[i].key, src_decls[i].val)) {
            pnode_free(pe);
            return 0;
        }
    }
    rs->n++;

    /*
     * 宿主自己还有文本（`<div>正文</div>` 上加 `::before`）：把文本挪进匿名 span。
     *
     * `flatten_node` 只在"HTML 里本来就写了子元素"时才拆混排，而伪元素是这里才
     * 造出来的。不拆的话文本留在宿主身上、伪元素节点又落在内容盒原点上，两者会
     * 叠在同一个位置（圆点被正文盖掉），`::before` 也失去了"在正文之前"的含义。
     * 拆出来走的是与混排完全相同的那条路，布局与绘制不需要任何新分支。
     */
    PNode *text_span = NULL;
    if (n->text != NULL) {
        text_span = pnode_new("span", n);
        if (text_span == NULL) {
            pnode_free(pe);
            return 0;
        }
        text_span->is_anon_text = 1;
        text_span->text = n->text;      /* 接管所有权 */
        n->text = NULL;
    }

    /* 拼进兄弟链表：::before 在最前（后面紧跟正文），::after 在最后（正文在它前面） */
    pe->parent = n;
    if (is_before) {
        PNode *rest = n->first_child;
        pe->next_sibling = (text_span != NULL) ? text_span : rest;
        if (text_span != NULL)
            text_span->next_sibling = rest;
        n->first_child = pe;
    } else {
        PNode *tail = n->first_child;
        if (tail == NULL) {
            n->first_child = (text_span != NULL) ? text_span : pe;
        } else {
            while (tail->next_sibling != NULL)
                tail = tail->next_sibling;
            tail->next_sibling = (text_span != NULL) ? text_span : pe;
        }
        if (text_span != NULL)
            text_span->next_sibling = pe;
    }
    return 1;
}

/* 对每条带伪元素的规则展开一次（规则条数很少，不优化） */
static int expand_pseudo(PNode *root, Rules *rs)
{
    const int base = rs->n;      /* 只遍历用户规则；新加的内部规则不再展开 */
    for (int i = 0; i < base; i++) {
        Rule *r = &rs->r[i];
        if (r->nparts <= 0)
            continue;
        if (r->parts[r->nparts - 1].pseudo_elem == 0) {
            /* `content` 只在伪元素上合法 —— 元素上用它是常见的误写，必须报错 */
            if (decls_get(&r->decls, "content") != NULL) {
                set_error("'content' 只能用在 ::before / ::after 上");
                return 0;
            }
            continue;
        }
        const char *cval = decls_get(&r->decls, "content");
        if (cval == NULL) {
            set_error("%s 规则缺少 'content'：::before / ::after 只有写了 content 才生成内容",
                      r->parts[r->nparts - 1].pseudo_elem == 1 ? "::before" : "::after");
            return 0;
        }
        /* 伪元素在级联**之前**展开，那时还没有自定义属性 —— 明确报错，别留成字面量 */
        if (strstr(cval, "var(") != NULL) {
            set_error("content 不支持 var()（伪元素在级联之前展开，那时还没有自定义属性）");
            return 0;
        }
        if (!expand_pseudo_rec(root, r, rs))
            return 0;
    }
    return 1;
}

/*
 * 算每个元素的兄弟位置（`:first-child` / `:nth-child` 用）。
 * 只在**元素**之间计数：`::before` 生成的合成节点不是元素，不参与。
 * 必须在 flatten 之后调（那时 first_child / next_sibling 才连成链表）。
 */
static void assign_sibling_pos(PNode *n)
{
    int idx = 0, total = 0;
    for (PNode *c = n->first_child; c != NULL; c = c->next_sibling)
        if (!not_an_element(c))
            total++;
    for (PNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        if (not_an_element(c))
            continue;
        c->sib_idx = idx++;
        c->sib_n = total;
        int tidx = 0, ttotal = 0;
        for (PNode *o = n->first_child; o != NULL; o = o->next_sibling) {
            if (not_an_element(o) || strcmp(o->tag, c->tag) != 0)
                continue;
            if (o == c)
                tidx = ttotal;
            ttotal++;
        }
        c->type_idx = tidx;
        c->type_n = ttotal;
        assign_sibling_pos(c);
    }
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
        /* `@media` 里的规则只在该断点区间生效（`s_media_on` 是当前区间的位置图） */
        if (r->media >= 0 &&
            (s_media_on == NULL || s_media_on[r->media] == 0))
            continue;
        /* `::before` / `::after` 的规则只作用于合成节点，不能落到真元素上 */
        if (r->nparts > 0 && r->parts[r->nparts - 1].pseudo_elem != 0)
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
    /*
     * 两趟：先普通声明、再 `!important` 声明。
     * 这样"important 永远赢"就成立了，不必给每条声明排优先级。
     */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nm; i++) {
            const Decls *d = &refs[i].r->decls;
            for (int j = 0; j < d->n; j++) {
                if ((d->d[j].important != 0) != (pass == 1))
                    continue;
                if (!decls_set(out, d->d[j].key, d->d[j].val)) {
                    ql_mem_free(refs);
                    return 0;
                }
            }
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
/* 从 "16px" / "16" 里取整数；取不到返回 def */
static int px_of_decl(const char *v, int def)
{
    if (v == NULL)
        return def;
    int n = atoi(v);
    return (n > 0) ? n : def;
}

/* 浏览器 `font-size: smaller/larger` 的比例：5/6 ≈ 0.833，1.2。窄到整数 px */
static int font_size_nudge(int parent_px, int larger)
{
    int px = larger ? (parent_px * 6) / 5 : (parent_px * 5 + 3) / 6;
    return (px > 0) ? px : 1;
}

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

    /*
     * `font-size: smaller / larger` 是**相对父级**的，必须在合并之后算 ——
     * 到 `compile_style` 那一步父级的 px 已经不在手上了。
     * 根没有继承时按 16px 起算，与 `font-size` 的默认值一致。
     */
    const char *fs = decls_get(out, "font-size");
    if (fs != NULL) {
        char fv[32];
        trim_lower(fs, fv, sizeof(fv));
        if (strcmp(fv, "smaller") == 0 || strcmp(fv, "larger") == 0) {
            int parent_px = px_of_decl(decls_get(inherited, "font-size"), 16);
            int px = font_size_nudge(parent_px, strcmp(fv, "larger") == 0);
            char buf[16];
            snprintf(buf, sizeof(buf), "%dpx", px);
            if (!decls_set(out, "font-size", buf))
                return 0;
        }
    }

    /*
     * CSS 变量：到这一步 out 里已经是"这个元素最终可见的声明"
     * （默认 UA 样式 + 级联 + 行内 + 继承）。在这里把 `var()` 换成字面量，
     * 下游（`compile_style`）就完全不需要知道变量存在。
     */
    {
        const char *tag = (n->tag != NULL) ? n->tag : "?";
        char where[64];
        snprintf(where, sizeof(where), "<%s>", tag);
        if (!resolve_custom_props(out, where))
            return 0;
    }
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
    if (strcmp(s, "sticky") == 0)
        return QL_UI_POSITION_STICKY;
    return -1;
}

static int align_content_value(const char *s)
{
    if (strcmp(s, "stretch") == 0 || strcmp(s, "normal") == 0)
        return QL_UI_ALIGN_CONTENT_STRETCH;
    if (strcmp(s, "flex-start") == 0 || strcmp(s, "start") == 0)
        return QL_UI_ALIGN_CONTENT_START;
    if (strcmp(s, "center") == 0)
        return QL_UI_ALIGN_CONTENT_CENTER;
    if (strcmp(s, "flex-end") == 0 || strcmp(s, "end") == 0)
        return QL_UI_ALIGN_CONTENT_END;
    if (strcmp(s, "space-between") == 0)
        return QL_UI_ALIGN_CONTENT_SPACE_BETWEEN;
    if (strcmp(s, "space-around") == 0)
        return QL_UI_ALIGN_CONTENT_SPACE_AROUND;
    if (strcmp(s, "space-evenly") == 0)
        return QL_UI_ALIGN_CONTENT_SPACE_EVENLY;
    return -1;
}

/* 指针形状：认 CSS 里最常用的那批，其余报错（不猜） */
static int cursor_value(const char *s)
{
    if (strcmp(s, "auto") == 0)
        return QL_UI_CURSOR_AUTO;
    if (strcmp(s, "default") == 0)
        return QL_UI_CURSOR_DEFAULT;
    if (strcmp(s, "pointer") == 0)
        return QL_UI_CURSOR_POINTER;
    if (strcmp(s, "text") == 0 || strcmp(s, "vertical-text") == 0)
        return QL_UI_CURSOR_TEXT;
    if (strcmp(s, "move") == 0 || strcmp(s, "all-scroll") == 0)
        return QL_UI_CURSOR_MOVE;
    if (strcmp(s, "not-allowed") == 0 || strcmp(s, "no-drop") == 0)
        return QL_UI_CURSOR_NOT_ALLOWED;
    if (strcmp(s, "help") == 0)
        return QL_UI_CURSOR_HELP;
    if (strcmp(s, "progress") == 0 || strcmp(s, "wait") == 0)
        return QL_UI_CURSOR_PROGRESS;
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
            k->sc_x = QL_UI_KF_UNSET;
            k->sc_y = QL_UI_KF_UNSET;
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
                if (!parse_length(decls_get(d, "border-radius"), where, 1, NULL, &unit, &v)) {
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
            k->sc_x = QL_UI_KF_UNSET;
            k->sc_y = QL_UI_KF_UNSET;
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

/* `border-style` / `outline-style` 的关键字（solid / dashed / dotted / none） */
static int parse_border_style_kw(const char *text, const char *where, uint8_t *out)
{
    char t[32];
    trim_lower(text, t, sizeof(t));
    if (strcmp(t, "solid") == 0)
        *out = QL_UI_BORDER_SOLID;
    else if (strcmp(t, "dashed") == 0)
        *out = QL_UI_BORDER_DASHED;
    else if (strcmp(t, "dotted") == 0)
        *out = QL_UI_BORDER_DOTTED;
    else if (strcmp(t, "none") == 0)
        *out = QL_UI_BORDER_NONE;
    else {
        set_error("%s: 不支持的样式 '%s'（solid / dashed / dotted / none）", where, text);
        return 0;
    }
    return 1;
}

/* 把一份声明集合编译成 QLUIStyle。检查顺序与 Python 侧一致（第一个错就停） */
static int compile_style(const Decls *st, const char *where, const KfRanges *ranges,
                         const KfTable *kftab, AnimTable *anims, IntList *fonts,
                         CalcPool *calc, QLUIStyle *out)
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
    if ((v = decls_get(st, "width")) != NULL && !parse_length(v, where, 1, calc, &wu, &wv))
        return 0;
    if ((v = decls_get(st, "height")) != NULL && !parse_length(v, where, 1, calc, &hu, &hv))
        return 0;

    /* min/max：`auto` = 没设；只接受 px / %（与 width/height 同一套） */
    uint8_t mnwu = QL_UI_UNIT_AUTO, mxwu = QL_UI_UNIT_AUTO;
    uint8_t mnhu = QL_UI_UNIT_AUTO, mxhu = QL_UI_UNIT_AUTO;
    int mnwv = 0, mxwv = 0, mnhv = 0, mxhv = 0;
    /*
     * 文本细节：`letter-spacing` / `text-indent` 是长度（em/rem 在末尾折成 px，
     * 这里只存数值与"是否 em/rem"，与 margin/padding 同一套单位位）。
     * `%` 与 `vw/vh` 对这两个属性没有意义，直接报错。
     */
    int ls_v = 0, ti_v = 0;
    uint8_t ls_unit = 0, ti_unit = 0;
    if ((v = decls_get(st, "letter-spacing")) != NULL) {
        uint8_t lu = 0;
        int lval = 0;
        if (!parse_length(v, where, 0, calc, &lu, &lval))
            return 0;
        if (!length_wants_const(lu, where, "letter-spacing"))
            return 0;
        if (lu == QL_UI_UNIT_PERCENT || lu == QL_UI_UNIT_VW ||
            lu == QL_UI_UNIT_VH || lu == QL_UI_UNIT_VMIN ||
            lu == QL_UI_UNIT_VMAX) {
            set_error("%s: letter-spacing 只接受 px / em / rem（收到 '%s'）", where, v);
            return 0;
        }
        ls_v = lval;
        ls_unit = (lu == QL_UI_UNIT_EM) ? 1 : (lu == QL_UI_UNIT_REM) ? 2 : 0;
    }
    if ((v = decls_get(st, "text-indent")) != NULL) {
        uint8_t lu = 0;
        int lval = 0;
        if (!parse_length(v, where, 0, calc, &lu, &lval))
            return 0;
        if (!length_wants_const(lu, where, "letter-spacing"))
            return 0;
        if (lu == QL_UI_UNIT_PERCENT || lu == QL_UI_UNIT_VW ||
            lu == QL_UI_UNIT_VH || lu == QL_UI_UNIT_VMIN ||
            lu == QL_UI_UNIT_VMAX) {
            set_error("%s: text-indent 只接受 px / em / rem（收到 '%s'）", where, v);
            return 0;
        }
        ti_v = lval;
        ti_unit = (lu == QL_UI_UNIT_EM) ? 1 : (lu == QL_UI_UNIT_REM) ? 2 : 0;
    }

    uint8_t text_transform = QL_UI_TEXT_NONE;
    if ((v = decls_get(st, "text-transform")) != NULL) {
        char t[32];
        trim_lower(v, t, sizeof(t));
        if (strcmp(t, "none") == 0)
            text_transform = QL_UI_TEXT_NONE;
        else if (strcmp(t, "uppercase") == 0)
            text_transform = QL_UI_TEXT_UPPERCASE;
        else if (strcmp(t, "lowercase") == 0)
            text_transform = QL_UI_TEXT_LOWERCASE;
        else if (strcmp(t, "capitalize") == 0)
            text_transform = QL_UI_TEXT_CAPITALIZE;
        else {
            set_error("%s: 只支持 text-transform: none / uppercase / lowercase / "
                      "capitalize（收到 '%s'）", where, v);
            return 0;
        }
    }

    uint8_t text_overflow = QL_UI_OVERFLOW_CLIP;
    if ((v = decls_get(st, "text-overflow")) != NULL) {
        char t[32];
        trim_lower(v, t, sizeof(t));
        if (strcmp(t, "clip") == 0)
            text_overflow = QL_UI_OVERFLOW_CLIP;
        else if (strcmp(t, "ellipsis") == 0)
            text_overflow = QL_UI_OVERFLOW_ELLIPSIS;
        else {
            set_error("%s: 只支持 text-overflow: clip / ellipsis（收到 '%s'）", where, v);
            return 0;
        }
    }

    uint8_t word_break = QL_UI_WORDBREAK_NORMAL;
    if ((v = decls_get(st, "word-break")) != NULL) {
        char t[32];
        trim_lower(v, t, sizeof(t));
        if (strcmp(t, "normal") == 0 || strcmp(t, "keep-all") == 0)
            word_break = QL_UI_WORDBREAK_NORMAL;
        else if (strcmp(t, "break-all") == 0 || strcmp(t, "break-word") == 0)
            word_break = QL_UI_WORDBREAK_ALL;
        else {
            set_error("%s: 只支持 word-break: normal / keep-all / break-all（收到 '%s'）",
                      where, v);
            return 0;
        }
    }
    if ((v = decls_get(st, "overflow-wrap")) != NULL) {
        char t[32];
        trim_lower(v, t, sizeof(t));
        if (strcmp(t, "normal") == 0)
            ;                                    /* 保持 word-break 的结论 */
        else if (strcmp(t, "break-word") == 0 || strcmp(t, "anywhere") == 0)
            word_break = QL_UI_WORDBREAK_ALL;
        else {
            set_error("%s: 只支持 overflow-wrap: normal / break-word / anywhere"
                      "（收到 '%s'）", where, v);
            return 0;
        }
    }

    /* `aspect-ratio: 16 / 9`（也接受 `16/9` 与单个数字 `1.5`） */
    uint16_t aspect_w = 0, aspect_h = 0;
    if ((v = decls_get(st, "aspect-ratio")) != NULL) {
        char t[64];
        trim_lower(v, t, sizeof(t));
        if (strcmp(t, "auto") != 0) {
            double a = 0, b = 1;
            char *slash = strchr(t, '/');
            int ok = 0;
            if (slash != NULL) {
                *slash = '\0';
                const char *e1 = NULL, *e2 = NULL;
                double x = 0, y = 0;
                if (match_number(skip_space(t), &e1, &x) &&
                    *skip_space(e1) == '\0' &&
                    match_number(skip_space(slash + 1), &e2, &y) &&
                    *skip_space(e2) == '\0' && x > 0 && y > 0) {
                    a = x;
                    b = y;
                    ok = 1;
                }
            } else {
                const char *e1 = NULL;
                double x = 0;
                if (match_number(skip_space(t), &e1, &x) && *skip_space(e1) == '\0' &&
                    x > 0) {
                    /* `aspect-ratio: 1.5` = 1.5 / 1 */
                    a = x;
                    b = 1;
                    ok = 1;
                }
            }
            if (!ok) {
                set_error("%s: aspect-ratio 必须是 `宽 / 高` 或正数（收到 '%s'）",
                          where, v);
                return 0;
            }
            /* 存千分比：16/9 → 1778/1000 太糊；直接存整数比，四舍五入到 1/100 */
            if (a < 100.0 && b < 100.0) {
                aspect_w = (uint16_t)py_round(a * 100.0);
                aspect_h = (uint16_t)py_round(b * 100.0);
            } else {
                aspect_w = (uint16_t)py_round(a);
                aspect_h = (uint16_t)py_round(b);
            }
        }
    }

    /* `opacity`：0..1 的数字，存成 0..255。默认 255（不透明） */
    int opacity = 255;
    if ((v = decls_get(st, "opacity")) != NULL) {
        double ov = 0;
        const char *end = NULL;
        if (!match_number(skip_space(v), &end, &ov) || *skip_space(end) != '\0') {
            set_error("%s: opacity 必须是 0~1 的数字（收到 '%s'）", where, v);
            return 0;
        }
        if (ov < 0.0 || ov > 1.0) {
            set_error("%s: opacity 超出 0~1（收到 '%s'）", where, v);
            return 0;
        }
        opacity = (int)py_round(ov * 255.0);
    }
    if ((v = decls_get(st, "min-width")) != NULL) {
        if (!parse_length(v, where, 1, calc, &mnwu, &mnwv))
            return 0;
        if (mnwu == QL_UI_UNIT_AUTO)
            mnwu = QL_UI_UNIT_AUTO;
    }
    if ((v = decls_get(st, "max-width")) != NULL &&
        !parse_length(v, where, 1, calc, &mxwu, &mxwv))
        return 0;
    if ((v = decls_get(st, "min-height")) != NULL &&
        !parse_length(v, where, 1, calc, &mnhu, &mnhv))
        return 0;
    if ((v = decls_get(st, "max-height")) != NULL &&
        !parse_length(v, where, 1, calc, &mxhu, &mxhv))
        return 0;
    if (mnwu == QL_UI_UNIT_PX && mxwu == QL_UI_UNIT_PX && mxwv < mnwv) {
        set_error("%s: max-width(%dpx) 小于 min-width(%dpx)", where, mxwv, mnwv);
        return 0;
    }

    QLUILength margin_e[4], padding_e[4];
    for (int i = 0; i < 4; i++) {
        margin_e[i].value = 0;
        margin_e[i].unit = QL_UI_UNIT_PX;
        margin_e[i].reserved = 0;
        padding_e[i] = margin_e[i];
    }
    if ((v = decls_get(st, "margin")) != NULL &&
        !parse_edges(v, where, 1, calc, margin_e))
        return 0;
    if ((v = decls_get(st, "padding")) != NULL &&
        !parse_edges(v, where, 0, calc, padding_e))
        return 0;
    {
        static const char *const sides[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) {
            char key[32];
            snprintf(key, sizeof(key), "margin-%s", sides[i]);
            if ((v = decls_get(st, key)) != NULL) {
                /* css-key* 单独给一条边：借 parse_edges 的语法，只取第 0 条 */
                QLUILength one[4];
                if (!parse_edges(v, where, 1, calc, one))
                    return 0;
                margin_e[i] = one[0];
            }
            snprintf(key, sizeof(key), "padding-%s", sides[i]);
            if ((v = decls_get(st, key)) != NULL) {
                QLUILength one[4];
                if (!parse_edges(v, where, 0, calc, one))
                    return 0;
                padding_e[i] = one[0];
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
                          1, calc, &unit, &num))
            return 0;
        if (!length_wants_const(unit, where, "font-size"))
            return 0;
        font_px = num;
    }
    int font_id = intlist_add(fonts, font_px);
    if (font_id < 0)
        return 0;

    /*
     * `em` / `rem` 在这里就折成 px —— 此刻 `font_px` 已经是这个元素的**最终字号**
     * （`font-size` 的继承值也在 decls 里），`rem` 用根元素字号。
     * 折完就只剩 px / % / vw / vh，布局期不必再关心字号。
     */

    /*
     * `vertical-align`：只支持 baseline / sub / super 三个值
     * （middle/top/bottom 要按行盒对齐，属于行内布局，本库还没有行内格式化上下文）。
     * 偏移量按**字号**算，所以继承不同字号时比例一致：
     *   super = -33% em（浏览器约 1/3 em），sub = +20% em。
     */
    int baseline_dy = 0;
    {
        const char *vav = decls_get(st, "vertical-align");
        if (vav != NULL) {
            char vt[32];
            trim_lower(vav, vt, sizeof(vt));
            if (strcmp(vt, "baseline") == 0) {
                baseline_dy = 0;
            } else if (strcmp(vt, "super") == 0) {
                baseline_dy = -(font_px * 33 / 100);
            } else if (strcmp(vt, "sub") == 0) {
                baseline_dy = (font_px * 20 / 100);
            } else {
                set_error("%s: 不支持的 vertical-align '%s'（只支持 baseline / sub / super）",
                          where, vav);
                return 0;
            }
        }
    }

    /*
     * `font-family`：本库只有**两级**（正文 / 等宽）。
     * 认 `monospace` 一族；其他常见族名一律落到正文，不报错 ——
     * 写 `font-family: Arial, sans-serif` 的页面不该因为"没有 Arial"编译不过。
     * 要真正分家，得先 `ql_font_set_mono_factory()` 注册等宽字体。
     */
    uint8_t font_family = QL_UI_FAMILY_DEFAULT;
    {
        const char *ffv = decls_get(st, "font-family");
        if (ffv != NULL && strstr(ffv, "mono") != NULL)
            font_family = QL_UI_FAMILY_MONO;
    }

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
     *
     * `pre*` 系列不是"不理会"，而是**把空白折叠推迟到建表期**：
     * 原始空白一路留到样式算好之后才折叠（见 add_text_chunk / 建表期 intern）。
     * 所以这里只做取值翻译，不做任何文本处理。
     */
    int white_space = QL_UI_WS_NOWRAP;
    {
        const char *ws = decls_get(st, "white-space");
        if (ws != NULL) {
            char wt[64];
            trim_lower(ws, wt, sizeof(wt));
            if (strcmp(wt, "normal") == 0) {
                white_space = QL_UI_WS_NORMAL;
            } else if (strcmp(wt, "nowrap") == 0) {
                white_space = QL_UI_WS_NOWRAP;
            } else if (strcmp(wt, "pre") == 0) {
                white_space = QL_UI_WS_PRE;
            } else if (strcmp(wt, "pre-wrap") == 0) {
                white_space = QL_UI_WS_PRE_WRAP;
            } else if (strcmp(wt, "pre-line") == 0) {
                white_space = QL_UI_WS_PRE_LINE;
            } else {
                set_error("%s: 不支持的 white-space '%s'"
                          "（支持 nowrap / normal / pre / pre-wrap / pre-line）",
                          where, ws);
                return 0;
            }
        }
    }

    /*
     * object-fit：只对 `<img>` 有效。装进 font_flags 的高 4 位（见 qlui.h），
     * 所以这里的变量最后会被移位合并。
     */
    uint8_t object_fit = QL_UI_FIT_FILL;
    {
        const char *of = decls_get(st, "object-fit");
        if (of != NULL) {
            char ot[32];
            trim_lower(of, ot, sizeof(ot));
            if (strcmp(ot, "fill") == 0)
                object_fit = QL_UI_FIT_FILL;
            else if (strcmp(ot, "contain") == 0)
                object_fit = QL_UI_FIT_CONTAIN;
            else if (strcmp(ot, "cover") == 0)
                object_fit = QL_UI_FIT_COVER;
            else if (strcmp(ot, "none") == 0)
                object_fit = QL_UI_FIT_NONE;
            else if (strcmp(ot, "scale-down") == 0)
                object_fit = QL_UI_FIT_SCALE_DOWN;
            else {
                set_error("%s: 不支持的 object-fit '%s'"
                          "（fill / contain / cover / none / scale-down）", where, of);
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
                          1, calc, &unit, &num))
            return 0;
        if (!length_wants_const(unit, where, "border-width"))
            return 0;
        bw = num;
        if (!parse_length((v = decls_get(st, "border-radius")) != NULL ? v : "0", where,
                          1, calc, &unit, &num))
            return 0;
        if (!length_wants_const(unit, where, "border-radius"))
            return 0;
        radius = num;
    }

    /*
     * `border-style`：`none` 按 CSS 语义把宽度也归零（否则"没有样式却占 2px"
     * 会让内容盒缩水，这是最容易踩的坑）。
     */
    uint8_t border_style = QL_UI_BORDER_SOLID;
    if ((v = decls_get(st, "border-style")) != NULL) {
        if (!parse_border_style_kw(v, "border-style", &border_style))
            return 0;
        if (border_style == QL_UI_BORDER_NONE)
            bw = 0;
    }

    /*
     * `outline`：**不占布局**的一圈线（focus 环用它）。
     * 默认 outline-style = none、宽度 0 —— 不写就完全不存在。
     */
    uint32_t outline_color = bd;
    uint8_t outline_style = QL_UI_BORDER_NONE;
    int outline_style_set = 0;   /* 显式写了 outline-style（`none` 不该被宽度顶掉） */
    int outline_w = 0;
    int outline_off = 0;
    {
        if ((v = decls_get(st, "outline-style")) != NULL) {
            if (!parse_border_style_kw(v, "outline-style", &outline_style))
                return 0;
            outline_style_set = 1;
        }
        if ((v = decls_get(st, "outline-width")) != NULL) {
            uint8_t unit;
            int num;
            if (!parse_length(v, where, 0, calc, &unit, &num))
                return 0;
            if (!length_wants_const(unit, where, "outline-width"))
                return 0;
            if (num < 0 || num > 64) {
                set_error("%s: outline-width 必须在 0~64px（收到 '%s'）", where, v);
                return 0;
            }
            outline_w = num;
            /*
             * 只写宽度**不画** —— 这是 CSS 的语义（`outline-style` 初值是 none，
             * 省略样式就等于没有线）。要画就写 `outline: 2px solid red`。
             * 反过来做（宽度隐含 solid）看着方便，但会让
             * `outline-width: 2px` 这种"只改宽度"的覆盖意外画出线来。
             */
            (void)outline_style_set;
        }
        if ((v = decls_get(st, "outline-offset")) != NULL) {
            uint8_t unit;
            int num;
            if (!parse_length(v, where, 0, calc, &unit, &num))
                return 0;
            if (!length_wants_const(unit, where, "outline-offset"))
                return 0;
            if (num < -64 || num > 64) {
                set_error("%s: outline-offset 必须在 -64~64px（收到 '%s'）", where, v);
                return 0;
            }
            outline_off = num;
        }
        if ((v = decls_get(st, "outline-color")) != NULL &&
            !parse_color(v, where, &outline_color, &a))
            return 0;
        if (outline_style == QL_UI_BORDER_NONE || outline_w <= 0) {
            outline_w = 0;
            outline_style = QL_UI_BORDER_NONE;   /* 没宽度 = 不画 */
        }
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
    /* flex 项：`align-self` / `order` */
    uint8_t align_self = QL_UI_ALIGN_SELF_AUTO;
    if ((v = decls_get(st, "align-self")) != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        if (strcmp(low, "auto") != 0) {
            const int as = align_value(low);
            if (as < 0) {
                set_error("%s: 不支持的 align-self '%s'", where, v);
                return 0;
            }
            align_self = (uint8_t)as;
        }
    }
    int order = 0;
    if ((v = decls_get(st, "order")) != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        const char *end = NULL;
        double num = 0;
        if (!match_number(low, &end, &num) || *end != '\0' || num != (double)(int)num) {
            set_error("%s: order 只接受整数（收到 '%s'）", where, v);
            return 0;
        }
        if (num < -32768.0 || num > 32767.0) {
            set_error("%s: order 超出 -32768..32767", where);
            return 0;
        }
        order = (int)num;
    }

    /* `visibility` / `pointer-events` */
    uint8_t visibility = QL_UI_VISIBLE;
    if ((v = decls_get(st, "visibility")) != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        if (strcmp(low, "visible") == 0)
            visibility = QL_UI_VISIBLE;
        else if (strcmp(low, "hidden") == 0 || strcmp(low, "collapse") == 0)
            visibility = QL_UI_HIDDEN;
        else {
            set_error("%s: 不支持的 visibility '%s'（只支持 visible/hidden）", where, v);
            return 0;
        }
    }
    uint8_t pointer_events = QL_UI_POINTER_AUTO;
    if ((v = decls_get(st, "pointer-events")) != NULL) {
        char low[32];
        trim_lower(v, low, sizeof(low));
        if (strcmp(low, "auto") == 0)
            pointer_events = QL_UI_POINTER_AUTO;
        else if (strcmp(low, "none") == 0)
            pointer_events = QL_UI_POINTER_NONE;
        else {
            set_error("%s: 不支持的 pointer-events '%s'（只支持 auto/none）", where, v);
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
    /*
     * `overflow` 简写写两个轴，`overflow-x` / `overflow-y` 各写一个。
     * 后写的覆盖先写的（与 CSS 的书写顺序一致：简写就是两条长写）。
     */
    int ov = QL_UI_OVERFLOW_VISIBLE;
    int ovx = QL_UI_OVERFLOW_VISIBLE;
    int ovy = QL_UI_OVERFLOW_VISIBLE;
    char ovlow[32];
    if ((v = decls_get(st, "overflow")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        const int o = overflow_value(ovlow);
        if (o < 0) {
            set_error("%s: 不支持的 overflow '%s'", where, v);
            return 0;
        }
        ov = ovx = ovy = o;
    }
    if ((v = decls_get(st, "overflow-x")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        const int o = overflow_value(ovlow);
        if (o < 0) {
            set_error("%s: 不支持的 overflow-x '%s'", where, v);
            return 0;
        }
        ovx = o;
    }
    if ((v = decls_get(st, "overflow-y")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        const int o = overflow_value(ovlow);
        if (o < 0) {
            set_error("%s: 不支持的 overflow-y '%s'", where, v);
            return 0;
        }
        ovy = o;
    }
    /* 两轴一致时简写值跟着走，方便宿主/工具直接读 .overflow */
    if (ovx == ovy)
        ov = ovx;
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
        /* 与 width/height 同一套长度语法（px / % / em / rem / vw / vh / auto） */
        static const char *const sides[4] = { "top", "right", "bottom", "left" };
        for (int i = 0; i < 4; i++) {
            uint8_t unit = QL_UI_UNIT_AUTO;
            int val = 0;
            v = decls_get(st, sides[i]);
            if (v != NULL && !parse_length(v, where, 1, calc, &unit, &val))
                return 0;
            inset[i].value = (int16_t)val;
            inset[i].unit = unit;
            inset[i].reserved = 0;
        }
    }

    /* 过渡 */
    int trans_ms = 0, trans_props = 0, trans_ease = QL_EASE_LINEAR;
    if ((v = decls_get(st, "transition-duration")) != NULL) {
        if (!parse_duration(v, where, "transition-duration", &trans_ms))
            return 0;
        /*
         * `transition-property` 可以写多个（逗号或空格分隔）：
         * 位图本来就能存，只是以前只解析单个名字。
         */
        const char *tp = decls_get(st, "transition-property");
        trans_props = (int)QL_UI_TRANS_ALL;      /* 不写 = all（与 CSS 一致） */
        if (tp != NULL) {
            trans_props = 0;
            for (const char *q = tp;;) {
                while (is_space(*q) || *q == ',')
                    q++;
                if (*q == '\0')
                    break;
                const char *ts = q;
                while (*q != '\0' && *q != ',' && !is_space(*q))
                    q++;
                char tok[64], low[64];
                size_t len = (size_t)(q - ts);
                if (len >= sizeof(tok))
                    len = sizeof(tok) - 1u;
                memcpy(tok, ts, len);
                tok[len] = '\0';
                trim_lower(tok, low, sizeof(low));
                const int bit = trans_prop_bit(low);
                if (bit < 0) {
                    set_error("%s: 暂不支持的 transition-property '%s'", where, low);
                    return 0;
                }
                trans_props |= bit;
            }
            if (trans_props == 0) {
                set_error("%s: transition-property 是空的", where);
                return 0;
            }
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
    out->min_width.value = (int16_t)mnwv;
    out->min_width.unit = mnwu;
    out->max_width.value = (int16_t)mxwv;
    out->max_width.unit = mxwu;
    out->min_height.value = (int16_t)mnhv;
    out->min_height.unit = mnhu;
    out->max_height.value = (int16_t)mxhv;
    out->max_height.unit = mxhu;
    out->opacity = (uint8_t)opacity;
    out->aspect_w = aspect_w;
    out->aspect_h = aspect_h;
    out->letter_spacing = (int16_t)ls_v;
    out->text_indent = (int16_t)ti_v;
    /* em/rem 的单位先记在 reserved_text 里，末尾统一折（那时字号才确定） */
    out->reserved_text =
        (uint8_t)((ls_unit & 0x3u) | (uint8_t)((ti_unit & 0x3u) << 2));
    out->text_transform = text_transform;
    out->text_overflow = text_overflow;
    out->word_break = word_break;

    out->margin.top = margin_e[0].value;
    out->margin.right = margin_e[1].value;
    out->margin.bottom = margin_e[2].value;
    out->margin.left = margin_e[3].value;
    out->margin_unit = edges_unit_bits(margin_e);
    out->padding.top = padding_e[0].value;
    out->padding.right = padding_e[1].value;
    out->padding.bottom = padding_e[2].value;
    out->padding.left = padding_e[3].value;
    out->padding_unit = edges_unit_bits(padding_e);
    out->shadow_dx = (int16_t)sh_dx;
    out->shadow_dy = (int16_t)sh_dy;
    out->display = (uint8_t)disp;
    out->bg_alpha = bg_a;
    out->border_width = (uint8_t)bw;
    out->border_style = border_style;
    out->border_radius = (int16_t)radius;
    out->outline_color = outline_color;
    out->outline_width = (int8_t)outline_w;
    out->outline_offset = (int8_t)outline_off;
    out->outline_style = outline_style;
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

    /*
     * `em` 折成 px：必须放在**所有字段都写进 out 之后**（inset/margin/padding
     * 就排在前面几条赋值里）。此刻 `font_px` 已经是这个元素的最终字号
     * （`font-size` 的继承值也在 decls 里）；`rem` 要等整棵树走完才知道根字号。
     */
    resolve_length_units(out, font_px, 0, calc);
    resolve_edge_units(&out->margin, &out->margin_unit, font_px, 0);
    resolve_edge_units(&out->padding, &out->padding_unit, font_px, 0);
    /* letter-spacing / text-indent 共用同一套 em/rem 折算（1 位 em、2 位 rem） */
    {
        int16_t *lv[2] = { &out->letter_spacing, &out->text_indent };
        for (int i = 0; i < 2; i++) {
            const uint8_t u = (uint8_t)((out->reserved_text >> (2 * i)) & 0x3u);
            if (u == 1 && font_px > 0)
                *lv[i] = (int16_t)(((int32_t)*lv[i] * font_px) / 100);
            /* `rem` 留到整棵树走完之后统一折（见 ql_ui_load_text 末尾那趟） */
            out->reserved_text =
                (uint8_t)(out->reserved_text & (uint8_t)~(uint8_t)(0x3u << (2 * i)));
        }
    }
    /* align-content / cursor / user-select / scroll-behavior */
    uint8_t align_content = QL_UI_ALIGN_CONTENT_STRETCH;
    if ((v = decls_get(st, "align-content")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        const int ac = align_content_value(ovlow);
        if (ac < 0) {
            set_error("%s: 不支持的 align-content '%s'", where, v);
            return 0;
        }
        align_content = (uint8_t)ac;
    }
    uint8_t cursor = QL_UI_CURSOR_AUTO;
    if ((v = decls_get(st, "cursor")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        const int cu = cursor_value(ovlow);
        if (cu < 0) {
            set_error("%s: 不支持的 cursor '%s'（支持 auto/default/pointer/text/move/"
                      "not-allowed/help/progress）", where, v);
            return 0;
        }
        cursor = (uint8_t)cu;
    }
    uint8_t user_select_none = 0;
    if ((v = decls_get(st, "user-select")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        if (strcmp(ovlow, "none") == 0)
            user_select_none = 1;
        else if (strcmp(ovlow, "auto") != 0 && strcmp(ovlow, "text") != 0 &&
                 strcmp(ovlow, "all") != 0 && strcmp(ovlow, "contain") != 0) {
            set_error("%s: 不支持的 user-select '%s'", where, v);
            return 0;
        }
    }
    uint8_t scroll_smooth = 0;
    if ((v = decls_get(st, "scroll-behavior")) != NULL) {
        trim_lower(v, ovlow, sizeof(ovlow));
        if (strcmp(ovlow, "smooth") == 0)
            scroll_smooth = 1;
        else if (strcmp(ovlow, "auto") != 0) {
            set_error("%s: 不支持的 scroll-behavior '%s'（auto / smooth）", where, v);
            return 0;
        }
    }

    out->overflow_x = (uint8_t)ovx;
    out->overflow_y = (uint8_t)ovy;
    out->align_content = align_content;
    out->cursor = cursor;
    out->user_select_none = user_select_none;
    out->scroll_smooth = scroll_smooth;
    out->visibility = visibility;
    out->pointer_events = pointer_events;
    out->align_self = align_self;
    out->order = (int16_t)order;
    out->backdrop_blur = (uint8_t)backdrop;
    out->line_h = (int16_t)line_h;
    out->white_space = (uint8_t)white_space;
    out->baseline_dy = (int8_t)baseline_dy;
    out->font_family = font_family;
    out->font_flags = (uint8_t)(font_flags | (uint8_t)(object_fit << QL_UI_FIT_SHIFT));
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
                         AnimTable *anims, IntList *fonts, CalcPool *calc,
                         uint16_t *out)
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
    if (!compile_style(decls, where, ranges, kftab, anims, fonts, calc, &e->st)) {
        ql_mem_free(sig);
        return 0;
    }
    e->sig = sig;
    e->siglen = siglen;
    *out = (uint16_t)tab->n++;
    return 1;
}

/* 文字里有"运行期才定"的值（输入框/下拉框/多行输入）：`text-transform` 不动它们 */
static int bears_runtime_value(const char *tag)
{
    return tag != NULL && (strcmp(tag, "input") == 0 || strcmp(tag, "textarea") == 0 ||
                           strcmp(tag, "select") == 0);
}

/*
 * `text-transform` 的实现：只动 ASCII 字母（CJK 没有大小写），
 * `capitalize` 按"每个词的首字母" —— 词边界用空白与常见标点判定。
 * 返回新分配的字符串（调用方负责 free）。
 */
static char *apply_text_transform(const char *src, uint8_t tt)
{
    const size_t n = strlen(src);
    char *out = (char *)xalloc(n + 1u);
    if (out == NULL)
        return NULL;
    int at_word_start = 1;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)src[i];
        char r = (char)c;
        const int lower = (c >= 'a' && c <= 'z');
        const int upper = (c >= 'A' && c <= 'Z');
        if (tt == QL_UI_TEXT_UPPERCASE && lower) {
            r = (char)(c - 'a' + 'A');
        } else if (tt == QL_UI_TEXT_LOWERCASE && upper) {
            r = (char)(c - 'A' + 'a');
        } else if (tt == QL_UI_TEXT_CAPITALIZE && at_word_start && lower) {
            r = (char)(c - 'a' + 'A');
        }
        at_word_start = (c == ' ' || c == '\t' || c == '\n' || c == '-' || c == '/' ||
                         c == '(' || c == '.' || c == ',' || c == ':' || c == '"' ||
                         c == '\'');
        out[i] = r;
    }
    out[n] = '\0';
    return out;
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

/* ============================== 图像热区 ============================== */

/*
 * `<map name="m">` 里的 `<area>` 与 `<img usemap="#m">` 在**构建期**配对：
 * 每条热区直接记下 `<img>` 的节点序号，运行期就不用按名字查表，也没有
 * 字符串比较。`<map>` 可以写在 `<img>` 后面（甚至前面），所以配对放在
 * 整棵树展开之后。
 *
 * 坐标是**图片像素**（原点 = 图片左上角），与 HTML 的 `coords` 一致；
 * 运行期按图片实际绘制尺寸缩放（见 ql_ui_area_at）。
 */

/* 每条热区最多多少个坐标（poly 的点数上限 = 这个数 / 2） */
#define QL_AREA_COORD_MAX 128

typedef struct {
    QLUIArea *v;
    int16_t  *c;
    int       n;
    int       cn;
    int       vcap, ccap;
} AreaOut;

typedef struct {
    const char  *map;
    const PNode *area;
} AreaRef;

/* 把 "x0 y0 x1 y1"（空格或逗号分隔）解析成 int16；越界或格式错都在这里挡住 */
static int area_parse_coords(const char *s, int16_t *dst, int cap, int *out_n)
{
    int n = 0;
    const char *p = s;
    while (p != NULL && *p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            p++;
        if (*p == '\0')
            break;
        char *end = NULL;
        const long v = strtol(p, &end, 10);
        if (end == p) {
            set_error("HTML: <area coords=\"%.24s\"> 里有多余字符", s);
            return 0;
        }
        if (v < -32768L || v > 32767L) {
            set_error("HTML: <area coords> 的值 %ld 超出范围（±32767）", v);
            return 0;
        }
        if (n >= cap) {
            set_error("HTML: <area coords> 最多 %d 个坐标", (int)QL_AREA_COORD_MAX);
            return 0;
        }
        dst[n++] = (int16_t)v;
        p = end;
    }
    *out_n = n;
    return 1;
}

static int area_push(AreaOut *out, TextPool *pool, uint16_t img, const AreaRef *ref)
{
    const PNode *a = ref->area;

    if (out->n >= (int)QL_CFG_UI_AREA_MAX) {
        set_error("<area> 超过上限 %d 个（见 QL_CFG_UI_AREA_MAX）",
                  (int)QL_CFG_UI_AREA_MAX);
        return 0;
    }

    int16_t tmp[QL_AREA_COORD_MAX];
    int ncoord = 0;
    if (a->coords != NULL && !area_parse_coords(a->coords, tmp, (int)QL_AREA_COORD_MAX,
                                                &ncoord))
        return 0;

    /* 形状与坐标个数必须自洽 —— 数量不对就直接报错，不猜 */
    switch (a->area_shape) {
    case QL_UI_AREA_RECT:
        if (ncoord != 4) {
            set_error("HTML: <area shape=\"rect\"> 需要 4 个坐标（x0 y0 x1 y1），"
                      "实际 %d 个", ncoord);
            return 0;
        }
        break;
    case QL_UI_AREA_CIRCLE:
        if (ncoord != 3 || tmp[2] < 0) {
            set_error("HTML: <area shape=\"circle\"> 需要 \"cx cy r\"（r >= 0）");
            return 0;
        }
        break;
    case QL_UI_AREA_POLY:
        if (ncoord < 6 || (ncoord & 1) != 0) {
            set_error("HTML: <area shape=\"poly\"> 需要偶数个坐标且至少 6 个，"
                      "实际 %d 个", ncoord);
            return 0;
        }
        break;
    default: /* QL_UI_AREA_DEFAULT */
        if (ncoord != 0) {
            set_error("HTML: <area shape=\"default\"> 不该有 coords");
            return 0;
        }
        break;
    }

    /* 扩容（两条数组各自按需倍增，最后一次性拷进 arena） */
    if (out->n == out->vcap) {
        int cap = (out->vcap > 0) ? out->vcap * 2 : 8;
        QLUIArea *nv = (QLUIArea *)ql_mem_realloc(out->v, (size_t)cap * sizeof(QLUIArea));
        if (nv == NULL)
            return 0;
        out->v = nv;
        out->vcap = cap;
    }
    if (out->cn + ncoord > out->ccap) {
        int cap = (out->ccap > 0) ? out->ccap : 32;
        while (cap < out->cn + ncoord)
            cap *= 2;
        int16_t *nc = (int16_t *)ql_mem_realloc(out->c, (size_t)cap * sizeof(int16_t));
        if (nc == NULL)
            return 0;
        out->c = nc;
        out->ccap = cap;
    }

    QLUIArea *e = &out->v[out->n];
    memset(e, 0, sizeof(*e));
    e->img = img;
    e->coord_off = (uint16_t)out->cn;
    e->shape = a->area_shape;
    e->ncoord = (uint8_t)ncoord;
    if (!text_pool_intern(pool, a->href, &e->href) ||
        !text_pool_intern(pool, a->alt, &e->alt))
        return 0;
    for (int i = 0; i < ncoord; i++)
        out->c[out->cn + i] = tmp[i];
    out->cn += ncoord;
    out->n++;
    return 1;
}

static int build_areas(const Flat *flat, int nflat, TextPool *pool, AreaOut *out)
{
    if (QL_CFG_UI_AREA_MAX <= 0)
        return 1;

    AreaRef *refs = NULL;
    int nrefs = 0, cap = 0;

    for (int i = 0; i < nflat; i++) {
        const PNode *m = flat[i].node;
        if (strcmp(m->tag, "map") != 0 || m->map_name == NULL)
            continue;
        for (const PNode *c = m->first_child; c != NULL; c = c->next_sibling) {
            if (strcmp(c->tag, "area") != 0)
                continue;
            if (nrefs == cap) {
                int nc = (cap > 0) ? cap * 2 : 8;
                AreaRef *nr = (AreaRef *)ql_mem_realloc(refs,
                                                        (size_t)nc * sizeof(AreaRef));
                if (nr == NULL) {
                    ql_mem_free(refs);
                    return 0;
                }
                refs = nr;
                cap = nc;
            }
            refs[nrefs].map = m->map_name;
            refs[nrefs].area = c;
            nrefs++;
        }
    }

    int ok = 1;
    for (int i = 0; ok && i < nflat; i++) {
        const PNode *im = flat[i].node;
        if (strcmp(im->tag, "img") != 0 || im->usemap == NULL)
            continue;
        const char *want = (im->usemap[0] == '#') ? im->usemap + 1 : im->usemap;
        for (int r = 0; ok && r < nrefs; r++) {
            if (strcmp(refs[r].map, want) == 0)
                ok = area_push(out, pool, (uint16_t)i, &refs[r]);
        }
    }

    ql_mem_free(refs);
    return ok;
}

/* 继承只带走这三个属性（与 Python 的 INHERITED 一致） */
static const char *const k_inherited[] = { "color", "font-size", "text-align",
                                           "line-height", "white-space",
                                           "font-weight", "font-style",
                                           "text-decoration",
                                           /* `visibility` 是 CSS 的继承属性：
                                              父级 hidden 并不会"移除"子节点，
                                              子级可以自己写 visible 重新显示 */
                                           "visibility" };

static int collect_inherited(const Decls *parent, Decls *out)
{
    for (size_t i = 0; i < sizeof(k_inherited) / sizeof(k_inherited[0]); i++) {
        const char *v = decls_get(parent, k_inherited[i]);
        if (v != NULL && !decls_set(out, k_inherited[i], v))
            return 0;
    }
    /* `--x` 自定义属性默认**继承**（与 CSS 一致），所以整条带过去 */
    for (int i = 0; i < parent->n; i++) {
        if (parent->d[i].key[0] != '-')
            continue;
        if (!decls_set(out, parent->d[i].key, parent->d[i].val))
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
    MediaConds medias;
    CalcPool calc;
    AreaOut areas;
    Flat *flat = NULL;
    int nflat = 0;
    QLUIDoc *doc = NULL;
    void *arena = NULL;
    /* `@media` 的临时数组：**必须在函数开头声明** —— 中途 `goto done` 会跳过
     * 后面的初始化语句，届时 free 的就是野指针（踩过） */
    uint16_t *bucket_idx = NULL;
    uint16_t *node_media_ofs = NULL;
    uint16_t *media_pool = NULL;
    int media_pool_n = 0;
    int nrange = 0;
    QLUIMedia ranges[QL_CFG_UI_MAX_MEDIA];
    uint8_t range_on[QL_CFG_UI_MAX_MEDIA][QL_CFG_UI_MAX_MEDIA];

    memset(&rules, 0, sizeof(rules));
    memset(&kfs, 0, sizeof(kfs));
    memset(&kftab, 0, sizeof(kftab));
    memset(&kfranges, 0, sizeof(kfranges));
    memset(&anims, 0, sizeof(anims));
    memset(&styles, 0, sizeof(styles));
    memset(&pool, 0, sizeof(pool));
    memset(&fonts, 0, sizeof(fonts));
    memset(&medias, 0, sizeof(medias));
    memset(&calc, 0, sizeof(calc));
    memset(&areas, 0, sizeof(areas));

    root = parse_html(html);
    if (root == NULL)
        goto done;
    if (!parse_css(css, &rules, &kfs, &medias))
        goto done;
    /*
     * `::before` / `::after` 在这里变成真节点（构建期一次到位，运行期零成本）。
     * 先算兄弟位置 —— 选择器里可能同时写 `:first-child::before`。
     */
    assign_sibling_pos(root);
    if (!expand_pseudo(root, &rules))
        goto done;
    if (!kf_compile(&kfs, &kftab, &kfranges))
        goto done;
    if (!flatten_tree(root, &flat, &nflat))
        goto done;
    assign_sibling_pos(root);

    /* 逐节点算样式（DFS 顺序天然父先子后，继承直接取父的最终声明） */
    int root_style_idx = -1;

    /*
     * `@media`：把条件（视口宽区间）切成互不相交的**区间**。
     * 同一个区间里"哪些媒体块生效"是常量，所以每个区间编译一套样式就够，
     * 运行期只按视口宽选区间下标（线性扫，最多 6 个）。
     */
    memset(range_on, 0, sizeof(range_on));
    if (medias.n > 0) {
        uint32_t bnd[2 * QL_CFG_UI_MAX_MEDIA + 2];
        int nbd = 0;
        bnd[nbd++] = 0;
        for (int m = 0; m < medias.n; m++) {
            bnd[nbd++] = medias.v[m].lo;
            if (medias.v[m].hi != 0xFFFFu)
                bnd[nbd++] = (uint32_t)medias.v[m].hi + 1u;
        }
        /* 排序去重 */
        for (int a = 0; a < nbd; a++)
            for (int b2 = a + 1; b2 < nbd; b2++)
                if (bnd[b2] < bnd[a]) {
                    const uint32_t t = bnd[a];
                    bnd[a] = bnd[b2];
                    bnd[b2] = t;
                }
        int nu = 0;
        for (int a = 0; a < nbd; a++)
            if (a == 0 || bnd[a] != bnd[a - 1])
                bnd[nu++] = bnd[a];
        for (int a = 0; a < nu; a++) {
            if (nrange >= (int)QL_CFG_UI_MAX_MEDIA) {
                set_error("@media 的区间数超过上限 %d（条件写得太碎）",
                          (int)QL_CFG_UI_MAX_MEDIA);
                goto done;
            }
            const uint32_t lo = bnd[a];
            const uint32_t hi = (a + 1 < nu) ? (bnd[a + 1] - 1u) : 0xFFFFu;
            if (lo > hi)
                continue;                  /* 空区间（理论上不会） */
            ranges[nrange].lo = (uint16_t)lo;
            ranges[nrange].hi = (uint16_t)hi;
            for (int m = 0; m < medias.n; m++)
                if (lo >= medias.v[m].lo && lo <= medias.v[m].hi)
                    range_on[nrange][m] = 1;
            nrange++;
        }
    }

    const int nbuck = (nrange > 0) ? nrange : 1;
    /* 每个 (节点, 区间) 的 4 个样式下标：base / hover / active / focus */
    if (nrange > 0) {
        const size_t total = (size_t)nflat * (size_t)nbuck * 4u;
        bucket_idx = (uint16_t *)xalloc(total * sizeof(uint16_t));
        if (bucket_idx == NULL)
            goto done;
        for (size_t k = 0; k < total; k++)
            bucket_idx[k] = 0xFFFF;
    }

    for (int b = 0; b < nbuck; b++) {
    s_media_on = (nrange > 0) ? range_on[b] : NULL;

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
        /*
         * 多断点时会重复走这一趟：`finalize_style` 是"往 out 里合并"，
         * 所以每趟开始必须清空 —— 否则上一个断点独有的声明（如
         * `flex-direction: column`）会残留到下一个断点（踩过）。
         */
        decls_free(&e->final_decls);
        if (!finalize_style(&base, n, &inherited, &e->final_decls)) {

            decls_free(&base);
            decls_free(&inherited);
            goto done;
        }
        decls_free(&base);
        decls_free(&inherited);

        uint16_t idx = 0xFFFF;
        if (!style_tab_add(&styles, &e->final_decls, where, &kfranges, &kftab, &anims,
                           &fonts, &calc, &idx))
            goto done;
        if (b == 0) {
            e->style_index = idx;
            if (i == 0)
                root_style_idx = (int)idx;
        }
        if (bucket_idx != NULL)
            bucket_idx[((size_t)i * (size_t)nbuck + (size_t)b) * 4u] = idx;

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
                if (b == 0)
                    e->state[bi] = 0xFFFF;
                if (bucket_idx != NULL)
                    bucket_idx[((size_t)i * (size_t)nbuck + (size_t)b) * 4u +
                               (size_t)bi + 1u] = 0xFFFF;
            } else {
                uint16_t vidx = 0xFFFF;
                if (!style_tab_add(&styles, &vfinal, where, &kfranges, &kftab, &anims,
                                   &fonts, &calc, &vidx)) {
                    ql_mem_free(vsig);
                    decls_free(&vbase);
                    decls_free(&vinherited);
                    decls_free(&vfinal);
                    ql_mem_free(base_sig);
                    goto done;
                }
                if (b == 0)
                    e->state[bi] = vidx;
                if (bucket_idx != NULL)
                    bucket_idx[((size_t)i * (size_t)nbuck + (size_t)b) * 4u +
                               (size_t)bi + 1u] = vidx;
            }
            ql_mem_free(vsig);
            decls_free(&vbase);
            decls_free(&vinherited);
            decls_free(&vfinal);
            (void)vdecl;
        }
        ql_mem_free(base_sig);

        uint16_t tidx = 0xFFFF;
        {
            /*
             * 空白折叠在这里做（不是解析期）：只有到这一步才知道这个节点
             * 最终的 `white-space`。`pre*` 保留原文并把 `\t` 展开成空格，
             * 其余与旧行为逐字节相同（collapse_ws 就是原来那个函数）。
             */
            const char *src = n->text;
            char *conv = NULL;
            uint8_t ws = QL_UI_WS_NOWRAP;
            if (e->style_index >= 0 && e->style_index < styles.n)
                ws = styles.e[e->style_index].st.white_space;

            if (src != NULL) {
                if (ws == QL_UI_WS_PRE_LINE)
                    conv = collapse_ws_keep_nl(src);
                else if (QL_UI_WS_PRESERVES(ws))
                    conv = preserve_ws_expand_tabs(src);
                else
                    conv = collapse_ws(src);
                if (conv == NULL)
                    goto done;
                src = conv;

                /*
                 * `text-transform` 也在这里结算（同样是"到这一步才知道样式"）。
                 * 输入框/下拉框跳过：它们的文字是**运行期的值**，改写初始值会让
                 * `ql_ui_input_text()` 读回来的东西和别处对不上。
                 */
                uint8_t tt = QL_UI_TEXT_NONE;
                if (e->style_index >= 0 && e->style_index < styles.n)
                    tt = styles.e[e->style_index].st.text_transform;
                if (tt != QL_UI_TEXT_NONE && !bears_runtime_value(n->tag)) {
                    char *tc = apply_text_transform(src, tt);
                    if (tc == NULL)
                        goto done;
                    ql_mem_free(conv);
                    conv = tc;
                    src = tc;
                }
            }
            int ok = text_pool_intern(&pool, src, &tidx);
            ql_mem_free(conv);
            if (!ok)
                goto done;
        }
        e->text_index = tidx;
    }
    }           /* for (b) 断点区间 */
    s_media_on = NULL;

    /* 该节点在各区间里有没有变体？没有就不占媒体样式池（绝大多数节点都走这条） */
    if (bucket_idx != NULL && nrange > 0) {
        node_media_ofs = (uint16_t *)xalloc((size_t)nflat * sizeof(uint16_t));
        if (node_media_ofs == NULL)
            goto done;
        size_t cap = (size_t)nflat * (size_t)nbuck * 4u;
        media_pool = (uint16_t *)xalloc(cap * sizeof(uint16_t));
        if (media_pool == NULL)
            goto done;
        for (int i = 0; i < nflat; i++) {
            node_media_ofs[i] = 0xFFFF;
            int varies = 0;
            for (int b = 1; b < nbuck && !varies; b++)
                for (int k = 0; k < 4; k++)
                    if (bucket_idx[((size_t)i * (size_t)nbuck + (size_t)b) * 4u +
                                   (size_t)k] !=
                        bucket_idx[((size_t)i * (size_t)nbuck) * 4u + (size_t)k]) {
                        varies = 1;
                        break;
                    }
            if (!varies)
                continue;
            node_media_ofs[i] = (uint16_t)media_pool_n;
            for (int b = 0; b < nbuck; b++)
                for (int k = 0; k < 4; k++)
                    media_pool[media_pool_n++] =
                        bucket_idx[((size_t)i * (size_t)nbuck + (size_t)b) * 4u +
                                   (size_t)k];
        }
    }
    ql_mem_free(bucket_idx);
    bucket_idx = NULL;

    /*
     * `rem` 统一折一遍：走完整棵树才知道根元素最终用哪个字号
     * （根自己的 `font-size` 也可能是 em/px）。`em` 已经在 compile_style 里折过了。
     */
    if (root_style_idx >= 0 && root_style_idx < styles.n) {
        const uint8_t rfid = styles.e[root_style_idx].st.font_id;
        const int root_px = (rfid < fonts.n) ? fonts.v[rfid] : 16;
        for (int i = 0; i < styles.n; i++) {
            QLUIStyle *st = &styles.e[i].st;
            resolve_length_units(st, 0, root_px, &calc);
            const int fp = (st->font_id < fonts.n) ? fonts.v[st->font_id] : 16;
            resolve_edge_units(&st->margin, &st->margin_unit, fp, root_px);
            resolve_edge_units(&st->padding, &st->padding_unit, fp, root_px);
            {
                int16_t *lv[2] = { &st->letter_spacing, &st->text_indent };
                for (int k = 0; k < 2; k++) {
                    const uint8_t u = (uint8_t)((st->reserved_text >> (2 * k)) & 0x3u);
                    if (u == 2 && root_px > 0)
                        *lv[k] = (int16_t)(((int32_t)*lv[k] * root_px) / 100);
                    st->reserved_text = (uint8_t)(st->reserved_text &
                                                  (uint8_t)~(uint8_t)(0x3u << (2 * k)));
                }
            }
        }
    }

    /* 热区要在文本池定稿之前收（href / alt 也要进池） */
    if (!build_areas(flat, nflat, &pool, &areas))
        goto done;

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
        const size_t off_areas = off;
        off += QL_UI_ALIGN8((size_t)areas.n * sizeof(QLUIArea));
        const size_t off_area_coords = off;
        off += QL_UI_ALIGN8((size_t)areas.cn * sizeof(int16_t));
        const size_t off_media = off;
        off += QL_UI_ALIGN8((size_t)nrange * sizeof(QLUIMedia));
        const size_t off_node_media = off;
        if (nrange > 0)
            off += QL_UI_ALIGN8((size_t)nflat * sizeof(uint16_t));
        const size_t off_media_styles = off;
        if (nrange > 0)
            off += QL_UI_ALIGN8((size_t)media_pool_n * sizeof(uint16_t));
        const size_t off_calc = off;
        if (calc.n > 0)
            off += QL_UI_ALIGN8((size_t)calc.n * sizeof(QLUICalcOp));
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

        QLUIArea *ard = (QLUIArea *)(void *)(base + off_areas);
        for (int i = 0; i < areas.n; i++)
            ard[i] = areas.v[i];

        int16_t *acd = (int16_t *)(void *)(base + off_area_coords);
        for (int i = 0; i < areas.cn; i++)
            acd[i] = areas.c[i];

        QLUIMedia *mdd = (QLUIMedia *)(void *)(base + off_media);
        for (int i = 0; i < nrange; i++)
            mdd[i] = ranges[i];
        uint16_t *nmd_ofs = (uint16_t *)(void *)(base + off_node_media);
        uint16_t *mst = (uint16_t *)(void *)(base + off_media_styles);
        if (nrange > 0) {
            for (int i = 0; i < nflat; i++)
                nmd_ofs[i] = (node_media_ofs != NULL) ? node_media_ofs[i] : 0xFFFF;
            for (int i = 0; i < media_pool_n; i++)
                mst[i] = media_pool[i];
        }

        QLUICalcOp *cop = (QLUICalcOp *)(void *)(base + off_calc);
        for (int i = 0; i < calc.n; i++)
            cop[i] = calc.ops[i];

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
        doc->area_count = (uint16_t)areas.n;
        doc->area_coord_count = (uint16_t)areas.cn;
        doc->areas = (areas.n > 0) ? ard : NULL;
        doc->area_coords = (areas.cn > 0) ? acd : NULL;
        doc->media_count = (uint16_t)nrange;
        doc->media = (nrange > 0) ? mdd : NULL;
        /* 没有 @media 时不分配这两个数组（blob 头里计数为 0，两边一致） */
        doc->node_media = (nrange > 0) ? nmd_ofs : NULL;
        doc->media_styles = (nrange > 0) ? mst : NULL;
        doc->calc_op_count = (uint16_t)calc.n;
        doc->calc_ops = (calc.n > 0) ? cop : NULL;
        doc->arena = arena;
        arena = NULL;         /* 所有权交给 doc */
    }

done:
    ql_mem_free(medias.v);
    ql_mem_free(calc.ops);
    ql_mem_free(node_media_ofs);
    ql_mem_free(media_pool);
    ql_mem_free(bucket_idx);
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
    ql_mem_free(areas.v);
    ql_mem_free(areas.c);

    return doc;
}

