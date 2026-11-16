/**
 * @file    qlwebvtt.c
 * @brief   WebVTT（`.vtt`）解析：时间轴 + 纯文本。
 *
 * 只做 `<track kind=subtitles>` 需要的那部分：
 *   - `WEBVTT` 头、`NOTE` / `STYLE` / `REGION` 块跳过；
 *   - cue 时间行 `start --> end`（毫秒、`mm:ss.mmm`、`hh:mm:ss.mmm` 都认）；
 *   - 文本里的内联标签（`<b>` `<i>` `<c>` `<v>` …）和常见实体丢掉，
 *     只留纯文本（本库没有富文本字幕渲染器）。
 *
 * 不分配、不依赖平台，方便单测和 MCU 裁剪。
 */
#include <string.h>

#include "qlwebvtt.h"

typedef struct {
    const char *p;
    const char *end;
} Cursor;

static void skip_ws(Cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t'))
        c->p++;
}

/* 解析 `[hh:]mm:ss[.mmm]`，返回毫秒；失败返回 -1 */
static int64_t parse_time(Cursor *c)
{
    int64_t parts[3] = { 0, 0, 0 };
    int n = 0;

    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        int64_t v = 0;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            v = v * 10 + (*c->p++ - '0');
        if (n < 3)
            parts[n++] = v;
        if (c->p < c->end && *c->p == ':') {
            c->p++;
            continue;
        }
        break;
    }
    if (n == 0)
        return -1;

    int64_t ms = 0;
    if (c->p < c->end && (*c->p == '.' || *c->p == ',')) {
        c->p++;
        int64_t frac = 0, mul = 100;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            frac += (*c->p++ - '0') * mul;
            mul /= 10;
        }
        ms = frac;
    }

    if (n == 3)
        return ((parts[0] * 60 + parts[1]) * 60 + parts[2]) * 1000 + ms;
    if (n == 2)
        return (parts[0] * 60 + parts[1]) * 1000 + ms;
    return parts[0] * 1000 + ms;
}

static const char *find_text(const char *s, size_t n, const char *needle)
{
    const size_t m = strlen(needle);
    if (m == 0 || n < m)
        return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, needle, m) == 0)
            return s + i;
    return NULL;
}

static int line_next(const char **p, const char *end, const char **line,
                     size_t *len)
{
    if (*p >= end)
        return 0;
    const char *s = *p;
    const char *e = memchr(s, '\n', (size_t)(end - s));
    if (e == NULL)
        e = end;
    const char *next = (e < end) ? e + 1 : end;
    if (e > s && e[-1] == '\r')
        e--;
    *line = s;
    *len = (size_t)(e - s);
    *p = next;
    return 1;
}

static int line_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t')
            return 0;
    return 1;
}

/*
 * 把 cue 文本里的标签/实体消掉。`dst` 至少 `len+1`。
 * 返回有效长度。
 */
static size_t clean_text(const char *s, size_t n, char *dst)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o < n; i++) {
        if (s[i] == '<') {
            while (i < n && s[i] != '>')
                i++;
            continue;
        }
        if (s[i] == '&') {
            /* 只处理最常见的几个；其他实体原样保留（宁可显示 &amp;） */
            struct { const char *name; char ch; } ents[] = {
                { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
                { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", ' ' },
            };
            size_t matched = 0;
            for (size_t k = 0; k < sizeof(ents) / sizeof(ents[0]); k++) {
                const size_t l = strlen(ents[k].name);
                if (i + 1 + l <= n && memcmp(s + i + 1, ents[k].name, l) == 0) {
                    dst[o++] = ents[k].ch;
                    i += l;
                    matched = 1;
                    break;
                }
            }
            if (matched)
                continue;
        }
        dst[o++] = s[i];
    }
    return o;
}

int ql_vtt_parse(const char *text, size_t len, ql_vtt_cue_fn fn, void *user)
{
    if (text == NULL || fn == NULL)
        return -1;
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }

    const char *p = text;
    const char *end = text + len;
    const char *line;
    size_t n;
    int cues = 0;
    int saw_header = 0;
    int parsing = 0;
    int64_t start_ms = 0, end_ms = 0;
    char raw[2048];
    size_t raw_len = 0;

    while (line_next(&p, end, &line, &n)) {
        if (!saw_header) {
            if (line_blank(line, n))
                continue;
            if (n < 6 || memcmp(line, "WEBVTT", 6) != 0)
                return -1;
            saw_header = 1;
            continue;
        }

        if (line_blank(line, n)) {
            if (parsing && raw_len > 0) {
                char clean[2048];
                const size_t cl = clean_text(raw, raw_len, clean);
                /* 去掉首尾空白 */
                size_t a = 0, b = cl;
                while (a < b && (clean[a] == ' ' || clean[a] == '\t' ||
                                 clean[a] == '\n' || clean[a] == '\r'))
                    a++;
                while (b > a && (clean[b - 1] == ' ' || clean[b - 1] == '\t' ||
                                 clean[b - 1] == '\n' || clean[b - 1] == '\r'))
                    b--;
                if (b > a)
                    fn(user, start_ms, end_ms, clean + a, b - a);
                parsing = 0;
                raw_len = 0;
            }
            continue;
        }

        if (parsing) {
            if (raw_len + n + 1 < sizeof(raw)) {
                if (raw_len > 0)
                    raw[raw_len++] = '\n';
                memcpy(raw + raw_len, line, n);
                raw_len += n;
            }
            continue;
        }

        /* 块首：NOTE / STYLE / REGION 跳过整块 */
        if ((n >= 4 && memcmp(line, "NOTE", 4) == 0) ||
            (n >= 5 && memcmp(line, "STYLE", 5) == 0) ||
            (n >= 6 && memcmp(line, "REGION", 6) == 0)) {
            while (line_next(&p, end, &line, &n))
                if (line_blank(line, n))
                    break;
            continue;
        }

        /* 可能是 cue id（下一行才是时间行），也可能直接是时间行 */
        const char *arrow = find_text(line, n, "-->");
        if (arrow == NULL) {
            continue;                 /* id 行：忽略 */
        }

        Cursor c = { line, line + n };
        skip_ws(&c);
        start_ms = parse_time(&c);
        skip_ws(&c);
        if (start_ms < 0 || c.p + 3 > c.end || memcmp(c.p, "-->", 3) != 0)
            return -1;
        c.p += 3;
        skip_ws(&c);
        end_ms = parse_time(&c);
        if (end_ms < 0)
            return -1;
        if (end_ms < start_ms)
            end_ms = start_ms;

        parsing = 1;
        raw_len = 0;
        cues++;
    }

    if (parsing && raw_len > 0) {
        char clean[2048];
        const size_t cl = clean_text(raw, raw_len, clean);
        size_t a = 0, b = cl;
        while (a < b && (clean[a] == ' ' || clean[a] == '\t'))
            a++;
        while (b > a && (clean[b - 1] == ' ' || clean[b - 1] == '\t'))
            b--;
        if (b > a)
            fn(user, start_ms, end_ms, clean + a, b - a);
    }
    return cues;
}
