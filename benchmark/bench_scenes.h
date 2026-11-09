/**
 * @file    bench_scenes.h
 * @brief   性能维度表：一个维度 = 一个 `BenchScene` 条目。
 *
 * 为什么要分维度：**一个场景测不出全部**。填充率、文本排版、动画求值、
 * 增量重绘、布局重排是五条不同的路径，混在一个界面里量出来的数字
 * 只会告诉你"整体还行"，不会告诉你**哪条路径变慢了**。
 * 所以每个维度只压一条路径，跑完一个换下一个，最后出一张表。
 *
 * 加一个新维度：写好 `build`（HTML+CSS）和可选的 `tick`（每帧要动的东西），
 * 然后往文件末尾的 `k_scenes[]` 里加一行。别的什么都不用改 ——
 * `bench.c` 只认这张表，HUD / 统计 / 汇总表都不认识具体维度。
 */
#ifndef QL_BENCH_SCENES_H
#define QL_BENCH_SCENES_H

#include <stdarg.h>

#include "bench_common.h"

typedef struct BenchScene {
    const char *id;          /* 命令行用：--scene cards */
    const char *what;        /* 一句话：这个维度测什么 */
    int         default_scale; /* 份数的默认值 */
    int         max_scale;     /* 份数上限（节点数别越过 QL_CFG_UI_MAX_NODES） */
    /* 生成界面。失败返回 NULL 并打印原因 */
    QLUIDoc *(*build)(const BenchOpts *o, int scale);
    /*
     * 可选：每帧的"业务动作"。CSS 动画由 `ql_ui_anim_tick()` 驱动，
     * 但滚动、改值这些要应用自己做 —— 这类维度在这里动手。
     */
    void (*tick)(QLUIRoot *ui, uint32_t frame);
} BenchScene;

/* ------------------------------ 工具 ------------------------------ */

/* 攒 HTML 的小缓冲：场景都是"一堆重复元素"，手写 snprintf 太啰嗦 */
typedef struct {
    char  *buf;
    size_t cap, n;
    int    ok;
} BenchHtml;

static void bh_init(BenchHtml *h, size_t cap)
{
    h->buf = (char *)malloc(cap);
    h->cap = cap;
    h->n = 0;
    h->ok = (h->buf != NULL);
    if (h->ok)
        h->buf[0] = '\0';
}

static void bh_add(BenchHtml *h, const char *fmt, ...)
{
    if (!h->ok || h->n + 1 >= h->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(h->buf + h->n, h->cap - h->n, fmt, ap);
    va_end(ap);
    if (w < 0)
        h->ok = 0;
    else
        h->n += (size_t)w;
}

static QLUIDoc *bh_finish(BenchHtml *h, const char *css, const char *name)
{
    QLUIDoc *doc = NULL;
    if (h->ok)
        doc = ql_ui_load_text(h->buf, css);
    if (doc == NULL)
        printf("维度 %s 编译失败：%s\n", name, ql_ui_text_last_error());
    free(h->buf);
    h->buf = NULL;
    return doc;
}

/* -------------------- ① cards：静态卡片网格（填充 + 文本） -------------------- */

static const char *k_css_cards =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 8px;"
    "        background-color: #FFFFFF; padding: 10px;"
    "        font-size: 11px; color: #1F2328; }"
    ".hero { width: 100%; height: 44px; box-sizing: border-box;"
    "        border-radius: 10px; box-shadow: 0px 4px 10px #C9D1D9;"
    "        background-image: linear-gradient(to right, #2E6BE6, #7A5AF8); }"
    ".card { width: 122px; height: 62px; box-sizing: border-box; padding: 6px;"
    "        border-radius: 8px; border-width: 1px; border-color: #D0D7DE;"
    "        background-color: #F6F8FA; }"
    ".c2 { background-color: #EEF3FB; border-color: #C7D6EA; }"
    ".c3 { background-image: linear-gradient(to bottom, #F4F6F9, #E8EDF3); }"
    ".t { font-size: 12px; color: #1F2328; }"
    ".s { font-size: 10px; color: #57606A; }";

static QLUIDoc *scene_cards_build(const BenchOpts *o, int scale)
{
    BenchHtml h;
    bh_init(&h, (size_t)scale * 260u + 2048u);
    bh_add(&h, "<div id=\"root\"><div class=\"hero\"></div>");
    for (int i = 0; i < scale; i++) {
        const char *cls = (i % 3 == 1) ? "card c2" : ((i % 3 == 2) ? "card c3" : "card");
        if (o->cjk)
            bh_add(&h, "<div class=\"%s\"><div class=\"t\">温度 #%d</div>"
                       "<div class=\"s\">%d.%d 单位</div></div>",
                   cls, i, (i * 7) % 100, i % 10);
        else
            bh_add(&h, "<div class=\"%s\"><div class=\"t\">Item %d</div>"
                       "<div class=\"s\">%d.%d unit</div></div>",
                   cls, i, (i * 7) % 100, i % 10);
    }
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_cards, "cards");
}

/* -------------------- ② anim：多动画同时跑（动画求值 + 脏区） -------------------- */

static const char *k_css_anim =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 6px;"
    "        background-color: #FFFFFF; padding: 10px; }"
    ".b { width: 44px; height: 44px; border-radius: 8px;"
    "     background-color: #2FB8A6; animation: pulse 900ms ease-in-out infinite alternate; }"
    ".b2 { background-color: #3B82F6; animation-duration: 1200ms; }"
    ".b3 { background-color: #E6C34A; animation-duration: 700ms; }"
    ".b4 { background-color: #A855F7; animation-duration: 1500ms; }"
    "@keyframes pulse {"
    "  from { background-color: #BFE9E1; opacity: 1.0; transform: scale(1.0); }"
    "  to   { background-color: #2E6BE6; opacity: 0.35; transform: scale(0.7); }"
    "}";

static QLUIDoc *scene_anim_build(const BenchOpts *o, int scale)
{
    (void)o;
    BenchHtml h;
    bh_init(&h, (size_t)scale * 48u + 1024u);
    bh_add(&h, "<div id=\"root\">");
    for (int i = 0; i < scale; i++) {
        const char *cls = "b";
        if (i % 4 == 1)
            cls = "b b2";
        else if (i % 4 == 2)
            cls = "b b3";
        else if (i % 4 == 3)
            cls = "b b4";
        bh_add(&h, "<div class=\"%s\"></div>", cls);
    }
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_anim, "anim");
}

/* -------------------- ③ move：一堆元素上下左右运动（布局+变换+脏区） -------------------- */

/*
 * LVGL 那个经典场景：几十个小方块各按自己的方向/周期来回走。
 * 每帧都有元素位置变化 → 脏区多且分散，最能压住"动画必须走脏区"这条路径。
 */
static const char *k_css_move =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 14px;"
    "        background-color: #FFFFFF; overflow: hidden; padding: 10px; }"
    ".m { width: 26px; height: 26px; border-radius: 6px;"
    "     background-color: #2FB8A6; }"
    ".x { animation: mv-x 1400ms ease-in-out infinite alternate; }"
    ".y { animation: mv-y 1100ms ease-in-out infinite alternate; }"
    ".d { background-color: #3B82F6; animation: mv-d 1700ms ease-in-out infinite alternate; }"
    ".e { background-color: #E6C34A; animation: mv-y 800ms ease-in-out infinite alternate; }"
    "@keyframes mv-x { from { transform: translate(-120px, 0px); }"
    "                  to   { transform: translate(120px, 0px); } }"
    "@keyframes mv-y { from { transform: translate(0px, -80px); }"
    "                  to   { transform: translate(0px, 80px); } }"
    "@keyframes mv-d { from { transform: translate(-90px, -50px); }"
    "                  to   { transform: translate(90px, 50px); } }";

static QLUIDoc *scene_move_build(const BenchOpts *o, int scale)
{
    (void)o;   /* 摆放交给 flex 网格：本库没有 inline style，也不该为基准开这个口子 */
    BenchHtml h;
    bh_init(&h, (size_t)scale * 48u + 1024u);
    bh_add(&h, "<div id=\"root\">");
    for (int i = 0; i < scale; i++) {
        const char *anim = (i % 4 == 1) ? "m y"
                           : (i % 4 == 2) ? "m d"
                           : (i % 4 == 3) ? "m e" : "m x";
        bh_add(&h, "<div class=\"%s\"></div>", anim);
    }
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_move, "move");
}

/* -------------------- ④ text：文本密排（折行 + 字形缓存） -------------------- */

static const char *k_css_text =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        background-color: #FFFFFF; padding: 8px;"
    "        font-size: 12px; color: #1F2328; }"
    "p { white-space: normal; line-height: 16px; margin: 0 0 6px 0; }"
    ".h { color: #0969DA; font-size: 13px; }";

static QLUIDoc *scene_text_build(const BenchOpts *o, int scale)
{
    BenchHtml h;
    bh_init(&h, (size_t)scale * 300u + 2048u);
    bh_add(&h, "<div id=\"root\">");
    for (int i = 0; i < scale; i++) {
        if (o->cjk)
            bh_add(&h, "<p class=\"h\">第 %d 段：文本排版压力</p>"
                       "<p>这一段用来压自动折行与字形缓存。中文每个字都是一个独立的"
                       "断点，所以同样的宽度下换行次数比英文多，行盒计算与字形查表"
                       "都更费。行高固定 16px，避免不同字体行高不同导致每台机器"
                       "排出来的行数不一样。</p>", i);
        else
            bh_add(&h, "<p class=\"h\">Para %d: text layout stress</p>"
                       "<p>This paragraph exists to stress automatic line wrapping and "
                       "the glyph cache. Words are broken at spaces, long words are hard "
                       "broken when they exceed the line box, and every line end has to be "
                       "trimmed so trailing blanks do not occupy width.</p>", i);
    }
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_text, "text");
}

/* -------------------- ⑤ shapes：圆角/阴影/渐变/圆形遮罩（SDF + 混合） -------------------- */

static const char *k_css_shapes =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 10px; align-items: center;"
    "        background-color: #FFFFFF; padding: 12px; }"
    ".r { width: 56px; height: 56px; border-radius: 14px;"
    "     background-color: #2FB8A6; }"
    ".g { background-image: linear-gradient(to bottom, #2E6BE6, #7A5AF8); }"
    ".s { box-shadow: 0px 5px 12px #B6BEC8; }"
    ".c { border-radius: 28px; background-color: #E6C34A; }"
    ".b { border-width: 3px; border-color: #0969DA; background-color: #F6F8FA; }"
    ".k { clip-path: circle(45%); background-image: linear-gradient(to right, #A855F7, #2FB8A6); }";

static QLUIDoc *scene_shapes_build(const BenchOpts *o, int scale)
{
    (void)o;
    BenchHtml h;
    bh_init(&h, (size_t)scale * 64u + 1024u);
    bh_add(&h, "<div id=\"root\">");
    for (int i = 0; i < scale; i++) {
        switch (i % 5) {
        case 0: bh_add(&h, "<div class=\"r\"></div>"); break;
        case 1: bh_add(&h, "<div class=\"r g\"></div>"); break;
        case 2: bh_add(&h, "<div class=\"r s\"></div>"); break;
        case 3: bh_add(&h, "<div class=\"r c\"></div>"); break;
        default: bh_add(&h, "<div class=\"r b\"></div>"); break;
        }
    }
    /* 圆形遮罩单独来几个：clip-path 走的是"整棵子树圆形裁剪"，比圆角贵 */
    for (int i = 0; i < scale / 6 + 1; i++)
        bh_add(&h, "<div class=\"r k\"></div>");
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_shapes, "shapes");
}

/* -------------------- ⑥ scroll：长列表滚动（重排 + 裁剪） -------------------- */

static const char *k_css_scroll =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-direction: column; background-color: #FFFFFF;"
    "        padding: 10px; font-size: 12px; color: #1F2328; }"
    ".list { width: 100%; flex: 1 1 0px; overflow: auto;"
    "        background-color: #F6F8FA; border-radius: 8px; }"
    ".row { display: flex; gap: 8px; height: 28px; padding-left: 8px;"
    "       align-items: center; }"
    ".n { width: 40px; color: #6E7781; }";

static QLUIDoc *scene_scroll_build(const BenchOpts *o, int scale)
{
    (void)o;
    BenchHtml h;
    bh_init(&h, (size_t)scale * 80u + 1024u);
    bh_add(&h, "<div id=\"root\"><div class=\"list\">");
    for (int i = 0; i < scale; i++)
        bh_add(&h, "<div class=\"row\"><span class=\"n\">%04d</span>"
                   "<span>Row %d content</span></div>", i, i);
    bh_add(&h, "</div></div>");
    return bh_finish(&h, k_css_scroll, "scroll");
}

/*
 * 每帧往下滚 6px，到底回到顶部。
 * 注意 `ql_ui_scroll_by()` 内部会**重排整棵树**（偏移是烘进布局的），
 * 所以这个维度量的其实是"滚动 = 重排 + 裁剪"的真实成本。
 */
static void scene_scroll_tick(QLUIRoot *ui, uint32_t frame)
{
    (void)frame;
    /* 第 0 个 div 是 #root，第 1 个是 .list */
    QLUINodeRT *list = ql_ui_find_by_tag(ui, QL_UI_TAG_DIV, 1);
    if (list == NULL || !ql_ui_scrollable(list))
        return;
    const ql_coord_t max_y = list->scroll_max_y;
    const ql_coord_t next = list->scroll_y + 6;
    (void)ql_ui_scroll_by(ui, list, 0, (next > max_y) ? -list->scroll_y : 6);
}

/* -------------------- ⑦ form：控件密集（控件绘制路径） -------------------- */

static const char *k_css_form =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 10px;"
    "        background-color: #FFFFFF; padding: 12px;"
    "        font-size: 11px; color: #1F2328; }"
    ".cell { width: 236px; padding: 8px; box-sizing: border-box;"
    "        border-radius: 8px; background-color: #F6F8FA;"
    "        border-width: 1px; border-color: #D0D7DE; }"
    ".lbl { color: #57606A; font-size: 10px; margin-bottom: 3px; }"
    "input { width: 100%; height: 26px; padding: 0px 6px; font-size: 11px; }"
    "select { width: 100%; height: 26px; padding: 0px 6px; font-size: 11px; }"
    "textarea { width: 100%; height: 46px; padding: 4px; font-size: 10px;"
    "           line-height: 14px; white-space: pre-wrap; }"
    "progress { width: 100%; height: 8px; }"
    "button { padding: 3px 12px; font-size: 11px; }";

static QLUIDoc *scene_form_build(const BenchOpts *o, int scale)
{
    (void)o;
    BenchHtml h;
    bh_init(&h, (size_t)scale * 460u + 2048u);
    bh_add(&h, "<div id=\"root\">");
    for (int i = 0; i < scale; i++) {
        bh_add(&h, "<div class=\"cell\">"
                   "<div class=\"lbl\">名称 %d</div>"
                   "<input id=\"n%d\" value=\"value %d\">"
                   "<div class=\"lbl\">角色</div>"
                   "<select id=\"s%d\"><option>管理员</option><option>编辑</option>"
                   "<option>访客</option></select>"
                   "<div class=\"lbl\">备注</div>"
                   "<textarea id=\"d%d\">第一行 %d\n第二行</textarea>"
                   "<div class=\"lbl\">进度</div>"
                   "<progress value=\"%d\" max=\"100\"></progress>"
                   "<button>提交</button>"
                   "</div>",
                   i, i, i, i, i, i, (i * 13) % 100);
    }
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_form, "form");
}

/* -------------------- ⑧ dirty：只有一小块在动（增量重绘） -------------------- */

/*
 * 这个维度**只在 `--mode dirty` 下有独立意义**：整屏内容静止，
 * 只有一个小方块在跑关键帧。它回答的是"一次小改动要花多少"——
 * 也就是脏区优化的收益有多大。`--mode full` 下它就是"再测一遍整屏"。
 */
static const char *k_css_dirty =
    "#root { width: 100%; height: 100%; box-sizing: border-box;"
    "        display: flex; flex-wrap: wrap; gap: 8px;"
    "        background-color: #FFFFFF; padding: 10px; }"
    ".card { width: 122px; height: 62px; border-radius: 8px;"
    "        border-width: 1px; border-color: #D0D7DE;"
    "        background-color: #F6F8FA; }"
    ".dot { width: 40px; height: 40px; border-radius: 20px;"
    "       background-color: #2FB8A6;"
    "       animation: tip 900ms ease-in-out infinite alternate; }"
    "@keyframes tip { from { transform: translate(0px, 0px); }"
    "                 to   { transform: translate(220px, 0px); } }";

static QLUIDoc *scene_dirty_build(const BenchOpts *o, int scale)
{
    (void)o;
    BenchHtml h;
    bh_init(&h, (size_t)scale * 40u + 2048u);
    bh_add(&h, "<div id=\"root\"><div class=\"dot\"></div>");
    for (int i = 0; i < scale; i++)
        bh_add(&h, "<div class=\"card\"></div>");
    bh_add(&h, "</div>");
    return bh_finish(&h, k_css_dirty, "dirty");
}

/* ------------------------------ 维度表 ------------------------------ */

/*
 * 份数上限按节点预算给：`QL_CFG_UI_MAX_NODES` 默认 4096，
 * 一张卡/一行 3~10 个节点，留足余量。
 */
static const BenchScene k_scenes[] = {
    { "cards",  "填充率：圆角/边框/渐变/阴影 + 文本（静态网格）",   120, 1200,
      scene_cards_build,  NULL },
    { "anim",   "动画求值 + 脏区：几十个元素同时跑关键帧",           80,  800,
      scene_anim_build,   NULL },
    { "move",   "元素上下左右运动：分散的脏区 + transform",          64,  600,
      scene_move_build,   NULL },
    { "text",   "文本排版：自动折行 + 字形缓存",                     40,  200,
      scene_text_build,   NULL },
    { "shapes", "几何：圆角/阴影/渐变/圆形遮罩（SDF 与混合）",       100,  900,
      scene_shapes_build, NULL },
    { "scroll", "滚动：滚动 = 重排整棵树 + 裁剪",                   300, 2000,
      scene_scroll_build, scene_scroll_tick },
    { "form",   "控件路径：input/select/textarea/progress/button",   24,  200,
      scene_form_build,   NULL },
    { "dirty",  "增量重绘：全屏静止、只有一个小块在动",              200,  900,
      scene_dirty_build,  NULL },
};

#define BENCH_SCENE_COUNT ((int)(sizeof(k_scenes) / sizeof(k_scenes[0])))

static const BenchScene *bench_scene_find(const char *id)
{
    for (int i = 0; i < BENCH_SCENE_COUNT; i++)
        if (strcmp(k_scenes[i].id, id) == 0)
            return &k_scenes[i];
    return NULL;
}

static void bench_scene_list(void)
{
    printf("可用维度（--scene 后接 id，逗号分隔；all = 全部）：\n");
    for (int i = 0; i < BENCH_SCENE_COUNT; i++)
        printf("  %-8s 份数默认 %-4d  %s\n", k_scenes[i].id,
               k_scenes[i].default_scale, k_scenes[i].what);
    printf("\n份数用 --scale N 覆盖（弹性上限见上表 max_scale）。\n");
}

/* 把 --scene 的逗号列表拆开（就地改写，最多 16 个） */
static int bench_scene_split(char *buf, char *out[], int max)
{
    int n = 0;
    char *p = buf;
    while (p != NULL && *p != '\0' && n < max) {
        out[n++] = p;
        char *comma = strchr(p, ',');
        if (comma == NULL)
            break;
        *comma = '\0';
        p = comma + 1;
    }
    return n;
}

#endif /* QL_BENCH_SCENES_H */
