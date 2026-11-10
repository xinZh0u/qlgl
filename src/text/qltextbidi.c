/**
 * @file    qltextbidi.c
 * @brief   双向文本：fribidi（可选，可 dlopen）+ 内建简化 UAX#9。
 *
 * 内建路径只做"单一嵌入层"：先给每个字符定一个级别（0=LTR, 1=RTL, 2=数字在
 * RTL 里），再按 UAX#9 的 L2 规则（从最高级别降到"最低的奇数级别"，反向
 * 连续同级别段）重排。这覆盖了 GUI 里 99% 的场景：
 *
 *   阿拉伯文/希伯来文句子 + 里面的数字（数字保持 LTR）+ 英文单词（保持 LTR）。
 *
 * 不做：嵌套嵌入（`unicode-bidi: isolate` 相互嵌套）、显式控制字符
 * （U+202A..U+202E）的完整处理、括号镜像。这些都在文档里如实标着。
 */
#include <qlgl/qlgl.h>
#include <qlgl/qltextbidi.h>

#include <stdlib.h>
#include <string.h>

/* ============================== 字符分类 ============================== */

typedef enum {
    BIDI_L = 0,   /* 强 LTR */
    BIDI_R,       /* 强 RTL（含阿拉伯字母 AL） */
    BIDI_EN,      /* 欧洲数字 */
    BIDI_AN,      /* 阿拉伯数字 */
    BIDI_N,       /* 中性（空白/标点） */
} QLBidiType;

/* 强 RTL 的码位区段（够 GUI 用：希伯来、阿拉伯、叙利亚、它拿、NKo、呈现形式） */
int ql_text_bidi_is_rtl_cp(uint32_t cp)
{
    return (cp >= 0x0590u && cp <= 0x05FFu) ||   /* 希伯来 */
           (cp >= 0x0600u && cp <= 0x07BFu) ||   /* 阿拉伯 / 叙利亚 / 它拿 / NKo */
           (cp >= 0x07C0u && cp <= 0x08FFu) ||   /* NKo 续 / 撒玛利亚 / 阿拉伯扩展 */
           (cp >= 0xFB1Du && cp <= 0xFDFFu) ||   /* 阿拉伯呈现形式 A */
           (cp >= 0xFE70u && cp <= 0xFEFFu) ||   /* 阿拉伯呈现形式 B */
           (cp >= 0x10800u && cp <= 0x10FFFu) || /* 古文字（腓尼基等） */
           (cp >= 0x1E800u && cp <= 0x1EFFFu);   /* 门德基卡库等 */
}

static QLBidiType bidi_type(uint32_t cp)
{
    if (cp >= '0' && cp <= '9')
        return BIDI_EN;
    if (cp >= 0x0660u && cp <= 0x0669u)          /* 阿拉伯-印度数字 */
        return BIDI_AN;
    if (cp >= 0x06F0u && cp <= 0x06F9u)          /* 扩展阿拉伯-印度数字 */
        return BIDI_AN;
    if (ql_text_bidi_is_rtl_cp(cp))
        return BIDI_R;
    if (cp == 0x00A0u || cp == 0x3000u)
        return BIDI_N;
    /* 空白、标点、控制符、以及绝大多数其它字符：先当中性，后面按两侧解析 */
    if ((cp < 0x80u && !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) ||
        (cp >= 0x2000u && cp <= 0x206Fu) || (cp >= 0x3000u && cp <= 0x303Fu))
        return BIDI_N;
    return BIDI_L;
}

int ql_text_bidi_has_rtl(const char *text, int32_t len)
{
    if (text == NULL)
        return 0;
    int32_t i = 0;
    while ((len < 0 ? text[i] != '\0' : i < len) && text[i] != '\0') {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        if (ql_text_bidi_is_rtl_cp(cp))
            return 1;
        i += used;
    }
    return 0;
}

/* ============================== 内建 UAX#9（单层） ============================== */

#define QL_BIDI_MAX_CPS  512

/*
 * 计算每个字符的嵌入级别：
 *   基准 0（LTR）：L → 0，R/AL → 1，EN/AN → 1 里算 2（I1）
 *   基准 1（RTL）：R/AL → 1，L → 2（I2），EN/AN → 2
 * 中性字符先按"两侧同向则跟随，否则跟随基准"解析（N1/N2 的简化）。
 */
static int bidi_levels(const uint32_t *cps, int32_t n, int par_rtl, uint8_t *levels)
{
    QLBidiType types[QL_BIDI_MAX_CPS];
    for (int32_t i = 0; i < n; i++)
        types[i] = bidi_type(cps[i]);

    /* N1/N2：反复向左/向右扫，把中性字符吃成邻居的方向 */
    for (int32_t i = 0; i < n; i++) {
        if (types[i] != BIDI_N)
            continue;
        int32_t j = i;
        while (j < n && types[j] == BIDI_N)
            j++;
        const QLBidiType before = (i > 0) ? types[i - 1] : BIDI_N;
        const QLBidiType after = (j < n) ? types[j] : BIDI_N;
        const int before_rtl = (before == BIDI_R);
        const int after_rtl = (after == BIDI_R);
        QLBidiType resolved;
        if (before_rtl && after_rtl)
            resolved = BIDI_R;
        else if (!before_rtl && !after_rtl && before != BIDI_N && after != BIDI_N)
            resolved = BIDI_L;
        else
            resolved = par_rtl ? BIDI_R : BIDI_L;   /* 边缘/混向：跟随基准方向 */
        for (int32_t k = i; k < j; k++)
            types[k] = resolved;
        i = j;
    }

    for (int32_t i = 0; i < n; i++) {
        uint8_t lvl;
        if (types[i] == BIDI_R) {
            lvl = 1;
        } else if (types[i] == BIDI_EN || types[i] == BIDI_AN) {
            /* 数字：基准 LTR 时是"偶数级别里的弱 LTR"，嵌在 RTL 段里才升一级；
               基准 RTL 时一律升到 2（I2）。这里用"左边最近的方向"近似上下文。 */
            int left_rtl = par_rtl;
            for (int32_t k = i - 1; k >= 0; k--) {
                if (types[k] == BIDI_R) {
                    left_rtl = 1;
                    break;
                }
                if (types[k] == BIDI_L) {
                    left_rtl = 0;
                    break;
                }
            }
            lvl = (uint8_t)(left_rtl ? 2 : (par_rtl ? 2 : 0));
        } else {
            lvl = (uint8_t)(par_rtl ? 2 : 0);
        }
        levels[i] = lvl;
    }
    return 1;
}

/* UAX#9 L2：从最高级别降到"最低的奇数级别"，反向连续同级别段 */
static void bidi_reorder(uint32_t *cps, int32_t n, const uint8_t *levels)
{
    uint8_t highest = 0;
    uint8_t lowest_odd = 255;
    for (int32_t i = 0; i < n; i++) {
        if (levels[i] > highest)
            highest = levels[i];
        if ((levels[i] & 1u) != 0u && levels[i] < lowest_odd)
            lowest_odd = levels[i];
    }
    if (lowest_odd == 255)
        return;

    for (int lvl = (int)highest; lvl >= (int)lowest_odd; lvl--) {
        int32_t i = 0;
        while (i < n) {
            if (levels[i] < (uint8_t)lvl) {
                i++;
                continue;
            }
            int32_t j = i;
            while (j < n && levels[j] >= (uint8_t)lvl)
                j++;
            /* 反向 [i, j) */
            for (int32_t a = i, b = j - 1; a < b; a++, b--) {
                const uint32_t t = cps[a];
                cps[a] = cps[b];
                cps[b] = t;
            }
            i = j;
        }
    }
}

/* ============================== fribidi（可选） ============================== */

#if QL_FEAT_CSS_BIDI && QL_CFG_HAVE_DLOPEN

typedef uint32_t QLBidiChar;      /* FriBidiChar */
typedef int32_t  QLBidiStrIndex;  /* FriBidiStrIndex */
typedef uint32_t QLBidiCharType;  /* FriBidiCharType */
typedef uint32_t QLBidiBracketType;
typedef int32_t  QLBidiParType;
typedef uint8_t  QLBidiLevel;     /* FriBidiLevel */
typedef uint32_t QLBidiFlags;

/*
 * 只用**不改字形**的三个函数：
 *   get_bidi_types / get_par_embedding_levels_ex / reorder_line
 *
 * 不用 `fribidi_log2vis` —— 它内部还会做阿拉伯"形状替换"（把字母换成
 * U+FE70.. 的呈现形式）。我们已经把整形交给 HarfBuzz 了，再替换一遍就是重复整形
 * （而且 HarfBuzz 面对呈现形式字符不会再连写，阿拉伯文会变成一溜孤立形）。
 */
typedef int (*ql_fribidi_types_fn)(const QLBidiChar *str, QLBidiStrIndex len,
                                   QLBidiCharType *bidi_types);
typedef int (*ql_fribidi_brackets_fn)(const QLBidiChar *str, QLBidiStrIndex len,
                                      const QLBidiCharType *bidi_types,
                                      QLBidiBracketType *bracket_types);
typedef int (*ql_fribidi_levels_fn)(const QLBidiCharType *bidi_types,
                                    const QLBidiBracketType *bracket_types,
                                    QLBidiStrIndex len, QLBidiParType *pbase_dir,
                                    QLBidiLevel *embedding_levels);
typedef int (*ql_fribidi_reorder_fn)(QLBidiFlags flags, const QLBidiCharType *bidi_types,
                                     QLBidiStrIndex len, QLBidiStrIndex off,
                                     QLBidiParType base_dir, QLBidiLevel *embedding_levels,
                                     QLBidiChar *visual_str, QLBidiStrIndex *map);

static ql_fribidi_types_fn    s_fb_types;
static ql_fribidi_brackets_fn s_fb_brackets;
static ql_fribidi_levels_fn   s_fb_levels;
static ql_fribidi_reorder_fn  s_fb_reorder;
static QLBidiParType          s_fb_par_ltr;
static QLBidiParType          s_fb_par_rtl;
static int s_fribidi_state;      /* 0 = 没试过，1 = 可用，-1 = 不可用 */

#if defined(QLGL_HAVE_FRIBIDI)
/* 构建期直连：直接取符号地址填进同一张表 */
#include <fribidi/fribidi.h>

_Static_assert(sizeof(FriBidiChar) == 4, "FriBidiChar 不是 32 位");
_Static_assert(sizeof(FriBidiStrIndex) == 4, "FriBidiStrIndex 不是 32 位");
_Static_assert(sizeof(FriBidiLevel) == 1, "FriBidiLevel 不是 8 位");

static int fribidi_fill_direct(void)
{
    s_fb_types = (ql_fribidi_types_fn)fribidi_get_bidi_types;
    s_fb_brackets = (ql_fribidi_brackets_fn)fribidi_get_bracket_types;
    s_fb_levels = (ql_fribidi_levels_fn)fribidi_get_par_embedding_levels_ex;
    s_fb_reorder = (ql_fribidi_reorder_fn)fribidi_reorder_line;
    s_fb_par_ltr = (QLBidiParType)FRIBIDI_PAR_LTR;
    s_fb_par_rtl = (QLBidiParType)FRIBIDI_PAR_RTL;
    QL_LOGI("bidi: using fribidi (direct link)");
    s_fribidi_state = 1;
    return 1;
}
#endif

static int fribidi_load(void)
{
    if (s_fribidi_state != 0)
        return s_fribidi_state > 0;

    /* 测试/排查用：QL_TEXT_BIDI=builtin 强制走内建 UAX#9 子集 */
    {
        const char *force = getenv("QL_TEXT_BIDI");
        if (force != NULL && strcmp(force, "builtin") == 0) {
            s_fribidi_state = -1;
            return 0;
        }
    }

#if defined(QLGL_HAVE_FRIBIDI)
    return fribidi_fill_direct();
#else

    void *lib = ql_dlopen("libfribidi.so.0");
    if (lib == NULL)
        lib = ql_dlopen("libfribidi.so");
    if (lib == NULL) {
        QL_LOGI("bidi: fribidi not found, using built-in UAX#9 subset");
        s_fribidi_state = -1;
        return 0;
    }
    QL_DLSYM(lib, "fribidi_get_bidi_types", s_fb_types);
    QL_DLSYM(lib, "fribidi_get_bracket_types", s_fb_brackets);
    QL_DLSYM(lib, "fribidi_get_par_embedding_levels_ex", s_fb_levels);
    QL_DLSYM(lib, "fribidi_reorder_line", s_fb_reorder);

    if (s_fb_types == NULL || s_fb_levels == NULL || s_fb_reorder == NULL) {
        QL_LOGW("bidi: fribidi found but symbols are missing, using built-in");
        ql_dlclose(lib);
        s_fribidi_state = -1;
        return 0;
    }

    /*
     * `FRIBIDI_PAR_LTR` / `FRIBIDI_PAR_RTL` 的数值我们不想写死（各版本都可能有
     * 微调），但它们的定义就是"强 LTR / 强 RTL 的 bidi type 值"。所以直接问库：
     * 对一个 'a' 和一个希伯来字母取 type，拿到的就是这两个常量。
     */
    {
        const QLBidiChar ltr_ch = (QLBidiChar)'a';
        const QLBidiChar rtl_ch = 0x05D0u;      /* ALEF */
        QLBidiCharType t = 0;
        if (s_fb_types(&ltr_ch, 1, &t) != 0)
            s_fb_par_ltr = (QLBidiParType)t;
        if (s_fb_types(&rtl_ch, 1, &t) != 0)
            s_fb_par_rtl = (QLBidiParType)t;
    }

    s_fribidi_state = 1;
    QL_LOGI("bidi: using fribidi (dlopen)");
    return 1;
#endif /* QLGL_HAVE_FRIBIDI */
}

#else

static int fribidi_load(void)
{
    return 0;
}

#endif

/* ============================== 入口 ============================== */

int32_t ql_text_bidi_log2vis(const char *text, int32_t len, int par_rtl, char *out,
                             int32_t cap)
{
    if (text == NULL || out == NULL || cap <= 0)
        return -1;

    const int32_t total = (len < 0) ? (int32_t)strlen(text) : len;

    /* 没有强 RTL 字符：原样拷贝（LTR 页面只多一次扫描） */
    if (!ql_text_bidi_has_rtl(text, total)) {
        if (total + 1 > cap)
            return -1;
        memcpy(out, text, (size_t)total);
        out[total] = '\0';
        return total;
    }

    /* 解码成码位（重排只改码位顺序，最后统一重新编码） */
    uint32_t cps[QL_BIDI_MAX_CPS];
    uint8_t levels[QL_BIDI_MAX_CPS];
    int32_t n = 0;
    for (int32_t i = 0; i < total;) {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        if (n >= QL_BIDI_MAX_CPS)
            return -1;          /* 一行太长：交给调用方按逻辑顺序画 */
        cps[n++] = cp;
        i += used;
    }
    if (n == 0)
        return -1;

    int done = 0;
#if QL_FEAT_CSS_BIDI && QL_CFG_HAVE_DLOPEN
    if (fribidi_load()) {
        QLBidiCharType types[QL_BIDI_MAX_CPS];
        QLBidiBracketType brackets[QL_BIDI_MAX_CPS];
        QLBidiLevel flv[QL_BIDI_MAX_CPS];
        QLBidiStrIndex map[QL_BIDI_MAX_CPS];
        QLBidiParType par = par_rtl ? s_fb_par_rtl : s_fb_par_ltr;

        if (s_fb_types(cps, n, types) != 0) {
            memset(brackets, 0, (size_t)n * sizeof(brackets[0]));
            if (s_fb_brackets != NULL)
                (void)s_fb_brackets(cps, n, types, brackets);
            if (s_fb_levels(types, brackets, n, &par, flv) != 0) {
                /*
                 * reorder_line 把 visual_str 重排成视觉顺序（只换位置，**不改字形**
                 * —— 呈现形式替换只发生在 fribidi_log2vis / fribidi_shape 里）。
                 * map 参数这里不用：它的下标语义在不同版本上略有出入，直接读
                 * visual_str 更稳。
                 */
                uint32_t vis[QL_BIDI_MAX_CPS];
                memcpy(vis, cps, (size_t)n * sizeof(uint32_t));
                if (s_fb_reorder(0, types, n, 0, par, flv, vis, map) != 0) {
                    memcpy(cps, vis, (size_t)n * sizeof(uint32_t));
                    done = 1;
                }
                (void)map;
            }
        }
    }
#else
    (void)done;
#endif

    if (!done) {
        bidi_levels(cps, n, par_rtl ? 1 : 0, levels);
        bidi_reorder(cps, n, levels);
    }

    int32_t o = 0;
    for (int32_t i = 0; i < n; i++) {
        char tmp[4];
        const int32_t w = ql_utf8_encode(cps[i], tmp);
        if (w <= 0)
            continue;
        if (o + w + 1 > cap)
            return -1;
        memcpy(out + o, tmp, (size_t)w);
        o += w;
    }
    out[o] = '\0';
    return o;
}

const char *ql_text_bidi_backend(void)
{
    return fribidi_load() ? "fribidi" : "builtin";
}
