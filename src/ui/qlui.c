/**
 * @file    qlui.c
 * @brief   把构建期生成的 QLUIDoc 物化成节点树，并做布局与绘制。
 *
 * 布局模型（M3）：
 *   - block 流 / inline 流 / **flexbox**（row|column × nowrap|wrap）；
 *   - 宽度支持 px 与百分比（相对父内容宽度），高度支持 px 与 auto；
 *   - 文本节点由生成器折叠成"元素的一段文本"，暂不支持混合内容。
 *
 * flex 的实现要点：
 *   1. **先测量、再分配、最后定位**。测量靠"在哑位置跑一遍 layout_node 读尺寸"，
 *      虽然多一次遍历，但保证与既有 block/inline 路径的尺寸计算**完全一致**
 *      （比另写一套 measure 函数可靠得多；布局不在渲染热路径上）；
 *   2. flex-grow/shrink 分配完后，若最终尺寸与自然尺寸不同，就把尺寸写进
 *      `style_copy`（border-box 语义）再 layout 一次——复用既有机制，不另开接口；
 *   3. `flex-direction: column` + `height: auto` 时容器高度由内容决定，
 *      **没有剩余空间可分**，此时 flex-grow 无效（与浏览器一致）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

/* 定义在下拉框那一段；构建时初始化要用 */
static void select_sync_text(struct QLUISelectState *st);

/* 起播关键帧动画（定义在过渡那一段，构建时要先用） */
static void anim_boot(QLUIRoot *ui);

/* 定义在布局那一段；构建时判断"父节点是不是 flex 容器"要用 */
static int is_flex_container(const QLUIStyle *s);
static int is_grid_container(const QLUIStyle *s);
static ql_coord_t layout_table_children(QLUIRoot *ui, QLUINodeRT *node,
                                        ql_coord_t content_x, ql_coord_t content_y,
                                        ql_coord_t content_w, int definite_h);
static int is_table_container(const QLUIStyle *s);
static void sort_children_by_order(QLUINodeRT *node);
static ql_coord_t max_content_main(const QLUINodeRT *node, int row, ql_coord_t avail_w,
                                   int depth);

/* 下标 == QLUITag 枚举值，顺序必须与 qlui.h / qlui_parse.c 的 k_tags[] 一致 */
static const char *const s_tag_names[QL_UI_TAG_COUNT] = {
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
    /* --- EMBED 包：只做占位的 13 个 --- */
    "iframe", "embed", "object", "audio", "video", "track", "canvas",
    "script", "noscript", "slot", "template", "svg", "math",
    /* --- TEXT 包（P4） --- */
    "pre", "sub", "sup",
    "textarea",
    /* --- B7 表格 --- */
    "table", "caption", "thead", "tbody", "tfoot", "tr", "td", "th",
    "colgroup", "col",
};

const char *ql_ui_tag_name(uint16_t tag)
{
    return (tag < (uint16_t)QL_UI_TAG_COUNT) ? s_tag_names[tag] : "?";
}

/* 只有空白的文本（用来识别 flex 里"不该产生项"的空白节点） */
static int text_is_only_ws(const char *t)
{
    if (t == NULL || t[0] == '\0')
        return 0;
    for (const char *p = t; *p != '\0';) {
        uint32_t cp = 0;
        int32_t used = ql_utf8_decode(p, &cp);
        if (used <= 0)
            return 0;
        if (!(cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0u ||
              cp == 0x3000u))
            return 0;
        p += used;
    }
    return 1;
}

/*
 * 这个节点的 text 是不是**要显示的文字**？
 * `<img>` 的 text 存的是资源名（"logo.png"），既不该排版也不该绘制 ——
 * 所有"按文本测量/绘制"的地方都必须走这个判断。
 */
static int has_shown_text(const QLUINodeRT *n)
{
    if (n == NULL || n->text == NULL)
        return 0;
    return !(n->src != NULL && n->src->tag == QL_UI_TAG_IMG);
}

/* 共享的隐藏样式：只用来关掉"只含空白的 flex 项" */
static const QLUIStyle s_style_hidden = { .display = QL_UI_DISPLAY_NONE,
                                          .mask_image = QL_UI_NO_MASK };

struct QLUISelectState {
    QLUINodeRT *node;
    uint16_t    sel;        /* 选中第几个 <option>（0 基） */
    uint8_t     open;       /* 面板是否展开 */
};


/*
 * 每个 <input> 对应池里的一条。值缓冲是**运行期唯一可变**的文本，
 * 所以长度有硬上限（QL_CFG_UI_INPUT_LEN）。池只在 doc 里真的有 <input> 时分配。
 */
struct QLUIInput {
    QLUINodeRT *node;
    /*
     * 可变文本。**指针**而不是内联数组：单行框用结构体里自带的 line[]，
     * 多行框指向共享的 textarea arena（缓冲大得多，不能给每个单行框都背上）。
     */
    char       *buf;
    uint16_t    len;      /* 字节数（不含结尾 0） */
    uint16_t    caret;    /* 光标：字节偏移，0..len */
    uint16_t    sel;      /* 选区锚点；== caret 表示没有选区 */
    ql_coord_t  scroll_x; /* 文本比盒子宽时横向滚动的像素 */
    ql_coord_t  scroll_y; /* 多行：纵向滚动的像素（单行恒 0） */
    uint8_t     multiline;/* 1 = <textarea> */
    /*
     * 上下键要用"记住的横向位置"而不是当前行的实际 x：
     * 从长行移到短行时光标会被夹到行尾，再移回去应该回到原来的列
     * （浏览器的 sticky column）。-1 = 无效，下次需要重新量。
     */
    ql_coord_t  want_x;
    char        line[QL_CFG_UI_INPUT_LEN];   /* 单行框的存储 */
};

/*
 * 每个 <progress>/<meter> 对应池里的一条。
 *
 * 只存**完成度**（0..1000）与"有没有值"：布局与绘制都只需要"多满"，
 * value/max 是解析期的事（`<progress value="70" max="100">` → 700）。
 */
struct QLUIProgress;

struct QLUIProgress {
    QLUINodeRT *node;
    uint16_t    permille;      /* 0..1000 */
    uint8_t     known;         /* 0 = 没有 value 属性（不确定进度） */
};

/*
 * 每个 <img> 对应池里的一条。`img.px` 来自两处：doc 里的资源段（`.rodata`，
 * owned=0）或运行期注册时解码出来的内存（owned=1，ql_ui_free 负责释放）。
 */
struct QLUIImageRT {
    QLUINodeRT *node;        /* NULL = 只是背景图资源（B4），不对应节点 */
    const char *name;        /* 资源名（指向 doc 的文本池或节点的 text） */
    QLImage     img;
    uint8_t     failed;      /* 解码失败过：不要每帧重试 */
};

/* 下面这几组定义在文件后部（构建期就要用），先声明 */
static struct QLUIProgress *progress_of(const QLUIRoot *ui, const QLUINodeRT *node);
static struct QLUIImageRT *image_of(const QLUIRoot *ui, const QLUINodeRT *node);
static void image_node_size(const QLUIRoot *ui, const QLUINodeRT *node, ql_coord_t *w,
                            ql_coord_t *h);
static int image_fit_rect(int32_t iw, int32_t ih, QLRect box, uint8_t fit,
                          QLRect *out_dst, QLRect *out_src);
static void details_apply(QLUIRoot *ui, QLUINodeRT *nd, int open);
static QLUINodeRT *dialog_top(const QLUIRoot *ui);
static void dialog_apply(QLUIRoot *ui, QLUINodeRT *nd, int open);
static void dialog_center(QLUIRoot *ui);
static void render_node(QLUINodeRT *node, QLDraw2D *d2d);
/* 子节点分两趟画（普通流 → sticky），见 render_children 的说明 */
static void render_children(QLUINodeRT *node, QLDraw2D *d2d);
static const QLImage *image_by_name(const QLUIRoot *ui, const char *name);

/*
 * 一个样式可能引用的资源（文本池下标）：
 *   which 0 = background-image、1 = border-image、2 = mask
 * 返回 QL_UI_NO_IMAGE（= 0xFFFF）表示这一种没有。
 */
static inline uint16_t style_res_idx(const QLUIStyle *st, int which)
{
    switch (which) {
    case 1:
        return st->border_image;
    case 2:
        return st->mask_image;
    default:
        return st->bg_image;
    }
}
static void draw_bg_image(QLUINodeRT *node, QLRect box, QLDraw2D *d2d);

/* clip-path（v34）：形状长度换算（覆盖率由后端解析算，见 QLDraw2D::set_mask_*） */
static int ui_has_clip_pts(const QLUIRoot *ui, const QLUIStyle *s);
static ql_coord_t clip_len_to_px(QLUILength len, ql_coord_t span);
static void draw_border_image(QLUINodeRT *node, QLRect box, QLDraw2D *d2d);

/* ------------------------------ 构建 ------------------------------ */

#if QL_FEAT_CSS_FONT_STACK
static void font_chains_reset(void);
static void fonts_register_from_doc(QLUIRoot *ui);
static const QLFont *lbl_scale_hook(const QLFont *font, int px);
static QLUIDoc *s_last_doc;
#endif

static QLUIRoot *build_internal(const QLUIDoc *doc, int owns_doc)
{
    if (doc == NULL || doc->node_count == 0 || doc->nodes == NULL)
        return NULL;

    QLUIRoot *ui = (QLUIRoot *)ql_mem_calloc(1, sizeof(QLUIRoot));
    if (ui == NULL)
        return NULL;

    ui->nodes = (QLUINodeRT *)ql_mem_calloc(doc->node_count, sizeof(QLUINodeRT));
    if (ui->nodes == NULL) {
        ql_mem_free(ui);
        return NULL;
    }

    ui->doc = doc;
    ui->owns_doc = (uint8_t)(owns_doc ? 1 : 0);
    ui->node_count = doc->node_count;

#if QL_FEAT_CSS_FONT_STACK
    /*
     * 文本栈：把 doc 里的 @font-face 注册进字体注册表，并把"按家族列表重取字体"
     * 的钩子挂给绘制层（DPR 放大时要按同一个列表重取整条回退链）。
     * 放在这里而不是 doc 加载时：注册表是**进程级**的，doc 可以换。
     */
    font_chains_reset();
    fonts_register_from_doc(ui);
    ql_font_set_scale_hook(lbl_scale_hook);
#endif

    /* 生成器保证 nodes[] 是深度优先顺序，parent 下标一定小于子节点 */
    for (uint16_t i = 0; i < doc->node_count; i++) {
        const QLUINode *src = &doc->nodes[i];
        QLUINodeRT *rt = &ui->nodes[i];

        rt->owner = ui;
        rt->src = src;
        uint16_t style_count = (doc->style_count != 0) ? doc->style_count : doc->node_count;
        rt->style = (src->style < style_count) ? &doc->styles[src->style]
                                               : &doc->styles[0];
        rt->text = NULL;
        rt->m_hover = 0xFFFF;
        rt->m_active = 0xFFFF;
        rt->m_focus = 0xFFFF;
        /* B2：初始状态位（disabled / checked 都是构建期定下来的静态初值） */
        if ((src->flags & QL_UI_NODE_CHECKED) != 0)
            rt->flags |= QL_UI_RT_CHECKED;
        if ((src->flags & QL_UI_NODE_DISABLED) != 0)
            rt->flags |= QL_UI_RT_DISABLED;
        if (src->text != 0xFFFFu && doc->text_pool != NULL && doc->text_offsets != NULL)
            rt->text = doc->text_pool + doc->text_offsets[src->text];

        if (src->parent != 0xFFFFu && src->parent < doc->node_count) {
            QLUINodeRT *parent = &ui->nodes[src->parent];
            rt->parent = parent;
            rt->depth = (uint16_t)(parent->depth + 1);

            /* 按生成顺序挂到父节点末尾 */
            if (parent->first_child == NULL) {
                parent->first_child = rt;
            } else {
                QLUINodeRT *sibling = parent->first_child;
                while (sibling->next_sibling != NULL)
                    sibling = sibling->next_sibling;
                sibling->next_sibling = rt;
            }
        }
    }

    /*
     * 输入框池：doc 里真的出现 <input> 才分配（静态界面 + MCU 的常态是零字节）。
     * 顺便把每个输入框的 node->text 指到池里的可变缓冲 —— 之后编辑只改缓冲，
     * 绘制、ql_ui_set_text 那套一行都不用改。
     */
    uint16_t n_ta = 0;
    for (uint16_t i = 0; i < doc->node_count; i++) {
        if (doc->nodes[i].tag == QL_UI_TAG_INPUT) {
            /* 复选框/单选钮没有文本编辑，不占输入框池（B2） */
            if ((doc->nodes[i].flags & QL_UI_NODE_CHECKBOX) != 0)
                continue;
            ui->input_count = (uint16_t)(ui->input_count + 1u);
        }
        else if (doc->nodes[i].tag == QL_UI_TAG_TEXTAREA)
            n_ta = (uint16_t)(n_ta + 1u);
    }
    if (n_ta > QL_CFG_UI_TEXTAREA_MAX)
        n_ta = QL_CFG_UI_TEXTAREA_MAX;
    if (n_ta > 0) {
        ui->ta_arena = (char *)ql_mem_calloc(n_ta, QL_CFG_UI_TEXTAREA_LEN);
        if (ui->ta_arena == NULL) {
            ql_ui_free(ui);
            return NULL;
        }
        ui->ta_count = n_ta;
    }
    if (n_ta > 0)
        ui->input_count = (uint16_t)(ui->input_count + n_ta);

    if (ui->input_count > 0) {
        if (ui->input_count > QL_CFG_UI_INPUT_MAX)
            ui->input_count = QL_CFG_UI_INPUT_MAX;
        ui->inputs = (struct QLUIInput *)ql_mem_calloc(ui->input_count,
                                                       sizeof(struct QLUIInput));
        if (ui->inputs == NULL) {
            ql_ui_free(ui);
            return NULL;
        }
        uint16_t k = 0;
        uint16_t ta_slot = 0;
        for (uint16_t i = 0; i < doc->node_count && k < ui->input_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            const int is_ta = (rt->src->tag == QL_UI_TAG_TEXTAREA);
            if (rt->src->tag != QL_UI_TAG_INPUT && !is_ta)
                continue;
            if ((rt->src->flags & QL_UI_NODE_CHECKBOX) != 0)
                continue;
            struct QLUIInput *in = &ui->inputs[k++];
            in->node = rt;
            /*
             * 多行框从 arena 里切一段；单行框用结构体自带的 line[]。
             * 两者都走 in->buf，所以后面所有编辑逻辑**只有一份**。
             */
            uint16_t cap;
            if (is_ta && ta_slot < ui->ta_count) {
                in->buf = ui->ta_arena + (size_t)ta_slot * QL_CFG_UI_TEXTAREA_LEN;
                in->multiline = 1;
                cap = QL_CFG_UI_TEXTAREA_LEN;
                ta_slot++;
            } else {
                in->buf = in->line;
                cap = QL_CFG_UI_INPUT_LEN;
            }
            /* 初始值 = 解析期带进来的文本（`value` 属性或 `<textarea>` 的内容） */
            const char *init = rt->text;
            size_t n = (init != NULL) ? strlen(init) : 0u;
            if (n > (size_t)cap - 1u)
                n = (size_t)cap - 1u;
            if (n > 0)
                memcpy(in->buf, init, n);
            in->buf[n] = '\0';
            in->len = (uint16_t)n;
            in->caret = 0;
            in->sel = 0;
            rt->text = in->buf;
        }
    }

    /*
     * 进度条池：同样按需分配。文本里带的 "value/max" 在这里折成完成度，然后清掉
     * `rt->text` —— 那串数字只是**运输工具**，不是要显示的文字。
     */
    for (uint16_t i = 0; i < doc->node_count; i++)
        if (doc->nodes[i].tag == QL_UI_TAG_PROGRESS || doc->nodes[i].tag == QL_UI_TAG_METER)
            ui->progress_count = (uint16_t)(ui->progress_count + 1u);
    if (ui->progress_count > 0) {
        if (ui->progress_count > QL_CFG_UI_PROGRESS_MAX)
            ui->progress_count = (uint16_t)QL_CFG_UI_PROGRESS_MAX;
        ui->progresses = (struct QLUIProgress *)ql_mem_calloc(
            ui->progress_count, sizeof(struct QLUIProgress));
        if (ui->progresses == NULL) {
            ql_ui_free(ui);
            return NULL;
        }
        uint16_t k = 0;
        for (uint16_t i = 0; i < doc->node_count && k < ui->progress_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            if (rt->src->tag != QL_UI_TAG_PROGRESS && rt->src->tag != QL_UI_TAG_METER)
                continue;
            struct QLUIProgress *pg = &ui->progresses[k++];
            pg->node = rt;
            pg->permille = 0;
            pg->known = 0;
            if (rt->text != NULL && rt->text[0] != '\0') {
                const char *slash = strchr(rt->text, '/');
                const long v = strtol(rt->text, NULL, 10);
                const long mx = (slash != NULL) ? strtol(slash + 1, NULL, 10) : 100L;
                long mm = (mx > 0) ? mx : 100L;
                long p = (v * 1000L) / mm;
                if (p < 0)
                    p = 0;
                if (p > 1000)
                    p = 1000;
                pg->permille = (uint16_t)p;
                pg->known = 1;
            }
            rt->text = NULL;
        }
    }

    /*
     * 下拉框池：同样按需分配。默认选中第 0 项，并把 select 显示的文字
     * 指到那一项的文本上（之后换选中项只改指针，不复制字符串）。
     */
    for (uint16_t i = 0; i < doc->node_count; i++)
        if (doc->nodes[i].tag == QL_UI_TAG_SELECT)
            ui->select_count = (uint16_t)(ui->select_count + 1u);
    if (ui->select_count > 0) {
        if (ui->select_count > QL_CFG_UI_SELECT_MAX)
            ui->select_count = QL_CFG_UI_SELECT_MAX;
        ui->selects = (struct QLUISelectState *)ql_mem_calloc(
            ui->select_count, sizeof(struct QLUISelectState));
        if (ui->selects == NULL) {
            ql_ui_free(ui);
            return NULL;
        }
        uint16_t k = 0;
        for (uint16_t i = 0; i < doc->node_count && k < ui->select_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            if (rt->src->tag != QL_UI_TAG_SELECT)
                continue;
            struct QLUISelectState *st = &ui->selects[k++];
            st->node = rt;
            st->sel = 0;
            st->open = 0;
            select_sync_text(st);
        }
    }

    /*
     * flex 容器里"只有空白"的文本节点不产生 flex 项。
     *
     * CSS：匿名 flex 项如果只含空白就不渲染。解析期为了让
     * `<span>a</span> <span>b</span>` 之间的空格不被吃掉，会插入一个只含空格的
     * span；到了这里才知道父节点是不是 flex 容器。是的话就用一份**共享的隐藏样式**
     * 把它关掉——每个节点零额外内存，也不用改任何布局循环。
     */
    for (uint16_t i = 0; i < doc->node_count; i++) {
        QLUINodeRT *rt = &ui->nodes[i];
        if (rt->parent != NULL && rt->text != NULL && rt->first_child == NULL &&
            is_flex_container(rt->parent->style) && text_is_only_ws(rt->text))
            rt->style = &s_style_hidden;
    }

    /*
     * 图像池：按需分配。doc 里带的资源（构建期转好的 / blob 里的）在这里接上；
     * 没带资源的（`ql_ui_load_text` 的常态）留给 ql_ui_register_image()。
     */
    for (uint16_t i = 0; i < doc->node_count; i++)
        if (doc->nodes[i].tag == QL_UI_TAG_IMG)
            ui->image_count = (uint16_t)(ui->image_count + 1u);
    /*
     * 背景图（`background-image: url()`）与 `border-image` 也要占资源槽；
     * 同名只占一个（两者共用同一张图时只解码一次）。
     */
    uint16_t bg_slots = 0;
    for (uint16_t i = 0; i < doc->style_count; i++) {
        for (int which = 0; which < 3; which++) {
            const uint16_t idx = style_res_idx(&doc->styles[i], which);
            if (idx == QL_UI_NO_IMAGE)
                continue;
            const char *nm = doc->text_pool + doc->text_offsets[idx];
            int dup = 0;
            for (uint16_t j = 0; j < i && !dup; j++) {
                const uint16_t oi = style_res_idx(&doc->styles[j], which);
                if (oi == QL_UI_NO_IMAGE)
                    continue;
                const char *o = doc->text_pool + doc->text_offsets[oi];
                if (strcmp(nm, o) == 0)
                    dup = 1;
            }
            if (!dup)
                bg_slots++;
        }
    }
    ui->image_count = (uint16_t)(ui->image_count + bg_slots);
    if (ui->image_count > 0) {
        if (ui->image_count > QL_CFG_UI_IMAGE_MAX)
            ui->image_count = (uint16_t)QL_CFG_UI_IMAGE_MAX;
        ui->images = (struct QLUIImageRT *)ql_mem_calloc(ui->image_count,
                                                         sizeof(struct QLUIImageRT));
        if (ui->images == NULL) {
            ql_ui_free(ui);
            return NULL;
        }
        uint16_t k = 0;
        for (uint16_t i = 0; i < doc->node_count && k < ui->image_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            if (rt->src->tag != QL_UI_TAG_IMG)
                continue;
            struct QLUIImageRT *ir = &ui->images[k++];
            ir->node = rt;
            ir->name = rt->text;
            /* 在 doc 的资源表里按名字找（线性扫，只在 build 时做一次） */
            for (uint16_t j = 0; j < doc->image_count; j++) {
                const QLUIImageSrc *src = &doc->images[j];
                const char *nm = doc->text_pool + doc->text_offsets[src->name];
                if (rt->text == NULL || strcmp(nm, rt->text) != 0)
                    continue;
                QLImageDecodeReq req;
                memset(&req, 0, sizeof(req));
                req.data = doc->image_data + src->off;
                req.len = src->len;
                req.src_format = src->format;
                req.pixel_format = src->pixel_format;
                req.w = src->w;
                req.h = src->h;
                if (ql_image_decode(&req, &ir->img) != QL_OK)
                    ir->failed = 1;
                break;
            }
        }

        /* 背景图 / 边框图资源（node == NULL，只按名字索引） */
        for (uint16_t i = 0; i < doc->style_count && k < ui->image_count; i++) {
          for (int which = 0; which < 3; which++) {
            const uint16_t sidx = style_res_idx(&doc->styles[i], which);
            if (sidx == QL_UI_NO_IMAGE)
                continue;
            const char *nm = doc->text_pool + doc->text_offsets[sidx];
            int dup = 0;
            for (uint16_t j = 0; j < k && !dup; j++)
                if (ui->images[j].name != NULL && strcmp(ui->images[j].name, nm) == 0)
                    dup = 1;
            if (dup)
                continue;
            struct QLUIImageRT *ir = &ui->images[k++];
            ir->node = NULL;
            ir->name = nm;
            for (uint16_t j = 0; j < doc->image_count; j++) {
                const QLUIImageSrc *src = &doc->images[j];
                const char *srcname = doc->text_pool + doc->text_offsets[src->name];
                if (strcmp(srcname, nm) != 0)
                    continue;
                QLImageDecodeReq req;
                memset(&req, 0, sizeof(req));
                req.data = doc->image_data + src->off;
                req.len = src->len;
                req.src_format = src->format;
                req.pixel_format = src->pixel_format;
                req.w = src->w;
                req.h = src->h;
                if (ql_image_decode(&req, &ir->img) != QL_OK)
                    ir->failed = 1;
                break;
            }
          }
        }
    }

    /*
     * <dialog>：数一下有几个（为 0 时浮层相关的扫描全部跳过），
     * 没写 open 的直接关掉（浮层默认不显示）。
     */
    for (uint16_t i = 0; i < doc->node_count; i++)
        if (doc->nodes[i].tag == QL_UI_TAG_DIALOG)
            ui->dialog_count = (uint16_t)(ui->dialog_count + 1u);
    if (ui->dialog_count > 0)
        for (uint16_t i = 0; i < doc->node_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            if (rt->src != NULL && rt->src->tag == QL_UI_TAG_DIALOG)
                dialog_apply(ui, rt, (rt->src->flags & QL_UI_NODE_OPEN) != 0);
        }

    /*
     * <details> 的初始状态：没写 open 就把非 summary 的子节点关掉。
     * 必须在布局之前做完（布局只认 style->display）。
     */
    for (uint16_t i = 0; i < doc->node_count; i++) {
        QLUINodeRT *rt = &ui->nodes[i];
        if (rt->src != NULL && rt->src->tag == QL_UI_TAG_DETAILS)
            details_apply(ui, rt, (rt->src->flags & QL_UI_NODE_OPEN) != 0);
    }

    /*
     * 过渡池按需分配：doc 里一个 transition 都没有时完全不占内存
     * （静态界面 + MCU 的常态）。
     */
    for (uint16_t i = 0; i < doc->style_count; i++) {
        if (doc->styles[i].trans_ms > 0 && doc->styles[i].trans_props != 0) {
            ui->trans = (QLUITrans *)ql_mem_calloc(QL_CFG_UI_TRANS_MAX,
                                                   sizeof(QLUITrans));
            break;
        }
    }

    /* 关键帧动画同理：没有 @keyframes 就一字节都不占 */
    if (doc->anim_count > 0 && doc->anims != NULL) {
        ui->runs = (QLUIAnimRun *)ql_mem_calloc(QL_CFG_UI_RUN_ANIM_MAX,
                                                sizeof(QLUIAnimRun));
    }
    anim_boot(ui);

    return ui;
}

QLUIRoot *ql_ui_build(const QLUIDoc *doc)
{
    return build_internal(doc, 0);
}

QLUIRoot *ql_ui_load_file(const char *path)
{
    QLUIDoc *doc = ql_ui_doc_load(path);
    if (doc == NULL) {
        QL_LOGE("ui: load \"%s\" failed: %s", path != NULL ? path : "?",
                ql_ui_doc_last_error());
        return NULL;
    }

    QLUIRoot *ui = build_internal(doc, 1);
    if (ui == NULL)
        ql_ui_doc_free(doc);
    return ui;
}

static QLUIRoot *s_input_target;   /* 挂在输入系统上的根（同一时刻只有一个） */

void ql_ui_free(QLUIRoot *ui)
{
    if (ui == NULL)
        return;
#if QL_FEAT_CSS_FONT_STACK
    /*
     * 字体回退链与字体注册表都是"当前文档"的缓存：文档走了就一起放掉
     * （注册表里的字体是 @font-face 读进来的，不跟着文档走就是进程级泄漏）。
     * 顺序：先链（它引用了注册表里的 face），再注册表。
     */
    if (s_last_doc != NULL && ui->doc == s_last_doc) {
        font_chains_reset();
        ql_font_family_reset();
        s_last_doc = NULL;
    }
#endif
    /* 输入汇里还握着这个指针，必须先摘掉，否则下一次事件就是 use-after-free */
    if (s_input_target == ui)
        ql_ui_detach_input();
    /* Grid 布局的临时区（只增不减，直到这里才释放） */
    ql_mem_free(ui->grid_place);
    ql_mem_free(ui->grid_size);
    ql_mem_free(ui->grid_occ);
    ui->grid_place = NULL;
    ui->grid_size = NULL;
    ui->grid_occ = NULL;
    /* 先把还挂着的过渡动画摘掉，避免动画池里留着指向已释放节点的指针 */
    if (ui->trans != NULL) {
        for (int i = 0; i < QL_CFG_UI_TRANS_MAX; i++) {
            if (ui->trans[i].active)
                ql_anim_cancel(&ui->trans[i].key);
        }
        ql_mem_free(ui->trans);
    }
    ql_mem_free(ui->runs);
    if (ui->images != NULL) {
        for (uint16_t i = 0; i < ui->image_count; i++)
            if (ui->images[i].img.owned)
                ql_mem_free((void *)ui->images[i].img.px);
        ql_mem_free(ui->images);
    }
    ql_mem_free(ui->progresses);
    ql_mem_free(ui->ta_arena);
    ql_mem_free(ui->inputs);
    ql_mem_free(ui->selects);
    /* 模板实例化替 `{i}` 生成的文本 */
    if (ui->rep_texts != NULL) {
        for (uint16_t i = 0; i < ui->rep_count; i++)
            ql_mem_free(ui->rep_texts[i]);
        ql_mem_free(ui->rep_texts);
    }
    /* 运行期样式副本（懒分配，见 ensure_style_copy） */
    for (uint16_t i = 0; i < ui->node_count; i++)
        ql_mem_free(ui->nodes[i].style_copy);
    ql_mem_free(ui->nodes);
    if (ui->owns_doc)
        ql_ui_doc_free((QLUIDoc *)(uintptr_t)ui->doc);
    ql_mem_free(ui);
}

/* ------------------------------ 模板实例化 ------------------------------ */

/* 运行期样式副本（定义在 layout 那一节；实例化也要用它） */
static QLUIStyle *ensure_style_copy(QLUINodeRT *node);

/* 节点指针 → 下标（不在数组里就是 0xFFFF） */
static uint16_t node_index(const QLUIRoot *ui, const QLUINodeRT *n)
{
    if (n == NULL || ui == NULL || ui->nodes == NULL)
        return 0xFFFFu;
    const ptrdiff_t d = n - ui->nodes;
    if (d < 0 || d >= (ptrdiff_t)ui->node_count)
        return 0xFFFFu;
    return (uint16_t)d;
}

/*
 * 子树在节点数组里的结束下标（不含）。
 * 节点数组是深度优先顺序，子树因此连续 —— 判据就是"父链上能不能走到它"。
 * 这样写是对的且好读：节点数只有几百，调用又只在实例化时发生。
 */
static uint16_t subtree_end(const QLUIRoot *ui, uint16_t idx)
{
    const QLUINodeRT *me = &ui->nodes[idx];
    uint16_t i = (uint16_t)(idx + 1u);
    while (i < ui->node_count) {
        const QLUINodeRT *p = ui->nodes[i].parent;
        int inside = 0;
        while (p != NULL) {
            if (p == me) {
                inside = 1;
                break;
            }
            p = p->parent;
        }
        if (!inside)
            break;
        i++;
    }
    return i;
}

/* 把 `s` 里的 `{i}` 换成数值 `k`（其余原样）。返回新分配的字符串（归 root） */
static char *rep_subst(QLUIRoot *ui, const char *s, uint32_t k)
{
    if (s == NULL || strstr(s, "{i}") == NULL)
        return NULL;                 /* 没有令牌：继续借用原指针，零成本 */

    char num[16];
    snprintf(num, sizeof(num), "%u", (unsigned)k);
    const size_t nl = strlen(num);

    size_t cap = strlen(s) + 16u * 4u;
    char *out = (char *)ql_mem_alloc(cap);
    if (out == NULL)
        return NULL;
    size_t o = 0;
    for (const char *p = s; *p != '\0';) {
        if (p[0] == '{' && p[1] == 'i' && p[2] == '}') {
            if (o + nl + 1u > cap)
                break;
            memcpy(out + o, num, nl);
            o += nl;
            p += 3;
        } else {
            if (o + 2u > cap)
                break;
            out[o++] = *p++;
        }
    }
    out[o] = '\0';

    if (ui->rep_count >= ui->rep_cap) {
        const uint16_t cap2 = (uint16_t)((ui->rep_cap == 0) ? 16 : ui->rep_cap * 2);
        char **nt = (char **)ql_mem_alloc((size_t)cap2 * sizeof(char *));
        if (nt == NULL) {
            ql_mem_free(out);
            return NULL;
        }
        if (ui->rep_texts != NULL) {
            memcpy(nt, ui->rep_texts, (size_t)ui->rep_count * sizeof(char *));
            ql_mem_free(ui->rep_texts);
        }
        ui->rep_texts = nt;
        ui->rep_cap = cap2;
    }
    ui->rep_texts[ui->rep_count++] = out;
    return out;
}

ql_err_t ql_ui_repeat(QLUIRoot *ui, QLUINodeRT *tpl, uint16_t count)
{
    if (ui == NULL || tpl == NULL || ui->nodes == NULL)
        return QL_ERR_INVAL;
    const uint16_t ti = node_index(ui, tpl);
    if (ti == 0xFFFFu || ti == 0u)
        return QL_ERR_INVAL;             /* 根节点不能当模板 */
    const uint16_t te0 = subtree_end(ui, ti);
    const uint16_t sub0 = (uint16_t)(te0 - ti);
    if (sub0 == 0)
        return QL_ERR_INVAL;

    if (count == 0) {
        /*
         * 0 份 = 真的把这支**删掉**（不是 `display: none`）。
         * 不能用 style_copy 隐藏：`ql_ui_layout()` 每轮开头都会把 style_copy 清掉
         * （那是"上一帧的强制尺寸"的清理），运行期覆盖活不过一次布局。
         */
        QLUINodeRT *old = ui->nodes;
        const uint16_t old_n = ui->node_count;
        const uint16_t parent_idx = node_index(ui, tpl->parent);
        const uint16_t after_idx = node_index(ui, tpl->next_sibling);
        uint16_t prev_idx = 0xFFFFu;
        if (parent_idx != 0xFFFFu) {
            for (QLUINodeRT *c = tpl->parent->first_child; c != NULL && c != tpl;
                 c = c->next_sibling)
                prev_idx = node_index(ui, c);
        }

        const uint16_t total = (uint16_t)(old_n - sub0);
        QLUINodeRT *arr =
            (QLUINodeRT *)ql_mem_alloc((size_t)total * sizeof(QLUINodeRT));
        if (arr == NULL)
            return QL_ERR_NOMEM;
        memcpy(arr, old, (size_t)ti * sizeof(QLUINodeRT));
        memcpy(arr + ti, old + te0, (size_t)(old_n - te0) * sizeof(QLUINodeRT));
        for (uint16_t i = 0; i < total; i++) {
            QLUINodeRT *n = &arr[i];
            if (n->parent != NULL) {
                const uint16_t k = (uint16_t)(n->parent - old);
                n->parent = (k >= te0) ? &arr[(uint16_t)(k - sub0)] : &arr[k];
            }
            if (n->first_child != NULL) {
                const uint16_t k = (uint16_t)(n->first_child - old);
                n->first_child = (k >= te0) ? &arr[(uint16_t)(k - sub0)] : &arr[k];
            }
            if (n->next_sibling != NULL) {
                const uint16_t k = (uint16_t)(n->next_sibling - old);
                n->next_sibling = (k >= te0) ? &arr[(uint16_t)(k - sub0)] : &arr[k];
            }
        }
        /*
         * 接链：`prev` / `parent` 的下标都 < ti，所以在新区里位置不变。
         * 模板的 next_sibling 一定在子树之外（`te0` 之后），换算后指向同一根节点。
         */
        QLUINodeRT *after =
            (after_idx == 0xFFFFu) ? NULL
                                   : &arr[(uint16_t)(after_idx - sub0)];
        if (prev_idx != 0xFFFFu)
            arr[prev_idx].next_sibling = after;
        else if (parent_idx != 0xFFFFu)
            arr[parent_idx].first_child = after;

        ui->nodes = arr;
        ui->node_count = total;
        ql_mem_free(old);
        return QL_OK;
    }

    const uint16_t te = te0;
    const uint16_t sub = sub0;

    const uint32_t extra = (uint32_t)(count - 1u) * (uint32_t)sub;
    if ((uint32_t)ui->node_count + extra > 0xFFFFu)
        return QL_ERR_FULL;

    QLUINodeRT *old = ui->nodes;
    const uint16_t old_n = ui->node_count;
    const uint16_t parent_idx = node_index(ui, tpl->parent);

    const uint32_t total = (uint32_t)old_n + extra;
    QLUINodeRT *arr = (QLUINodeRT *)ql_mem_alloc((size_t)total * sizeof(QLUINodeRT));
    if (arr == NULL)
        return QL_ERR_NOMEM;
    memcpy(arr, old, (size_t)old_n * sizeof(QLUINodeRT));

    /* 指针重定位：旧指针都指向 old 内部，换成新基址 + 同一下标 */
    for (uint16_t i = 0; i < old_n; i++) {
        QLUINodeRT *n = &arr[i];
        if (n->parent != NULL)
            n->parent = &arr[n->parent - old];
        if (n->first_child != NULL)
            n->first_child = &arr[n->first_child - old];
        if (n->next_sibling != NULL)
            n->next_sibling = &arr[n->next_sibling - old];
    }
    ql_mem_free(old);                    /* 旧数组只用于上面的指针换算 */
    ui->nodes = arr;                     /* 之后所有下标换算都以新数组为基 */
    ui->node_count = (uint16_t)total;

    /*
     * 逐份克隆：模板子树内部的下标一律映射到 base + (j - ti)。
     * 接链位置是**模板自己的位置**（不是子树末端）：一份份依次插在它后面，
     * 于是列表项挨着长，模板后面的兄弟（比如"总计"那一行）还在最后。
     */
    QLUINodeRT *tail = &arr[ti];
    uint16_t base = old_n;
    for (uint16_t c = 1; c < count; c++) {
        for (uint16_t j = ti; j < te; j++) {
            const uint16_t dj = (uint16_t)(base + (uint16_t)(j - ti));
            QLUINodeRT *dst = &arr[dj];
            *dst = arr[j];

            /* 父节点：子树内部 → 映射；子树外面 → 原样 */
            if (j == ti) {
                dst->parent = (parent_idx != 0xFFFFu) ? &arr[parent_idx] : NULL;
            } else {
                const uint16_t pid = node_index(ui, arr[j].parent);
                dst->parent = (pid >= ti && pid < te)
                                  ? &arr[base + (uint16_t)(pid - ti)]
                                  : ((pid != 0xFFFFu) ? &arr[pid] : NULL);
            }
            /* 子 / 兄弟：只有落在子树内的才重映射 */
            if (arr[j].first_child != NULL) {
                const uint16_t cid = node_index(ui, arr[j].first_child);
                dst->first_child = (cid >= ti && cid < te)
                                       ? &arr[base + (uint16_t)(cid - ti)]
                                       : &arr[cid];
            } else {
                dst->first_child = NULL;
            }
            if (arr[j].next_sibling != NULL) {
                const uint16_t sid = node_index(ui, arr[j].next_sibling);
                dst->next_sibling = (sid >= ti && sid < te)
                                        ? &arr[base + (uint16_t)(sid - ti)]
                                        : &arr[sid];
            } else {
                dst->next_sibling = NULL;
            }

            /* 运行期状态不复制（滚动位置、脏标记、状态变体下标都要重算） */
            dst->used_copy = 0;
            dst->style_copy = NULL;
            dst->flags = 0;
            dst->m_hover = 0xFFFF;
            dst->m_active = 0xFFFF;
            dst->m_focus = 0xFFFF;
            dst->col_n = 0;
            dst->col_w = 0;
            if (arr[j].style_copy != NULL) {
                /* 模板自己改过样式：副本要独立一份，否则共享会串味 */
                QLUIStyle *cp = (QLUIStyle *)ql_mem_alloc(sizeof(QLUIStyle));
                if (cp == NULL) {
                    /* 半成品：把新数组还给旧数组，恢复原状 */
                    ql_mem_free(arr);
                    ui->nodes = old;
                    ui->node_count = old_n;
                    return QL_ERR_NOMEM;
                }
                *cp = *arr[j].style_copy;
                dst->style_copy = cp;
                dst->style = cp;
                dst->used_copy = 1;
            } else {
                dst->style = arr[j].style;
            }
        }
        /* 接进兄弟链：这一份的根插到 tail（上一份的根 / 模板自己）后面 */
        arr[base].next_sibling = tail->next_sibling;
        tail->next_sibling = &arr[base];
        tail = &arr[base];
        base = (uint16_t)(base + sub);
    }

    /* 文本令牌替换：模板那份 {i} = 1，第 c 份 = c（从 1 开始数） */
    {
        uint16_t b = ti;
        for (uint16_t c = 1; c <= count; c++) {
            for (uint16_t j = 0; j < sub; j++) {
                QLUINodeRT *n = &ui->nodes[(uint16_t)(b + j)];
                char *rep = rep_subst(ui, n->text, c);
                if (rep != NULL)
                    n->text = rep;
            }
            b = (uint16_t)(b + sub);
        }
    }
    return QL_OK;
}

QLUINodeRT *ql_ui_root_node(QLUIRoot *ui)
{
    return (ui != NULL && ui->node_count > 0) ? &ui->nodes[0] : NULL;
}

/* ------------------------------ 布局 ------------------------------ */

/* 布局期间的视口尺寸（fixed 定位用；单线程，布局期间有效） */
static ql_coord_t s_viewport_w;
static ql_coord_t s_viewport_h;

/*
 * `calc()` 的字节码池：布局期间有效（与 `s_viewport_w/h` 同一个生命周期）。
 * 样式里只存下标，所以求值必须能拿到当前文档的池。
 */
static const QLUICalcOp *s_calc_ops;
static uint16_t s_calc_op_count;

#define QL_CALC_STACK_MAX 16

static ql_coord_t calc_term_px(const QLUICalcOp *op, ql_coord_t basis,
                               ql_coord_t fallback)
{
    switch (op->unit) {
    case QL_UI_UNIT_PX:
        return (ql_coord_t)op->value;
    case QL_UI_UNIT_PERCENT:
        return (ql_coord_t)(((int32_t)basis * (int32_t)op->value) / 100);
    case QL_UI_UNIT_VW:
        return (ql_coord_t)(((int32_t)s_viewport_w * (int32_t)op->value) / 100);
    case QL_UI_UNIT_VH:
        return (ql_coord_t)(((int32_t)s_viewport_h * (int32_t)op->value) / 100);
    case QL_UI_UNIT_VMIN: {
        const ql_coord_t m = (s_viewport_w < s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)op->value) / 100);
    }
    case QL_UI_UNIT_VMAX: {
        const ql_coord_t m = (s_viewport_w > s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)op->value) / 100);
    }
    default:
        return fallback;
    }
}

/* 求值 `QL_UI_UNIT_CALC`：后缀字节码，成功时结果压栈到 1 个元素 */
static ql_coord_t calc_eval(uint16_t start, ql_coord_t basis, ql_coord_t fallback)
{
    ql_coord_t st[QL_CALC_STACK_MAX];
    int sp = 0;
    if (s_calc_ops == NULL || start >= s_calc_op_count)
        return fallback;
    for (uint16_t i = start; i < s_calc_op_count; i++) {
        const QLUICalcOp *op = &s_calc_ops[i];
        switch (op->op) {
        case QL_UI_CALC_TERM:
            if (sp >= QL_CALC_STACK_MAX)
                return fallback;
            st[sp++] = calc_term_px(op, basis, fallback);
            break;
        case QL_UI_CALC_ADD:
        case QL_UI_CALC_SUB:
            if (sp < 2)
                return fallback;
            st[sp - 2] = (op->op == QL_UI_CALC_ADD) ? (st[sp - 2] + st[sp - 1])
                                                    : (st[sp - 2] - st[sp - 1]);
            sp--;
            break;
        case QL_UI_CALC_MIN:
        case QL_UI_CALC_MAX: {
            const int n = (int)op->unit;
            if (n < 1 || sp < n)
                return fallback;
            ql_coord_t v = st[sp - n];
            for (int k = 1; k < n; k++) {
                const ql_coord_t c = st[sp - n + k];
                if (op->op == QL_UI_CALC_MIN) {
                    if (c < v)
                        v = c;
                } else if (c > v) {
                    v = c;
                }
            }
            sp -= n;
            st[sp++] = v;
            break;
        }
        case QL_UI_CALC_CLAMP:
            if (sp < 3)
                return fallback;
            {
                ql_coord_t v = st[sp - 2];
                if (v < st[sp - 3])
                    v = st[sp - 3];
                if (v > st[sp - 1])
                    v = st[sp - 1];
                sp -= 3;
                st[sp++] = v;
            }
            break;
        case QL_UI_CALC_END:
            return (sp == 1) ? st[0] : fallback;
        default:
            return fallback;
        }
    }
    return fallback;
}

/* 表达式里是否用到百分比（百分比高度在父级高度不定时要退成 auto） */
static int calc_len_uses_percent(QLUILength len)
{
    if (len.unit != QL_UI_UNIT_CALC)
        return 0;
    if (s_calc_ops == NULL || len.value < 0 || len.value >= s_calc_op_count)
        return 0;
    for (uint16_t i = (uint16_t)len.value; i < s_calc_op_count; i++) {
        const QLUICalcOp *op = &s_calc_ops[i];
        if (op->op == QL_UI_CALC_END)
            break;
        if (op->op == QL_UI_CALC_TERM && op->unit == QL_UI_UNIT_PERCENT)
            return 1;
    }
    return 0;
}

static ql_coord_t resolve_length(QLUILength len, ql_coord_t basis, ql_coord_t fallback)
{
    switch (len.unit) {
    case QL_UI_UNIT_PX:
        return (ql_coord_t)len.value;
    case QL_UI_UNIT_PERCENT:
        return (ql_coord_t)(((int32_t)basis * (int32_t)len.value) / 100);
    /*
     * 视口单位：只有布局期才知道视口多大，所以留到这里解析
     * （`em` / `rem` 已经在构建期折成 px 了）。
     */
    case QL_UI_UNIT_VW:
        return (ql_coord_t)(((int32_t)s_viewport_w * (int32_t)len.value) / 100);
    case QL_UI_UNIT_VH:
        return (ql_coord_t)(((int32_t)s_viewport_h * (int32_t)len.value) / 100);
    case QL_UI_UNIT_VMIN: {
        const ql_coord_t m = (s_viewport_w < s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)len.value) / 100);
    }
    case QL_UI_UNIT_VMAX: {
        const ql_coord_t m = (s_viewport_w > s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)len.value) / 100);
    }
    case QL_UI_UNIT_CALC:
        return calc_eval((uint16_t)len.value, basis, fallback);
    default:
        return fallback;
    }
}

/*
 * `height` 是否"确定"（有具体像素值）。
 *
 * CSS 语义：百分比高度只有在**父级高度确定**时才能解析；父级是 auto 时
 * 百分比按 `auto` 处理。之前这里一律当成确定，于是 `%` 乘以基准 0 得到 0 ——
 * 表现就是"套一层 `height:100%` 的 div 变成 0 高，里面全空"。
 */
static int height_is_definite(const QLUIStyle *s, ql_coord_t basis_h)
{
    if (s->height.unit == QL_UI_UNIT_PX || s->height.unit == QL_UI_UNIT_VW ||
        s->height.unit == QL_UI_UNIT_VH || s->height.unit == QL_UI_UNIT_VMIN ||
        s->height.unit == QL_UI_UNIT_VMAX)
        return 1;
    if (s->height.unit == QL_UI_UNIT_PERCENT)
        return basis_h >= 0;
    if (s->height.unit == QL_UI_UNIT_CALC)
        return !calc_len_uses_percent(s->height) || basis_h >= 0;
    return 0;
}

/* 解析 `height`：`basis_h < 0` 表示父级高度不确定 → 百分比退回 fallback */
static ql_coord_t height_of_style(const QLUIStyle *s, ql_coord_t basis_h,
                                  ql_coord_t fallback)
{
    if (s->height.unit == QL_UI_UNIT_PERCENT && basis_h < 0)
        return fallback;
    if (s->height.unit == QL_UI_UNIT_CALC && basis_h < 0 &&
        calc_len_uses_percent(s->height))
        return fallback;
    return resolve_length(s->height, basis_h > 0 ? basis_h : 0, fallback);
}

/* 样式 → 字体：字号来自生成期的 font_sizes 表，具体字体由字体系统按字号给 */
/* ---------------------- 文本栈（B8）：家族列表 → 回退链 ---------------------- */

#if QL_FEAT_CSS_FONT_STACK
/* 一条回退链的缓存槽（字号 / 列表 / 字重 / 斜体 决定一条链） */
#define QL_FONT_CHAIN_SLOTS  16
typedef struct {
    const QLUIDoc *doc;
    uint16_t list;
    uint16_t px;
    uint8_t  bold;
    uint8_t  italic;
    QLFont  *head;      /* 链头（每节都是一个包装体，最后一节的 fallback 是默认字体） */
} FontChainSlot;
static FontChainSlot s_chains[QL_FONT_CHAIN_SLOTS];
static int s_chain_next;

/*
 * 放掉一条链：**只放包装体**。
 * 链尾是注册表里的字体（或内置位图字体）：注册表自己的缓存由
 * `ql_font_family_reset()` 负责，这里动了它就会 double free + 打乱别人的缓存。
 */
static void chain_free(QLFont *head)
{
    QLFont *w = head;
    while (w != NULL && w->is_chain_wrapper) {
        QLFont *next = (QLFont *)(uintptr_t)w->fallback_font;
        ql_font_destroy(w);
        w = next;
    }
}

static void font_chains_reset(void)
{
    for (int i = 0; i < QL_FONT_CHAIN_SLOTS; i++) {
        chain_free(s_chains[i].head);
        memset(&s_chains[i], 0, sizeof(s_chains[i]));
    }
    s_chain_next = 0;
}

/* 文本池里的字符串（家族名 / 资源名） */
static const char *doc_pool_str(const QLUIDoc *doc, uint16_t idx)
{
    if (doc == NULL || doc->text_pool == NULL || doc->text_offsets == NULL)
        return NULL;
    return doc->text_pool + doc->text_offsets[idx];
}

/* 通用家族名（CSS 关键字）落到本库的"正文 / 等宽" */
static const QLFont *family_generic(const char *name, int px)
{
    if (strcmp(name, "monospace") == 0)
        return ql_font_mono(px);
    if (strcmp(name, "sans-serif") == 0 || strcmp(name, "serif") == 0 ||
        strcmp(name, "system-ui") == 0 || strcmp(name, "cursive") == 0 ||
        strcmp(name, "fantasy") == 0)
        return ql_font_default(px);
    return NULL;
}

static const QLFont *family_resolve(const char *name, int weight, int italic, int px)
{
    const QLFont *f = family_generic(name, px);
    if (f != NULL)
        return f;
    const int id = ql_font_family_id(name);
    if (id > 0)
        return ql_font_family_font(id, weight, italic, px);
    return NULL;                  /* 这台机器上没有这个家族：继续往后找 */
}

/*
 * 把一个已注册的字体包成"链节点"。
 *
 * 为什么要包装：`QLFont.fallback_font` 是**字体自己的**字段，而注册表里的字体是
 * 按 (家族, 字号) 共享的 —— 同一张字面可能被两条不同的链（`A, monospace` 与
 * `A, B`）用到。直接改共享字体的 fallback 会让两条链互相污染。
 * 包装体只"借"后端与 impl，字形缓存是自己的（互不干扰），销毁时只放自己的部分。
 */
static QLFont *chain_wrap(const QLFont *src, const QLFont *tail, uint16_t list)
{
    if (src == NULL)
        return NULL;
    QLFont *w = (QLFont *)ql_mem_calloc(1, sizeof(QLFont));
    if (w == NULL)
        return NULL;
    w->name = src->name;
    w->bpp = src->bpp;
    w->line_height = src->line_height;
    w->ascent = src->ascent;
    w->descent = src->descent;
    w->fallback = src->fallback;
    w->glyphs = NULL;
    w->glyph_count = 0;
    w->glyph_capacity = 0;
    w->bitmap = NULL;
    w->glyph_bitmaps = NULL;
    w->backend = src->backend;
    w->impl = src->impl;
    w->pixel_size = src->pixel_size;
    w->is_bold = src->is_bold;
    w->is_italic = src->is_italic;
    w->family_id = src->family_id;
    w->list_id = list;
    w->is_chain_wrapper = 1;
    w->fallback_font = tail;
    return w;
}

/* 按 (doc, 列表, 字号, 字重, 斜体) 取（并缓存）一条回退链的链头 */
static const QLFont *font_chain_get(const QLUIDoc *doc, int list, int px, int weight,
                                    int italic)
{
    const uint8_t bold = (uint8_t)((weight >= 600) ? 1 : 0);
    const uint8_t ital = (uint8_t)(italic ? 1 : 0);
    for (int i = 0; i < QL_FONT_CHAIN_SLOTS; i++) {
        const FontChainSlot *c = &s_chains[i];
        if (c->head != NULL && c->doc == doc && c->list == (uint16_t)list &&
            c->px == (uint16_t)px && c->bold == bold && c->italic == ital)
            return c->head;
    }

    if (doc == NULL || doc->font_lists == NULL || doc->font_names == NULL ||
        list <= 0 || list >= (int)doc->font_list_count)
        return NULL;

    const QLUIFontList *fl = &doc->font_lists[list];
    const int n = (int)fl->count;
    if (n <= 0)
        return NULL;

    /*
     * 从后往前串：最后一个家族的回退是"默认字体"。
     *
     * 三种情况不进链：
     *   - 家族没注册（这台机器上没有）→ 跳过，就是 CSS 的 fallback；
     *   - 解析结果与链尾是同一个字体（例如 `sans-serif` 就是默认字体）→ 跳过，
     *     否则会包出一个"自己回退到自己"的节点；
     *   - 静态位图字体（`backend == NULL`）→ 它自己不需要包装，直接当链尾
     *     （`ql_font_find_glyph` 的回退链对静态字体同样有效）。
     */
    const QLFont *def = ql_font_default(px);
    const QLFont *tail = def;
    for (int k = n - 1; k >= 0; k--) {
        const uint16_t ni = (uint16_t)(fl->first + k);
        if (ni >= doc->font_name_count)
            continue;
        const char *nm = doc_pool_str(doc, doc->font_names[ni]);
        if (nm == NULL || nm[0] == '\0')
            continue;
        const QLFont *f = family_resolve(nm, weight, italic, px);
        if (f == NULL || f == tail)
            continue;
        if (f->backend == NULL) {
            tail = f;
            continue;
        }
        const QLFont *w = chain_wrap(f, tail, (uint16_t)list);
        if (w != NULL)
            tail = w;
    }

    /* 一个家族都没装上：直接用默认字体（但要把列表下标记住，DPR 缩放才有依据） */
    if (tail == def)
        return tail;

    FontChainSlot *slot = &s_chains[s_chain_next];
    s_chain_next = (s_chain_next + 1) % QL_FONT_CHAIN_SLOTS;
    chain_free(slot->head);
    slot->doc = doc;
    slot->list = (uint16_t)list;
    slot->px = (uint16_t)px;
    slot->bold = bold;
    slot->italic = ital;
    slot->head = (QLFont *)(uintptr_t)tail;
    return tail;
}

/* DPR 缩放钩子：按同一个家族列表重取（见 ql_font_set_scale_hook） */

#endif /* QL_FEAT_CSS_FONT_STACK */

/* 文档里的家族列表解析出的字体（含逐字回退链） */
static const QLFont *font_of_style(const QLUINodeRT *node, const QLUIStyle *style)
{
    int size = 0;
    const QLUIDoc *doc = (node != NULL && node->owner != NULL) ? node->owner->doc : NULL;
    if (doc != NULL) {
        if (doc->font_sizes != NULL && style->font_id < doc->font_size_count)
            size = (int)doc->font_sizes[style->font_id];
    }
    const int px = (size > 0) ? size : 16;

#if QL_FEAT_CSS_FONT_STACK
    if (doc != NULL && style->font_family != 0) {
        const int weight = ((style->font_flags & QL_UI_FONT_BOLD) != 0) ? 700 : 400;
        const int italic = ((style->font_flags & QL_UI_FONT_ITALIC) != 0) ? 1 : 0;
        const QLFont *f = font_chain_get(doc, (int)style->font_family, px, weight, italic);
        if (f != NULL)
            return f;
    }
#endif
    return (style->font_family == QL_UI_FAMILY_MONO) ? ql_font_mono(px)
                                                    : ql_font_default(px);
}

#if QL_FEAT_CSS_FONT_STACK
/* 缩放钩子的实现：认识列表号就用列表重取，否则交给默认工厂 */
static const QLFont *lbl_scale_hook(const QLFont *font, int px)
{
    if (font == NULL)
        return NULL;
    if (font->list_id != 0 && s_last_doc != NULL) {
        const int weight = font->is_bold ? 700 : 400;
        const QLFont *f =
            font_chain_get(s_last_doc, (int)font->list_id, px, weight, font->is_italic ? 1 : 0);
        if (f != NULL && f->backend != NULL)
            return f;
    }
    return NULL;      /* 让 ql_font_scale 走它自己的默认分支 */
}

static void fonts_register_from_doc(QLUIRoot *ui)
{
    const QLUIDoc *doc = (ui != NULL) ? ui->doc : NULL;
    /*
     * 换了文档才重建注册表：注册表是进程级的，同一个 doc 反复 build 时
     * 重复注册只会白读一遍字体文件。
     */
    if (s_last_doc != doc)
        ql_font_family_reset();
    s_last_doc = (QLUIDoc *)doc;
    if (doc == NULL || doc->font_faces == NULL)
        return;
    for (uint16_t i = 0; i < doc->font_face_count; i++) {
        const QLUIFontFace *fc = &doc->font_faces[i];
        const char *family = doc_pool_str(doc, fc->family);
        const char *src = doc_pool_str(doc, fc->src);
        if (family == NULL || src == NULL)
            continue;
        (void)ql_font_family_register(family, src, (int)fc->weight, (int)fc->italic);
    }
}
#endif


static ql_coord_t inner_width_of(const QLUIStyle *style, ql_coord_t content_w)
{
    return content_w - style->padding.left - style->padding.right -
           ql_ui_border_h(style);
}

static ql_coord_t outer_width_of(const QLUINodeRT *node)
{
    const QLUIStyle *s = node->style;
    return node->border_box.w + s->margin.left + s->margin.right;
}

static ql_coord_t line_height_of(const QLFont *font)
{
    return font->line_height;
}

/* 文本行高：样式写了 line-height 就用它，否则用字体自己的（= normal） */
/* 折行用的可调项（字间距 / 首行缩进 / word-break）：布局与绘制必须同一份 */
static QLTextWrapOpt wrap_opt_of(const QLUIStyle *s)
{
    QLTextWrapOpt o;
    o.letter_spacing = (ql_coord_t)s->letter_spacing;
    o.word_spacing = (ql_coord_t)s->word_spacing;
    o.indent = (ql_coord_t)s->text_indent;
    o.break_any = (uint8_t)(s->word_break == QL_UI_WORDBREAK_ALL);
    o.tab_size = s->tab_size;
    return o;
}

static ql_coord_t text_line_h_of(const QLUIStyle *s, const QLFont *font)
{
    return (s->line_h > 0) ? s->line_h : line_height_of(font);
}

/*
 * `min-width` / `max-width` 的夹紧。返回夹紧后的**同一个度量**
 * （`is_border_box` 决定外面传进来的数是内容宽还是边框盒宽）。
 * 没设时 `unit == AUTO`，直接原值返回。
 */
static ql_coord_t clamp_len(QLUILength lo, QLUILength hi, ql_coord_t v,
                            ql_coord_t basis)
{
    if (lo.unit != QL_UI_UNIT_AUTO) {
        const ql_coord_t m = resolve_length(lo, basis, v);
        if (v < m)
            v = m;
    }
    if (hi.unit != QL_UI_UNIT_AUTO) {
        const ql_coord_t m = resolve_length(hi, basis, v);
        if (m >= 0 && v > m)
            v = m;
    }
    return v;
}

/* 计算节点的边框盒宽度（与 layout_node 内部逻辑保持一致） */
static ql_coord_t border_width_of(const QLUINodeRT *node, ql_coord_t avail_w)
{
    const QLUIStyle *s = node->style;

    if (s->display == QL_UI_DISPLAY_NONE)
        return 0;

    /*
     * <img>：CSS 没给宽度时用图片的固有宽度（inline-block 的 shrink-to-fit）。
     * 必须在"文本尺寸"判断之前 —— 否则会把资源名的宽度当成图片宽度。
     */
    if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG &&
        s->width.unit == QL_UI_UNIT_AUTO) {
        ql_coord_t iw = 0, ih = 0;
        image_node_size(node->owner, node, &iw, &ih);
        return iw + s->padding.left + s->padding.right + ql_ui_border_h(s);
    }

    const QLFont *font = font_of_style(node, s);
    /*
     * 表格角色（cell / caption / row / row-group）一律**不用文本撑宽**：
     * 它们的宽度由列宽（或表格内容宽）决定，与浏览器一致。
     */
    const int table_role = s->display >= QL_UI_DISPLAY_TABLE;
    int text_sized = s->display != QL_UI_DISPLAY_BLOCK && !table_role &&
                     s->width.unit == QL_UI_UNIT_AUTO &&
                     has_shown_text(node) && node->first_child == NULL;

    if (text_sized)
        return ql_font_line_width_ls2(font, node->text, -1,
                                      (ql_coord_t)s->letter_spacing,
                                      (ql_coord_t)s->word_spacing, s->tab_size) +
               s->padding.left + s->padding.right + ql_ui_border_h(s);

    if (s->width.unit == QL_UI_UNIT_AUTO) {
        ql_coord_t w = avail_w - s->margin.left - s->margin.right;
        return (w > 0) ? w : 0;
    }

    ql_coord_t v = resolve_length(s->width, avail_w, avail_w);
    if (s->box_sizing == 1)
        return clamp_len(s->min_width, s->max_width, v, avail_w);

    /*
     * content-box：min/max 指**内容宽**，所以先夹内容宽再加 padding/border。
     * （border-box 时它们指边框盒，直接夹总量 —— 上面那条 return。）
     */
    v = clamp_len(s->min_width, s->max_width, v, avail_w);
    return v + s->padding.left + s->padding.right + ql_ui_border_h(s);
}


/* ------------------------------ flexbox ------------------------------ */

static int is_flex_container(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_FLEX || s->display == QL_UI_DISPLAY_INLINE_FLEX;
}

static int flex_is_row(const QLUIStyle *s)
{
    return s->flex_direction == QL_UI_FLEX_ROW ||
           s->flex_direction == QL_UI_FLEX_ROW_REVERSE;
}

static int flex_reversed(const QLUIStyle *s)
{
    return s->flex_direction == QL_UI_FLEX_ROW_REVERSE ||
           s->flex_direction == QL_UI_FLEX_COLUMN_REVERSE;
}

/*
 * 拿到节点的可写样式副本（懒分配）。
 *
 * 为什么按需分配：`QLUIStyle` 现在 400 字节，而绝大多数节点从不需要运行期改样式
 * （静态界面里这个指针恒为 NULL）。按需分配把 `QLUINodeRT` 从"人人 400 字节"
 * 变成"改过的节点才 400 字节"，MCU 上省下的就是实打实的 .bss。
 */
static QLUIStyle *ensure_style_copy(QLUINodeRT *node)
{
    if (node->style_copy != NULL)
        return node->style_copy;
    QLUIStyle *cp = (QLUIStyle *)ql_mem_alloc(sizeof(QLUIStyle));
    if (cp == NULL)
        return NULL;
    *cp = *node->style;
    node->style_copy = cp;
    node->style = cp;
    node->used_copy = 1;
    return cp;
}

/* 给节点写一个"强制主/交叉尺寸"，走 style_copy（border-box 语义） */
static void force_size(QLUINodeRT *node, ql_coord_t main_sz, int row, int apply_main,
                       ql_coord_t cross_sz, int apply_cross)
{
    if (!apply_main && !apply_cross)
        return;

    if (ensure_style_copy(node) == NULL)
        return;

    QLUIStyle *s = node->style_copy;
    s->box_sizing = 1;          /* 我们给的是边框盒尺寸 */
    if (row) {
        if (apply_main) {
            s->width.value = (int16_t)((main_sz < 0) ? 0 : main_sz);
            s->width.unit = QL_UI_UNIT_PX;
        }
        if (apply_cross) {
            s->height.value = (int16_t)((cross_sz < 0) ? 0 : cross_sz);
            s->height.unit = QL_UI_UNIT_PX;
        }
    } else {
        if (apply_main) {
            s->height.value = (int16_t)((main_sz < 0) ? 0 : main_sz);
            s->height.unit = QL_UI_UNIT_PX;
        }
        if (apply_cross) {
            s->width.value = (int16_t)((cross_sz < 0) ? 0 : cross_sz);
            s->width.unit = QL_UI_UNIT_PX;
        }
    }
}

/* 该节点的"自然主尺寸"（含 margin），由测量结果推出 */
static ql_coord_t natural_main(const QLUINodeRT *node, int row)
{
    const QLUIStyle *s = node->style;
    return row ? (node->border_box.w + s->margin.left + s->margin.right)
               : (node->border_box.h + s->margin.top + s->margin.bottom);
}

/* 交叉方向的外尺寸（含 margin） */
static ql_coord_t natural_cross(const QLUINodeRT *node, int row)
{
    const QLUIStyle *s = node->style;
    return row ? (node->border_box.h + s->margin.top + s->margin.bottom)
               : (node->border_box.w + s->margin.left + s->margin.right);
}

/*
 * 前向声明：flex 的测量阶段要递归调 layout_node，而 layout_node 的 flex 分支
 * 又要调 layout_flex_children —— 两者互相依赖。
 */
static ql_coord_t layout_node(QLUINodeRT *node, ql_coord_t x, ql_coord_t y,
                              ql_coord_t avail_w, ql_coord_t basis_h);
static ql_coord_t layout_flex_children(QLUINodeRT *node, ql_coord_t content_x,
                                       ql_coord_t content_y, ql_coord_t content_w,
                                       ql_coord_t avail_h, int definite_h);
static ql_coord_t layout_grid_children(QLUIRoot *ui, QLUINodeRT *node,
                                       ql_coord_t content_x, ql_coord_t content_y,
                                       ql_coord_t content_w, ql_coord_t avail_h,
                                       int definite_h);
static ql_coord_t layout_columns_children(QLUINodeRT *node, ql_coord_t content_x,
                                          ql_coord_t content_y, ql_coord_t content_w,
                                          ql_coord_t decl_h);
static int is_multicol(const QLUIStyle *s);

/* 父节点的内容宽/高（relative 定位的百分比基准） */
static ql_coord_t parent_w_hint(const QLUINodeRT *node)
{
    if (node->parent == NULL)
        return s_viewport_w;
    return node->parent->content_box.w;
}

/* ------------------------------ 定位 ------------------------------ */

static int is_out_of_flow(const QLUIStyle *s)
{
    return s->position == QL_UI_POSITION_ABSOLUTE || s->position == QL_UI_POSITION_FIXED;
}

/* 把一个节点的整棵子树整体平移（relative 定位用） */
static void translate_subtree(QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy)
{
    if (dx == 0 && dy == 0)
        return;
    node->border_box.x += dx;
    node->border_box.y += dy;
    node->content_box.x += dx;
    node->content_box.y += dy;
    node->abs_cb.x += dx;
    node->abs_cb.y += dy;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling)
        translate_subtree(c, dx, dy);
}

/* 解析一条 inset，auto 时返回 fallback */
static ql_coord_t resolve_inset(QLUILength len, ql_coord_t basis, ql_coord_t fallback)
{
    switch (len.unit) {
    case QL_UI_UNIT_PX:
        return (ql_coord_t)len.value;
    case QL_UI_UNIT_PERCENT:      /* 相对包含块 */
        return (ql_coord_t)(((int32_t)basis * (int32_t)len.value) / 100);
    case QL_UI_UNIT_VW:
        return (ql_coord_t)(((int32_t)s_viewport_w * (int32_t)len.value) / 100);
    case QL_UI_UNIT_VH:
        return (ql_coord_t)(((int32_t)s_viewport_h * (int32_t)len.value) / 100);
    case QL_UI_UNIT_VMIN: {
        const ql_coord_t m = (s_viewport_w < s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)len.value) / 100);
    }
    case QL_UI_UNIT_VMAX: {
        const ql_coord_t m = (s_viewport_w > s_viewport_h) ? s_viewport_w : s_viewport_h;
        return (ql_coord_t)(((int32_t)m * (int32_t)len.value) / 100);
    }
    case QL_UI_UNIT_CALC:
        return calc_eval((uint16_t)len.value, basis, fallback);
    default:
        return fallback;
    }
}

/*
 * 给绝对/固定定位的子节点定位。
 * cb 是包含块（绝对定位 = 最近定位祖先的内容盒；固定定位 = 视口）。
 * 顺序：top/bottom 决定 y，left/right 决定 x；两者都给且尺寸 auto 时拉伸。
 */
static void layout_absolute(QLUINodeRT *child, QLRect cb, ql_coord_t viewport_w)
{
    const QLUIStyle *s = child->style;
    if (s->display == QL_UI_DISPLAY_NONE)
        return;

    const QLUILength *top = &s->inset[0];
    const QLUILength *right = &s->inset[1];
    const QLUILength *bottom = &s->inset[2];
    const QLUILength *left = &s->inset[3];

    int has_l = left->unit != QL_UI_UNIT_AUTO;
    int has_r = right->unit != QL_UI_UNIT_AUTO;
    int has_t = top->unit != QL_UI_UNIT_AUTO;
    int has_b = bottom->unit != QL_UI_UNIT_AUTO;

    ql_coord_t l = resolve_inset(*left, cb.w, 0);
    ql_coord_t r = resolve_inset(*right, cb.w, 0);
    ql_coord_t t = resolve_inset(*top, cb.h, 0);
    ql_coord_t b = resolve_inset(*bottom, cb.h, 0);

    /*
     * 宽度按这个优先级决定：
     *   1. 显式 width
     *   2. left + right 都给 → 拉伸
     *   3. auto → shrink-to-fit（只支持"内容是本节点自己的文本"）
     *   4. 其余 → 撑满包含块（已在手册里写明）
     */
    ql_coord_t avail_w = (cb.w > 0) ? cb.w : viewport_w;
    ql_coord_t own_w = -1;      /* >= 0 表示宽度已定 */

    if (s->width.unit != QL_UI_UNIT_AUTO) {
        own_w = border_width_of(child, cb.w);
    } else if (has_l && has_r) {
        ql_coord_t stretch = cb.w - l - r;
        own_w = (stretch < 0) ? 0 : stretch;
    } else {
        const QLFont *font = font_of_style(child, s);
        if (has_shown_text(child) && child->first_child == NULL && font != NULL) {
            ql_coord_t tw = ql_font_line_width_ls2(font, child->text, -1,
                                                   (ql_coord_t)s->letter_spacing,
                                                   (ql_coord_t)s->word_spacing,
                                                   s->tab_size) +
                            ql_ui_border_h(s) + s->padding.left + s->padding.right;
            ql_coord_t cap = cb.w - l - r;
            if (cap > 0 && tw > cap)
                tw = cap;
            own_w = tw;
        } else {
            own_w = cb.w;
        }
    }

    ql_coord_t x = cb.x + l;
    if (own_w >= 0) {
        if (ensure_style_copy(child) == NULL)
            return;
        child->style_copy->box_sizing = 1;
        child->style_copy->width.value = (int16_t)((own_w < 0) ? 0 : own_w);
        child->style_copy->width.unit = QL_UI_UNIT_PX;
        avail_w = own_w;
    }
    if (has_r && !has_l)
        x = cb.x + cb.w - r - own_w - (s->margin.left + s->margin.right);

    /* 高度同理：显式 height 优先，否则 top+bottom 拉伸 */
    int definite_h = (s->height.unit != QL_UI_UNIT_AUTO) || (has_t && has_b);
    if (definite_h && s->height.unit == QL_UI_UNIT_AUTO) {
        ql_coord_t stretch = cb.h - t - b;
        if (stretch < 0)
            stretch = 0;
        if (ensure_style_copy(child) == NULL)
            return;
        child->style_copy->box_sizing = 1;
        child->style_copy->height.value = (int16_t)stretch;
        child->style_copy->height.unit = QL_UI_UNIT_PX;
    }

    ql_coord_t y;
    if (has_t) {
        y = cb.y + t;
    } else if (has_b) {
        (void)layout_node(child, x, 0, avail_w, cb.h);
        y = cb.y + cb.h - b - child->border_box.h - (s->margin.top + s->margin.bottom);
    } else {
        y = cb.y;        /* 都没给：贴在包含块左上（CSS 的 static position 未实现） */
    }

    (void)layout_node(child, x, y, avail_w, cb.h);
}

/*
 * 按 z-index 稳定排序子链表。
 * 在布局末尾做一次，之后**绘制与命中测试都自动遵循 z 序**，不必各自排序。
 * 插入排序：子节点数通常 < 20，且布局不在渲染热路径上。
 */
static void sort_children_by_z(QLUINodeRT *parent)
{
    QLUINodeRT *sorted = NULL;      /* 已排序部分（头插，之后反转） */
    QLUINodeRT *cur = parent->first_child;

    while (cur != NULL) {
        QLUINodeRT *next = cur->next_sibling;
        int z = cur->style->z_index;

        QLUINodeRT *prev = NULL;
        QLUINodeRT *p = sorted;
        while (p != NULL && p->style->z_index <= z) {
            prev = p;
            p = p->next_sibling;
        }
        cur->next_sibling = p;
        if (prev == NULL) {
            /* 头插：记录在 sorted 前 */
            cur->next_sibling = sorted;
            sorted = cur;
        } else {
            prev->next_sibling = cur;
        }
        cur = next;
    }
    parent->first_child = sorted;
}

/* 递归对所有节点排序（布局结束后调一次） */
static void sort_tree_by_z(QLUINodeRT *node)
{
    sort_children_by_z(node);
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling)
        sort_tree_by_z(c);
}

/*
 * `position: sticky`：布局末尾的一次**补偿平移**。
 *
 * 语义：元素在流里占位（像 relative），但当最近的滚动容器滚到"它要出界"时，
 * 它贴住容器的可视边缘（`top` / `bottom` 给出贴边距离）。
 *
 * 为什么放在布局末尾而不是布局中：滚动是"整棵子树平移"（见 apply_scroll），
 * 平移之后才谈得上"有没有出界"。补偿平移改的是 border_box，于是绘制、命中、
 * 脏区、paint_box 全都自动正确 —— 与 relative 用的是同一条机制
 * （translate_subtree），没有第二套坐标。
 *
 * 只在写了 inset 的方向上生效（CSS 也是这样：`position: sticky` 不给 `top`
 * 就跟 relative 没区别）。
 */
static void sticky_adjust(QLUINodeRT *node, const QLUINodeRT *anc)
{
    const QLUIStyle *s = node->style;
    const QLRect cb = (anc != NULL) ? anc->content_box
                                    : ql_rect_make(0, 0, s_viewport_w, s_viewport_h);

    const int has_t = s->inset[0].unit != QL_UI_UNIT_AUTO;
    const int has_r = s->inset[1].unit != QL_UI_UNIT_AUTO;
    const int has_b = s->inset[2].unit != QL_UI_UNIT_AUTO;
    const int has_l = s->inset[3].unit != QL_UI_UNIT_AUTO;
    if (!has_t && !has_r && !has_b && !has_l)
        return;

    ql_coord_t dx = 0, dy = 0;
    if (has_t) {
        const ql_coord_t t = resolve_inset(s->inset[0], cb.h, 0);
        const ql_coord_t want = cb.y + t;
        if (node->border_box.y < want)
            dy = want - node->border_box.y;
    }
    if (has_b) {
        const ql_coord_t b = resolve_inset(s->inset[2], cb.h, 0);
        const ql_coord_t limit =
            ql_rect_bottom(cb) - b - node->border_box.h;
        if (node->border_box.y + dy > limit &&
            (!has_t || limit >= cb.y + resolve_inset(s->inset[0], cb.h, 0)))
            dy = limit - node->border_box.y;
    }
    if (has_l) {
        const ql_coord_t l = resolve_inset(s->inset[3], cb.w, 0);
        const ql_coord_t want = cb.x + l;
        if (node->border_box.x < want)
            dx = want - node->border_box.x;
    }
    if (has_r) {
        const ql_coord_t r = resolve_inset(s->inset[1], cb.w, 0);
        const ql_coord_t limit = ql_rect_right(cb) - r - node->border_box.w;
        if (node->border_box.x + dx > limit &&
            (!has_l || limit >= cb.x + resolve_inset(s->inset[3], cb.w, 0)))
            dx = limit - node->border_box.x;
    }
    /*
     * 约束在自己的**包含块**（父内容盒）里：这就是 sticky "跟到头就一起滚走" 的来源。
     * 只在自己往正方向推的时候夹（dy > 0），否则会在父盒已经滚出视口时往回拽。
     */
    if (dy > 0 && node->parent != NULL) {
        const ql_coord_t limit =
            ql_rect_bottom(node->parent->content_box) - node->border_box.h;
        if (node->border_box.y + dy > limit)
            dy = limit - node->border_box.y;
    }
    if (dx > 0 && node->parent != NULL) {
        const ql_coord_t limit =
            ql_rect_right(node->parent->content_box) - node->border_box.w;
        if (node->border_box.x + dx > limit)
            dx = limit - node->border_box.x;
    }
    if (dx == 0 && dy == 0)
        return;
    translate_subtree(node, dx, dy);
}

static void sticky_walk(QLUINodeRT *node, QLUINodeRT *clip_anc)
{
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        const QLUIStyle *s = c->style;
        if (s->display == QL_UI_DISPLAY_NONE)
            continue;
        if (s->position == QL_UI_POSITION_STICKY)
            sticky_adjust(c, clip_anc);
        /* 自己是滚动容器 → 成为后代的参照 */
        QLUINodeRT *next_anc = clip_anc;
        if (s->overflow_x != QL_UI_OVERFLOW_VISIBLE ||
            s->overflow_y != QL_UI_OVERFLOW_VISIBLE)
            next_anc = c;
        sticky_walk(c, next_anc);
    }
}

/* ------------------------------ 输入框 ------------------------------ */


/* [0, off) 里有几个码点（密码框把每个码点画成一个圆点） */
static uint16_t cp_count(const char *text, uint16_t off)
{
    uint16_t k = 0;
    uint16_t i = 0;
    while (i < off) {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        i = (uint16_t)(i + (uint16_t)used);
        k++;
    }
    return k;
}

/*
 * 密码框的显示文本：每个码点换成一个圆点。
 * 返回的指针要么是 in->buf（普通框），要么是 tmp（调用方给的栈缓冲）。
 * caret 的横向位置也按这份显示文本算，否则光会飘。
 */
static const char *input_display_text(const struct QLUIInput *in, int password,
                                      char *tmp, size_t cap)
{
    if (!password)
        return in->buf;
    size_t n = 0;
    for (uint16_t i = 0; i < in->len && n + 3u < cap;) {
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(in->buf + i, &cp);
        if (used <= 0)
            break;
        memcpy(tmp + n, "\xE2\x80\xA2", 3u);   /* U+2022 BULLET */
        n += 3u;
        i = (uint16_t)(i + used);
    }
    tmp[n] = '\0';
    return tmp;
}

/* 像素 x → 最近的字符边界（往上/下取更近的那个） */
static uint16_t caret_at_px(const QLFont *font, const char *text, uint16_t len,
                            ql_coord_t x)
{
    uint16_t best = 0;
    ql_coord_t best_d = 0x7FFFFFFF;
    uint16_t i = 0;
    while (i <= len) {
        const ql_coord_t px = ql_font_line_width(font, text, (int32_t)i);
        const ql_coord_t d = (px > x) ? (px - x) : (x - px);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
        if (i == len)
            break;
        uint32_t cp = 0;
        const int32_t used = ql_utf8_decode(text + i, &cp);
        if (used <= 0)
            break;
        i = (uint16_t)(i + (uint16_t)used);
    }
    return best;
}

/* 光标前一个字符的起点（UTF-8 安全） */
static uint16_t prev_boundary(const char *text, uint16_t off)
{
    if (off == 0)
        return 0;
    uint16_t i = (uint16_t)(off - 1u);
    while (i > 0 && ((unsigned char)text[i] & 0xC0u) == 0x80u)
        i--;
    return i;
}

static uint16_t next_boundary(const char *text, uint16_t len, uint16_t off)
{
    if (off >= len)
        return len;
    uint32_t cp = 0;
    const int32_t used = ql_utf8_decode(text + off, &cp);
    if (used <= 0)
        return len;
    const uint16_t nx = (uint16_t)(off + (uint16_t)used);
    return (nx > len) ? len : nx;
}

/* ------------------------------ 多行光标计算 ------------------------------ */

/*
 * 把输入串按盒子宽折成视觉行（`\n` 是硬换行）。
 * `lines` 为 NULL 时只数行数。上限是 QL_CFG_TEXT_MAX_LINES。
 */
static int input_visual_lines(const QLFont *font, const struct QLUIInput *in,
                              ql_coord_t box_w, QLTextLine *lines, int max)
{
    /*
     * **尾随空格要保留**：输入框末尾的空格是真实内容，光标得跟着它走。
     * 不保留的话在末尾敲空格看起来"什么都没发生"（值其实插进去了）。
     */
    return ql_text_wrap_ex(font, in->buf, box_w, lines, max, 1);
}

/* 字节偏移落在第几个视觉行（lines 的 start 指向 buf 内部） */
static int line_index_of(const char *base, const QLTextLine *lines, int count,
                         uint16_t off)
{
    int li = 0;
    for (int i = 0; i < count; i++) {
        const uint16_t s = (uint16_t)(lines[i].start - base);
        if (s <= off)
            li = i;
        else
            break;
    }
    return li;
}

/* 光标在内容盒内的位置（相对 content_box 左上角，未减去滚动） */
static void caret_px(const QLFont *font, const struct QLUIInput *in,
                     const QLTextLine *lines, int count, ql_coord_t line_h,
                     ql_coord_t *out_x, ql_coord_t *out_y)
{
    int li = line_index_of(in->buf, lines, count, in->caret);
    const QLTextLine *ln = &lines[li];
    const uint16_t ls = (uint16_t)(ln->start - in->buf);
    const uint16_t rel = (in->caret > ls) ? (uint16_t)(in->caret - ls) : 0u;
    /* 行尾之后跟着尾部空白被 trim 过，光标仍要能落到"逻辑行尾" */
    const uint16_t measure = (uint16_t)((rel <= ln->len) ? rel : ln->len);
    *out_x = ql_font_line_width(font, ln->start, (int32_t)measure);
    *out_y = (ql_coord_t)li * line_h;
}

static struct QLUIInput *input_of(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->inputs == NULL)
        return NULL;
    if (node->src == NULL)
        return NULL;
    if (node->src->tag != QL_UI_TAG_INPUT &&
        node->src->tag != QL_UI_TAG_TEXTAREA)
        return NULL;
    for (uint16_t i = 0; i < ui->input_count; i++)
        if (ui->inputs[i].node == node)
            return &ui->inputs[i];
    return NULL;
}

static void input_emit(QLUIRoot *ui, struct QLUIInput *in)
{
    if (ui->input_cb != NULL)
        ui->input_cb(ui, in->node, in->buf, ui->input_user);
    /* 值变了：这个节点要重画 */
    if (in->node != NULL)
        in->node->flags |= QL_UI_RT_DIRTY;
}

/* 把 [sel, caret) 选区删掉（sel 与 caret 顺序无关）；返回删除后的光标位置 */
static uint16_t input_drop_selection(struct QLUIInput *in)
{
    if (in->sel == in->caret)
        return in->caret;
    uint16_t a = in->sel;
    uint16_t b = in->caret;
    if (a > b) {
        const uint16_t t = a;
        a = b;
        b = t;
    }
    memmove(in->buf + a, in->buf + b, (size_t)(in->len - b) + 1u);
    in->len = (uint16_t)(in->len - (b - a));
    in->caret = a;
    in->sel = a;
    return a;
}

/* 在光标处插入一段 UTF-8；超长就截断（不报错：用户打字不该因为满而失败） */
/* 这个输入框的缓冲容量（含结尾 0），单行与多行不同 */
static uint16_t input_cap(const struct QLUIInput *in)
{
    return in->multiline ? (uint16_t)QL_CFG_UI_TEXTAREA_LEN
                         : (uint16_t)QL_CFG_UI_INPUT_LEN;
}

static void input_scroll_to_caret(struct QLUIInput *in);
static ql_coord_t input_scroll_max(const struct QLUIInput *in);
/* 编辑之后统一走这条：把视图拉回光标可见，并通知值变了 */
static void input_after_edit(QLUIRoot *ui, struct QLUIInput *in);

/* 编辑之后的收尾：视图归位 + 通知（所有改缓冲的路径都要过它） */
static void input_after_edit(QLUIRoot *ui, struct QLUIInput *in)
{
    input_scroll_to_caret(in);
    input_emit(ui, in);
}


/*
 * 组字结束：把预编辑文本清掉并标脏。
 *
 * 什么时候算结束？**只要有字符真的进了值缓冲**。输入法提交时发的是
 * "XLookupChars 文本 + 一次清空 preedit"，但清空那一下要等输入法下一个回调
 * 才到；中间那一帧里"提交后的值"和"还没清的预编辑"同时存在 —— 屏幕上就是
 * 一个词出现两遍，第二遍还带下划线，松手才消失。所以在插入处直接收尾。
 */
static void input_preedit_clear(QLUIRoot *ui)
{
    if (ui == NULL || ui->preedit_len == 0)
        return;
    ui->preedit_len = 0;
    ui->preedit_caret = 0;
    ui->preedit[0] = '\0';
    if (ui->focus != NULL)
        ui->focus->flags |= QL_UI_RT_DIRTY;
}

static int input_insert(QLUIRoot *ui, struct QLUIInput *in, const char *s)
{
    size_t add = strlen(s);
    if (add == 0)
        return 0;
    (void)input_drop_selection(in);

    const size_t room = (size_t)input_cap(in) - 1u - in->len;
    if (add > room)
        add = room;
    if (add == 0)
        return 0;      /* 满了：静默截断（打字不该因为"满了"而失败） */

    input_preedit_clear(ui);        /* 有字进缓冲 = 组字结束 */
    memmove(in->buf + in->caret + add, in->buf + in->caret,
            (size_t)(in->len - in->caret) + 1u);
    memcpy(in->buf + in->caret, s, add);
    in->len = (uint16_t)(in->len + add);
    in->caret = (uint16_t)(in->caret + add);
    in->sel = in->caret;
    in->want_x = -1;
    /* 打字打到底要自动滚：否则光标/新字会落到框外（多行尤其明显） */
    input_scroll_to_caret(in);
    input_emit(ui, in);
    return 1;
}

/* 光标移完把视图滚到能看见它（横向单行 / 纵向多行） */
static void input_scroll_to_caret(struct QLUIInput *in)
{
    if (in->node == NULL)
        return;
    const QLFont *font = font_of_style(in->node, in->node->style);
    if (font == NULL)
        return;
    const QLRect cb = in->node->content_box;
    /* 行距必须和绘制用的是同一个（CSS line-height 优先） */
    const ql_coord_t lh = text_line_h_of(in->node->style, font);

    ql_coord_t cx = 0, cy = 0;
    if (in->multiline) {
        QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
        int count = input_visual_lines(font, in, cb.w, lines, QL_CFG_TEXT_MAX_LINES);
        if (count > QL_CFG_TEXT_MAX_LINES)
            count = QL_CFG_TEXT_MAX_LINES;
        if (count > 0)
            caret_px(font, in, lines, count, lh, &cx, &cy);
        /* 竖直：整行露出来（上边界优先，与浏览器一致） */
        if (cy - in->scroll_y < 0)
            in->scroll_y = cy;
        else if (cy + lh - in->scroll_y > cb.h)
            in->scroll_y = cy + lh - cb.h;
        /*
         * 夹到内容范围：内容变短（删字、Ctrl+A 后删光、程序改值）时，
         * 旧的 scroll_y 可能已经超出末尾 —— 不夹的话光标会被画到框上面去，
         * 看起来就是"文字清空了，光标也不见了"。
         */
        const ql_coord_t max_y = input_scroll_max(in);
        if (in->scroll_y > max_y)
            in->scroll_y = max_y;
        if (in->scroll_y < 0)
            in->scroll_y = 0;
    } else {
        cx = ql_font_line_width(font, in->buf, (int32_t)in->caret);
    }
    /* 横向：留 1px 余量，别让光标贴着边框 */
    if (cx - in->scroll_x < 0)
        in->scroll_x = cx;
    else if (cx - in->scroll_x > cb.w - 2 && cb.w > 2)
        in->scroll_x = cx - (cb.w - 2);
    if (in->scroll_x < 0)
        in->scroll_x = 0;
}

/* ------------------------------ 剪贴板 ------------------------------ */

void ql_ui_set_clipboard(QLUIRoot *ui, const QLUIClipboard *cb, void *user)
{
    if (ui == NULL)
        return;
    ui->clipboard = cb;
    ui->clipboard_user = user;
}

/* 焦点输入框的选区文本；没有选区就取全文（"整个框复制走"更符合直觉） */
static const char *input_selection_text(struct QLUIInput *in, uint16_t *out_len)
{
    if (in == NULL) {
        *out_len = 0;
        return NULL;
    }
    uint16_t a = in->sel, b = in->caret;
    if (a == b) {         /* 没有选区：全文 */
        a = 0;
        b = in->len;
    }
    if (a > b) {
        const uint16_t t = a;
        a = b;
        b = t;
    }
    *out_len = (uint16_t)(b - a);
    return (const char *)((in->buf != NULL) ? in->buf + a : NULL);
}

static struct QLUIInput *focused_input(QLUIRoot *ui)
{
    return (ui->focus != NULL) ? input_of(ui, ui->focus) : NULL;
}

ql_err_t ql_ui_clipboard_copy(QLUIRoot *ui)
{
    if (ui == NULL)
        return QL_ERR_INVAL;
    struct QLUIInput *in = focused_input(ui);
    if (in == NULL)
        return QL_ERR_INVAL;
    if (ui->clipboard == NULL || ui->clipboard->set_text == NULL)
        return QL_ERR_UNSUPPORTED;

    uint16_t n = 0;
    const char *src = input_selection_text(in, &n);
    if (src == NULL || n == 0)
        return QL_ERR_INVAL;

    /* 先复制到栈上：后端可能要求 NUL 结尾 */
    char buf[QL_CFG_UI_TEXTAREA_LEN];
    const size_t k = (n < (uint16_t)sizeof(buf) - 1u) ? (size_t)n
                                                     : sizeof(buf) - 1u;
    memcpy(buf, src, k);
    buf[k] = '\0';
    ui->clipboard->set_text(buf, ui->clipboard_user);
    return QL_OK;
}

ql_err_t ql_ui_clipboard_cut(QLUIRoot *ui)
{
    if (ui == NULL)
        return QL_ERR_INVAL;
    struct QLUIInput *in = focused_input(ui);
    if (in == NULL)
        return QL_ERR_INVAL;
    const ql_err_t rc = ql_ui_clipboard_copy(ui);
    if (rc != QL_OK)
        return rc;
    if (in->sel != in->caret) {
        (void)input_drop_selection(in);
        input_after_edit(ui, in);      /* 剪切同样是"内容变短"，视图要归位 */
    }
    return QL_OK;
}

/* 已经有内容就插进去；返回 1 = 处理完了 */
static int paste_apply(QLUIRoot *ui, const char *text)
{
    struct QLUIInput *in = focused_input(ui);
    if (in == NULL || text == NULL)
        return 1;
    if (text[0] == '\0')
        return 1;
    (void)input_insert(ui, in, text);
    return 1;
}

ql_err_t ql_ui_clipboard_paste(QLUIRoot *ui)
{
    if (ui == NULL)
        return QL_ERR_INVAL;
    if (focused_input(ui) == NULL)
        return QL_ERR_INVAL;
    if (ui->clipboard == NULL || ui->clipboard->get_text == NULL)
        return QL_ERR_UNSUPPORTED;

    char buf[QL_CFG_UI_TEXTAREA_LEN];
    buf[0] = '\0';
    if (ui->clipboard->get_text(buf, sizeof(buf), ui->clipboard_user))
        return (paste_apply(ui, buf) == 1) ? QL_OK : QL_ERR_INVAL;

    /* 异步：内容还没到（X11 要等 SelectionNotify），下一帧再试 */
    ui->paste_pending = 1;
    return QL_OK;
}

/* 每帧重试未完成的粘贴（由 ql_ui_anim_tick 调用） */
static void clipboard_retry_paste(QLUIRoot *ui)
{
    if (!ui->paste_pending)
        return;
    if (ui->clipboard == NULL || ui->clipboard->get_text == NULL) {
        ui->paste_pending = 0;
        return;
    }
    char buf[QL_CFG_UI_TEXTAREA_LEN];
    buf[0] = '\0';
    if (ui->clipboard->get_text(buf, sizeof(buf), ui->clipboard_user)) {
        ui->paste_pending = 0;
        (void)paste_apply(ui, buf);
    }
}

static void input_set_caret(QLUIRoot *ui, struct QLUIInput *in, uint16_t pos, int extend)
{
    if (pos > in->len)
        pos = in->len;
    in->caret = pos;
    if (!extend)
        in->sel = pos;
    in->want_x = -1;      /* 横向位置变了：上下键重新量 */
    input_scroll_to_caret(in);
    if (in->node != NULL)
        in->node->flags |= QL_UI_RT_DIRTY;
    (void)ui;
}

/* 点进来：聚焦 + 定光标（并开始拖动选字） */
/* 多行：按 y 找到视觉行，再在行内按 x 定位 */
static uint16_t caret_at_xy(const QLFont *font, const struct QLUIInput *in,
                            ql_coord_t box_w, ql_coord_t line_h,
                            ql_coord_t x, ql_coord_t y)
{
    QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
    int count = input_visual_lines(font, in, box_w, lines, QL_CFG_TEXT_MAX_LINES);
    if (count <= 0)
        return 0;
    if (count > QL_CFG_TEXT_MAX_LINES)
        count = QL_CFG_TEXT_MAX_LINES;
    /*
     * 指针跑到文本上方/下方：直接落到全文首/尾，**不再按最后一行的 x 折算**。
     * 浏览器就是这样（拖选往下拉到底，选中的是到文末，而不是"最后一行的第 0 列"）。
     */
    if (y < 0)
        return 0;
    if (line_h > 0 && y >= (ql_coord_t)count * line_h)
        return in->len;
    int li = (line_h > 0 && y > 0) ? (int)(y / line_h) : 0;
    if (li >= count)
        li = count - 1;
    const QLTextLine *ln = &lines[li];
    const uint16_t ls = (uint16_t)(ln->start - in->buf);
    const ql_coord_t rel = (x > 0) ? x : 0;
    const uint16_t within = caret_at_px(font, ln->start, (uint16_t)ln->len,
                                       rel > 0 ? rel : 0);
    return (uint16_t)(ls + within);
}

/*
 * 多行输入框的内容可滚范围（像素）= 总视觉行数 × 行高 - 内容盒高。
 * 和容器滚动（node->scroll_y）不是一回事：输入框的内容是它自己画的，
 * 所以偏移存在 QLUIInput.scroll_y 里，不走 overflow 那套重排。
 */
static ql_coord_t input_scroll_max(const struct QLUIInput *in)
{
    if (in == NULL || in->node == NULL || !in->multiline)
        return 0;
    const QLFont *font = font_of_style(in->node, in->node->style);
    if (font == NULL)
        return 0;
    const QLRect cb = in->node->content_box;
    const ql_coord_t lh = text_line_h_of(in->node->style, font);
    if (lh <= 0)
        return 0;

    QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
    int count = input_visual_lines(font, in, cb.w, lines, QL_CFG_TEXT_MAX_LINES);
    if (count > QL_CFG_TEXT_MAX_LINES)
        count = QL_CFG_TEXT_MAX_LINES;
    if (count <= 0)
        return 0;

    const ql_coord_t total = (ql_coord_t)count * lh;
    return (total > cb.h) ? (total - cb.h) : 0;
}

/* 滚多行文本；返回**真正用掉**的距离（到边界就是 0，调用方好接着往父级滚） */
static ql_coord_t input_scroll_by(struct QLUIInput *in, ql_coord_t dy)
{
    if (in == NULL || in->node == NULL)
        return 0;
    const ql_coord_t max_y = input_scroll_max(in);
    const ql_coord_t old = in->scroll_y;
    ql_coord_t want = old + dy;
    if (want < 0)
        want = 0;
    if (want > max_y)
        want = max_y;
    in->scroll_y = want;
    if (want != old)
        in->node->flags |= QL_UI_RT_DIRTY;
    return want - old;
}

static int input_pointer_down(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x,
                              ql_coord_t y)
{
    struct QLUIInput *in = input_of(ui, node);
    if (in == NULL)
        return 0;
    if ((node->flags & QL_UI_RT_DISABLED) != 0)
        return 1;              /* 禁用：吃掉点击，但不动光标 */
    ql_ui_set_focus_node(ui, node);
    const QLFont *font = font_of_style(node, node->style);
    const QLRect cb = node->content_box;
    const ql_coord_t rel = x - cb.x + in->scroll_x;
    if (in->multiline) {
        /* 多行：y 定行、x 定列。行高用绘制时的同一个值 */
        const ql_coord_t lh = text_line_h_of(node->style, font);
        const ql_coord_t ry = y - cb.y + in->scroll_y;
        const uint16_t pos = caret_at_xy(font, in, cb.w, lh, rel, ry);
        input_set_caret(ui, in, pos, 0);
    } else {
        const uint16_t pos = caret_at_px(font, in->buf, in->len, rel > 0 ? rel : 0);
        input_set_caret(ui, in, pos, 0);
    }
    /*
     * `user-select: none`：仍然可以点进去、点在哪光标就在哪，
     * 只是**拖拽不再连选**（这是 CSS 里这个属性对输入框的意义）。
     */
    ui->input_drag = (node->style->user_select_none == 0);
    return 1;
}

static void input_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    struct QLUIInput *in = (ui->focus != NULL) ? input_of(ui, ui->focus) : NULL;
    if (in == NULL || !ui->input_drag)
        return;
    const QLFont *font = font_of_style(in->node, in->node->style);
    const QLRect cb = in->node->content_box;
    const ql_coord_t rel = x - cb.x + in->scroll_x;

    if (!in->multiline) {
        const uint16_t pos = caret_at_px(font, in->buf, in->len, rel > 0 ? rel : 0);
        input_set_caret(ui, in, pos, 1);
        return;
    }

    /*
     * 多行拖选：**必须按 (x, y) 定位**，不能只拿 x 当单行算。
     * 之前这里走的是单行公式 —— 拖到第二行时按"整串一行"折算字节偏移，
     * 选区和鼠标就对不上了（单行没这个问题，所以一直没暴露）。
     */
    const ql_coord_t lh = text_line_h_of(in->node->style, font);
    const ql_coord_t ry = y - cb.y + in->scroll_y;
    const uint16_t pos = caret_at_xy(font, in, cb.w, lh, rel > 0 ? rel : 0, ry);
    input_set_caret(ui, in, pos, 1);

    /*
     * 拖到框外继续滚（浏览器行为）：指针在上下边界外时按行距滚，
     * 否则内容一长就没法跨屏选。
     */
    if (lh > 0 && in->node->content_box.h > 0) {
        const ql_coord_t top = cb.y;
        const ql_coord_t bottom = cb.y + cb.h;
        ql_coord_t dy = 0;
        if (y < top)
            dy = y - top;              /* 负数 = 往上滚 */
        else if (y > bottom)
            dy = y - bottom;
        if (dy != 0) {
            const ql_coord_t step = (dy > 0) ? ((dy < lh) ? dy : lh)
                                             : ((-dy < lh) ? dy : -lh);
            /* 夹到内容末尾：不然拖到框外会一直滚进空白 */
            const ql_coord_t max_y = input_scroll_max(in);
            ql_coord_t want = in->scroll_y + step;
            if (want < 0)
                want = 0;
            if (want > max_y)
                want = max_y;
            in->scroll_y = want;
            /* 滚完重新定位（内容动了，同一个 y 对应的字符变了） */
            const ql_coord_t ry2 = y - cb.y + in->scroll_y;
            const uint16_t p2 = caret_at_xy(font, in, cb.w, lh,
                                            rel > 0 ? rel : 0, ry2);
            input_set_caret(ui, in, p2, 1);
        }
    }
}

/* 键盘：只处理"编辑"这一层；没有剪贴板（平台层不提供） */
static int input_key(QLUIRoot *ui, struct QLUIInput *in, const QLInputEvent *ev)
{
    const int shift = (ev->mods & QL_MOD_SHIFT) != 0;
    switch (ev->key) {
    case QL_KEY_BACKSPACE:
        if (in->sel != in->caret) {
            (void)input_drop_selection(in);
            input_after_edit(ui, in);
        } else if (in->caret > 0) {
            const uint16_t from = prev_boundary(in->buf, in->caret);
            memmove(in->buf + from, in->buf + in->caret,
                    (size_t)(in->len - in->caret) + 1u);
            in->len = (uint16_t)(in->len - (in->caret - from));
            in->caret = from;
            in->sel = from;
            in->want_x = -1;
            input_scroll_to_caret(in);
            input_emit(ui, in);
        }
        return 1;
    case QL_KEY_DELETE:
        if (in->sel != in->caret) {
            (void)input_drop_selection(in);
            input_after_edit(ui, in);
        } else if (in->caret < in->len) {
            const uint16_t to = next_boundary(in->buf, in->len, in->caret);
            memmove(in->buf + in->caret, in->buf + to,
                    (size_t)(in->len - to) + 1u);
            in->len = (uint16_t)(in->len - (to - in->caret));
            in->sel = in->caret;
            in->want_x = -1;
            input_scroll_to_caret(in);
            input_emit(ui, in);
        }
        return 1;
    case QL_KEY_LEFT:
        input_set_caret(ui, in, prev_boundary(in->buf, in->caret), shift);
        return 1;
    case QL_KEY_RIGHT:
        input_set_caret(ui, in, next_boundary(in->buf, in->len, in->caret), shift);
        return 1;
    case QL_KEY_HOME:
    case QL_KEY_END: {
        /*
         * 单行：整串开头/结尾。多行：**本行**开头/结尾（与浏览器一致）——
         * 否则 Home 会跳到全文开头，跨十行的 textarea 上基本没法用。
         */
        uint16_t pos;
        if (!in->multiline) {
            pos = (ev->key == QL_KEY_HOME) ? 0u : in->len;
        } else {
            QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
            const QLFont *font = font_of_style(in->node, in->node->style);
            const ql_coord_t bw = in->node->content_box.w;
            int count = input_visual_lines(font, in, bw, lines, QL_CFG_TEXT_MAX_LINES);
            if (count > QL_CFG_TEXT_MAX_LINES)
                count = QL_CFG_TEXT_MAX_LINES;
            if (count <= 0) {
                pos = 0;
            } else {
                const int li = line_index_of(in->buf, lines, count, in->caret);
                const uint16_t ls = (uint16_t)(lines[li].start - in->buf);
                pos = (ev->key == QL_KEY_HOME) ? ls
                                               : (uint16_t)(ls + lines[li].len);
            }
        }
        input_set_caret(ui, in, pos, shift);
        return 1;
    }
    case QL_KEY_UP:
    case QL_KEY_DOWN: {
        /* 单行没有"上一行"：上下键应该拿去做别的事（这里是翻页/滚动），所以不吃 */
        if (!in->multiline)
            return 0;
        const QLFont *font = font_of_style(in->node, in->node->style);
        const ql_coord_t bw = in->node->content_box.w;
        const ql_coord_t lh = text_line_h_of(in->node->style, font);
        QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
        int count = input_visual_lines(font, in, bw, lines, QL_CFG_TEXT_MAX_LINES);
        if (count > QL_CFG_TEXT_MAX_LINES)
            count = QL_CFG_TEXT_MAX_LINES;
        if (count <= 0)
            return 1;
        int li = line_index_of(in->buf, lines, count, in->caret);
        const int want = (ev->key == QL_KEY_UP) ? li - 1 : li + 1;
        if (want < 0 || want >= count)
            return 1;      /* 到边界就停住，不跳到全文首/尾 */
        /*
         * 横向位置用**记住的** want_x（sticky column）：
         *   "长行 → 短行 → 长行"要回到原来的列，而不是停在短行的行尾。
         * want_x 由 input_set_caret 在"非上下键"的移动时置为 -1 失效。
         */
        ql_coord_t cx = in->want_x;
        if (cx < 0) {
            ql_coord_t cy = 0;
            caret_px(font, in, lines, count, lh, &cx, &cy);
            in->want_x = cx;
        }
        const QLTextLine *ln = &lines[want];
        const uint16_t ls = (uint16_t)(ln->start - in->buf);
        const uint16_t within = caret_at_px(font, ln->start, (uint16_t)ln->len, cx);
        const uint16_t npos = (uint16_t)(ls + within);
        /* 直接落位：不经过 input_set_caret，免得把 want_x 清掉 */
        in->caret = (npos > in->len) ? in->len : npos;
        if (!shift)
            in->sel = in->caret;
        input_scroll_to_caret(in);
        if (in->node != NULL)
            in->node->flags |= QL_UI_RT_DIRTY;
        return 1;
        return 1;
    }
    case QL_KEY_ENTER:
        if (in->multiline) {
            /* 多行：回车就是插入一个 '\n'（走和普通字符同一条插入路径） */
            if (input_insert(ui, in, "\n"))
                return 1;
        }
        if (ui->input_cb != NULL)
            ui->input_cb(ui, in->node, in->buf, ui->input_user);
        return 1;
    case QL_KEY_ESCAPE:
        ui->input_drag = 0;
        ql_ui_set_focus_node(ui, NULL);
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------ 键盘焦点 ------------------------------ */

/* 能接受键盘操作的控件（也决定 Tab 的落点） */
static int node_is_focusable(const QLUINodeRT *nd)
{
    if (nd == NULL || nd->src == NULL || nd->style == NULL)
        return 0;
    if (nd->style->display == QL_UI_DISPLAY_NONE)
        return 0;
    const uint16_t tag = nd->src->tag;
    return tag == QL_UI_TAG_INPUT || tag == QL_UI_TAG_TEXTAREA ||
           tag == QL_UI_TAG_SELECT || tag == QL_UI_TAG_BUTTON;
}

/*
 * Tab / Shift+Tab：按树序在可聚焦节点之间循环，到头上就绕回。
 *
 * 聚焦到输入框时光标放到末尾（不选中全文）—— 选中全文就得再处理"一打字就
 * 覆盖整串"的语义，那是 `<textarea>`/多行编辑该一起决定的事，先不做。
 */
static void focus_set(QLUIRoot *ui, QLUINodeRT *nd)
{
    ql_ui_set_focus_node(ui, nd);
    struct QLUIInput *in = input_of(ui, nd);
    if (in != NULL) {
        in->caret = in->len;
        in->sel = in->len;
        in->scroll_x = 0;
        nd->flags |= QL_UI_RT_DIRTY;
    }
}

static void focus_step(QLUIRoot *ui, int dir)
{
    const int n = (int)ui->node_count;
    if (n <= 0)
        return;
    int i = (ui->focus != NULL) ? (int)(ui->focus - ui->nodes) : (dir > 0 ? -1 : n);
    for (int k = 0; k < n; k++) {
        i += dir;
        if (i < 0)
            i = n - 1;
        else if (i >= n)
            i = 0;
        if (node_is_focusable(&ui->nodes[i])) {
            focus_set(ui, &ui->nodes[i]);
            return;
        }
    }
    ql_ui_set_focus_node(ui, NULL);  /* 一个能聚焦的都没有 */
}

/* ------------------------------ 滚动 ------------------------------ */

/*
 * 把 `overflow: auto` 容器的子树按当前偏移平移。
 *
 * 为什么把偏移**烘进布局**而不是在绘制时加：
 *   - 绘制、命中测试、paint_box、脏区全都读 node->border_box，只要它是对的，
 *     上面四件事一行都不用改；
 *   - 代价是滚动后要重跑一次布局（211 节点 0.037ms，比维护一套并行的
 *     "绘制偏移 + 逆变换命中"便宜，也不容易漏）。
 *
 * 这也是为什么 `ql_ui_scroll_by()` 内部直接重排一次：正确性优先。
 */
static void apply_scroll(QLUINodeRT *node, ql_coord_t cx, ql_coord_t cy,
                         ql_coord_t cw, ql_coord_t ch)
{
    const QLUIStyle *s = node->style;
    const int auto_x = (s->overflow_x == QL_UI_OVERFLOW_AUTO);
    const int auto_y = (s->overflow_y == QL_UI_OVERFLOW_AUTO);

    if (!auto_x && !auto_y) {
        /* 两轴都不 auto：不滚（visible / hidden 都只裁不滚，与 CSS 一致） */
        node->scroll_x = node->scroll_y = 0;
        node->scroll_max_x = node->scroll_max_y = 0;
        return;
    }

    /* 内容外沿（含 margin）：从内容盒左上角量起 */
    ql_coord_t ext_w = 0, ext_h = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        const QLRect b = c->border_box;
        const ql_coord_t r = ql_rect_right(b) + s->margin.right + c->style->margin.right;
        const ql_coord_t bo =
            ql_rect_bottom(b) + s->margin.bottom + c->style->margin.bottom;
        const ql_coord_t w = r - cx;
        const ql_coord_t h = bo - cy;
        if (w > ext_w)
            ext_w = w;
        if (h > ext_h)
            ext_h = h;
    }

    /* 只有 auto 的那个轴才有可滚范围（另一个轴固定为 0） */
    node->scroll_max_x = (auto_x && ext_w > cw) ? (ext_w - cw) : 0;
    node->scroll_max_y = (auto_y && ext_h > ch) ? (ext_h - ch) : 0;

    if (node->scroll_x < 0)
        node->scroll_x = 0;
    if (node->scroll_y < 0)
        node->scroll_y = 0;
    if (node->scroll_x > node->scroll_max_x)
        node->scroll_x = node->scroll_max_x;
    if (node->scroll_y > node->scroll_max_y)
        node->scroll_y = node->scroll_max_y;

    if (node->scroll_x != 0 || node->scroll_y != 0)
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling)
            translate_subtree(c, -node->scroll_x, -node->scroll_y);
}

/*
 * 行内盒排满一行之后，按容器的 `text-align` 把整行推到中间或右边。
 *
 * 行内盒没有"剩余空间分配"的概念（那是 flex/grid 的事），所以对齐就是平移
 * 已经布好的子树——比按新 x 重排一遍便宜，也保证子节点内部
 * （百分比宽度、绝对定位的包含块）都跟着走。
 *
 * `first` 是本行第一个参与排布的子节点，`stop` 是下一行的第一个子节点
 * （NULL 表示本行是最后一行）。
 */
static void align_inline_row(QLUINodeRT *first, QLUINodeRT *stop,
                             ql_coord_t row_w, ql_coord_t used, uint8_t align)
{
    if (first == NULL || align == QL_TEXT_ALIGN_LEFT)
        return;
    const ql_coord_t free_w = row_w - used;
    if (free_w <= 0)
        return;
    const ql_coord_t dx = (align == QL_TEXT_ALIGN_CENTER) ? (free_w / 2) : free_w;
    if (dx == 0)
        return;
    for (QLUINodeRT *c = first; c != stop; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        if (is_out_of_flow(c->style))
            continue;
        translate_subtree(c, dx, 0);
    }
}

/* ------------------------------ 主布局 ------------------------------ */

static ql_coord_t layout_node(QLUINodeRT *node, ql_coord_t x, ql_coord_t y,
                              ql_coord_t avail_w, ql_coord_t basis_h)
{
    const QLUIStyle *s = node->style;

    if (s->display == QL_UI_DISPLAY_NONE) {
        node->border_box = ql_rect_make(x, y, 0, 0);
        node->content_box = node->border_box;
        return 0;
    }

    const QLFont *font = font_of_style(node, s);
    ql_coord_t border_w = border_width_of(node, avail_w);

    /*
     * `margin: auto`：横向把**剩余空间**分掉（`margin: 0 auto` 就是最常用的居中）。
     * 只在块级盒上有意义（行内盒宽度由内容决定，没有剩余可分配）。
     * 纵向 auto 按 0 处理 —— 那是"块级外边距合并"的领域，本库还不做外边距合并，
     * 单独实现一套纵向居中容易与它打架，宁可明确不做。
     */
    ql_coord_t m_left = s->margin.left;
    ql_coord_t m_right = s->margin.right;
    {
        const uint8_t lu = (uint8_t)((s->margin_unit >> 6) & 0x3u);   /* left */
        const uint8_t ru = (uint8_t)((s->margin_unit >> 2) & 0x3u);   /* right */
        const int auto_l = (lu == 3);
        const int auto_r = (ru == 3);
        if ((auto_l || auto_r) && s->display == QL_UI_DISPLAY_BLOCK) {
            ql_coord_t used = border_w + (auto_l ? 0 : m_left) +
                              (auto_r ? 0 : m_right);
            ql_coord_t rest = avail_w - used;
            if (rest < 0)
                rest = 0;
            if (auto_l && auto_r) {
                m_left = rest / 2;
                m_right = rest - m_left;      /* 奇数像素落在右边，总宽不差 1 */
            } else if (auto_l) {
                m_left = rest;
            } else {
                m_right = rest;
            }
        }
    }

    ql_coord_t box_x = x + m_left;
    ql_coord_t box_y = y + s->margin.top;

    ql_coord_t content_x = box_x + s->border_w[3] + s->padding.left;
    ql_coord_t content_y = box_y + s->border_w[0] + s->padding.top;
    ql_coord_t content_w = inner_width_of(s, border_w);
    if (content_w < 0)
        content_w = 0;

    ql_coord_t content_h;

    /*
     * 子节点的百分比高度基准：只有本节点高度**确定**时才把内容高传下去，
     * 否则传 -1（子节点里的 `height:100%` 按 auto 处理，与 CSS 一致）。
     *
     * 必须在分支之前算：block 分支要先知道基准才能布局子节点，而 content_h
     * 那时正由子节点的高度累加出来（鸡生蛋），所以这里直接用**声明值**。
     */
    ql_coord_t decl_h = -1;
    if (height_is_definite(s, basis_h)) {
        decl_h = height_of_style(s, basis_h, 0);
        if (s->box_sizing == 1) {
            decl_h -= ql_ui_border_v(s) + s->padding.top + s->padding.bottom;
            if (decl_h < 0)
                decl_h = 0;
        }
    }

    if (node->first_child != NULL && is_table_container(s)) {
        int definite_h = height_is_definite(s, basis_h);
        content_h = layout_table_children(node->owner, node, content_x, content_y,
                                          content_w, definite_h);
    } else if (node->first_child != NULL && is_grid_container(s)) {
        /* grid 容器：轨道定尺之后逐格摆放（见 layout_grid_children） */
        int definite_h = height_is_definite(s, basis_h);
        ql_coord_t avail_h = 0;
        if (definite_h) {
            ql_coord_t hv = height_of_style(s, basis_h, 0);
            if (s->box_sizing == 1)
                avail_h = hv - ql_ui_border_v(s) - s->padding.top - s->padding.bottom;
            else
                avail_h = hv;
            if (avail_h < 0)
                avail_h = 0;
        }
        content_h = layout_grid_children(node->owner, node, content_x, content_y,
                                         content_w, avail_h, definite_h);
    } else if (node->first_child != NULL && is_multicol(s)) {
        /* 多栏：量一遍 → 按栏高贪心分配（见 layout_columns_children） */
        content_h = layout_columns_children(node, content_x, content_y, content_w,
                                            decl_h);
    } else if (node->first_child != NULL && is_flex_container(s)) {
        /*
         * flex 容器：显式高度时才有剩余空间可分配（grow/shrink 才有意义）。
         * box-sizing：border-box 时 height 指边框盒，content-box 时指内容盒。
         */
        int definite_h = height_is_definite(s, basis_h);
        ql_coord_t avail_h = 0;
        if (definite_h) {
            ql_coord_t hv = height_of_style(s, basis_h, 0);
            if (s->box_sizing == 1) {
                avail_h = hv - ql_ui_border_v(s) - s->padding.top - s->padding.bottom;
            } else {
                avail_h = hv;
            }
            if (avail_h < 0)
                avail_h = 0;
        }
        content_h = layout_flex_children(node, content_x, content_y, content_w,
                                         avail_h, definite_h);
    } else if (node->first_child != NULL) {
        /* block 子节点纵向堆叠；inline / inline-block 子节点横向排布并换行 */
        ql_coord_t cursor_y = content_y;
        ql_coord_t cursor_x = content_x;
        ql_coord_t row_h = 0;
        QLUINodeRT *row_first = NULL;   /* 本行第一个行内子节点 */
        int in_row = 0;

        for (QLUINodeRT *child = node->first_child; child != NULL;
             child = child->next_sibling) {
            uint8_t d = child->style->display;
            if (d == QL_UI_DISPLAY_NONE)
                continue;
            if (is_out_of_flow(child->style))
                continue;      /* 绝对/固定定位：不参与常规流，后面单独处理 */

            if (d == QL_UI_DISPLAY_BLOCK) {
                if (in_row) {           /* 结束当前行 */
                    align_inline_row(row_first, child, content_w,
                                     cursor_x - content_x, s->text_align);
                    cursor_y += row_h;
                    cursor_x = content_x;
                    row_h = 0;
                    row_first = NULL;
                    in_row = 0;
                }
                cursor_y += layout_node(child, content_x, cursor_y, content_w,
                                        decl_h);
            } else {
                ql_coord_t child_avail = content_w;
                ql_coord_t child_w = border_width_of(child, content_w) +
                                     child->style->margin.left + child->style->margin.right;
                if (in_row && cursor_x + child_w > content_x + content_w) {
                    align_inline_row(row_first, child, content_w,
                                     cursor_x - content_x, s->text_align);
                    cursor_y += row_h;  /* 换行 */
                    cursor_x = content_x;
                    row_h = 0;
                    row_first = NULL;
                    in_row = 0;
                }
                if (!in_row)
                    row_first = child;
                ql_coord_t h = layout_node(child, cursor_x, cursor_y, child_avail,
                                           decl_h);
                cursor_x += outer_width_of(child);
                if (h > row_h)
                    row_h = h;
                in_row = 1;
            }
        }

        if (in_row) {
            align_inline_row(row_first, NULL, content_w, cursor_x - content_x,
                             s->text_align);
            cursor_y += row_h;
        }
        content_h = cursor_y - content_y;
    } else if (has_shown_text(node)) {
        const ql_coord_t lh = text_line_h_of(s, font);
        if (s->height.unit == QL_UI_UNIT_AUTO) {
            /*
             * 折行时高度由行数决定：这里必须用**和绘制同一份**行数统计
             * （ql_text_wrap 传 out = NULL 就只数行），否则会出现
             * "布局按一行算、绘制画了三行"的错位。
             */
            /*
             * 行数必须和绘制用同一份统计：`ql_text_wrap` 在 max_w = 0 时
             * 不折行、但仍然按 `\n` 硬换行 —— 所以 `pre` 也走它，
             * 只是不给宽度。
             */
            const QLTextWrapOpt wopt = wrap_opt_of(s);
            int lines = ql_text_wrap_opt(font, node->text,
                                         QL_UI_WS_WRAPS(s->white_space) ? content_w : 0,
                                         &wopt, NULL, 0, 0);
            if (lines < 1)
                lines = 1;
            content_h = lh * (ql_coord_t)lines;
        } else {
            content_h = height_of_style(s, basis_h, lh);
        }
    } else if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG) {
        /*
         * <img> 的高度：CSS 给了就按 CSS（下面的通用收尾会覆盖），
         * 否则按固有高；宽度被改写时按宽高比缩（整数运算，避免浮点）：
         *   h = content_w * 固有h / 固有w
         */
        ql_coord_t iw = 0, ih = 0;
        image_node_size(node->owner, node, &iw, &ih);
        if (s->height.unit == QL_UI_UNIT_AUTO && iw > 0 && ih > 0 &&
            content_w != iw) {
            content_h = (ql_coord_t)(((int32_t)content_w * (int32_t)ih) / (int32_t)iw);
        } else {
            content_h = ih;
        }
    } else {
        content_h = 0;
    }

    /*
     * 显式高度精确生效：更矮时内容溢出（由 overflow 决定是否裁掉），更高时留白。
     * 这与 CSS 一致——容器高度不由内容决定。
     *
     * box-sizing 语义：border-box 时 height 指边框盒，要扣掉 padding 与边框
     * 才是内容高（之前漏了这一步，导致所有 box-sizing:border-box 的元素被撑大
     * 一整个 padding+border）。
     *
     * 例外：flex 容器的高度已在上面算好（含剩余空间分配），这里不再覆盖。
     */
    if (height_is_definite(s, basis_h) &&
        !(node->first_child != NULL &&
          (is_flex_container(s) || is_grid_container(s) || is_table_container(s)))) {
        ql_coord_t hv = height_of_style(s, basis_h, content_h);
        if (s->box_sizing == 1) {
            hv -= ql_ui_border_v(s) + s->padding.top + s->padding.bottom;
            if (hv < 0)
                hv = 0;
        }
        content_h = hv;
    }

    /*
     * `aspect-ratio`：只补"缺的那一边"。
     *   - 高度确定 + 宽度 auto → 由高推宽（同时要把 border_w 一起改，否则盒子对不上）
     *   - 否则高度 auto → 由宽推高
     * 两个方向都写了具体值时以具体值为准（与 CSS 一致）。
     */
    if (s->aspect_w > 0 && s->aspect_h > 0) {
        const int h_def = height_is_definite(s, basis_h);
        if (h_def && s->width.unit == QL_UI_UNIT_AUTO) {
            const ql_coord_t cw =
                (ql_coord_t)(((int32_t)content_h * (int32_t)s->aspect_w) /
                             (int32_t)s->aspect_h);
            content_w = (cw > 0) ? cw : 0;
            border_w = content_w + s->padding.left + s->padding.right +
                       ql_ui_border_h(s);
        } else if (!h_def) {
            const ql_coord_t ch =
                (ql_coord_t)(((int32_t)content_w * (int32_t)s->aspect_h) /
                             (int32_t)s->aspect_w);
            content_h = (ch > 0) ? ch : 0;
        }
    }

    /*
     * `min-height` / `max-height`：与宽度同一套口径 ——
     * border-box 时夹边框盒，content-box 时夹内容高。
     */
    if (s->box_sizing == 1) {
        ql_coord_t bh = content_h + s->padding.top + s->padding.bottom +
                        ql_ui_border_v(s);
        const ql_coord_t clamped =
            clamp_len(s->min_height, s->max_height, bh, 0);
        if (clamped != bh) {
            content_h = clamped - s->padding.top - s->padding.bottom -
                        ql_ui_border_v(s);
            if (content_h < 0)
                content_h = 0;
        }
    } else {
        content_h = clamp_len(s->min_height, s->max_height, content_h, 0);
    }

    ql_coord_t border_h = content_h + s->padding.top + s->padding.bottom +
                          ql_ui_border_v(s);

    node->border_box = ql_rect_make(box_x, box_y, border_w, border_h);
    node->content_box = ql_rect_make(content_x, content_y, content_w, content_h);

    /* ---- 定位收尾 ---- */
    if (s->position == QL_UI_POSITION_STATIC) {
        node->abs_cb = (node->parent != NULL)
                           ? node->parent->abs_cb
                           : ql_rect_make(0, 0, s_viewport_w, s_viewport_h);
    } else {
        /* 绝对定位的包含块是**padding box**（CSS 规范），即边框盒内缩一个边框宽 */
        node->abs_cb = ql_rect_make(box_x + s->border_w[3], box_y + s->border_w[0],
                                    border_w - ql_ui_border_h(s),
                                    border_h - ql_ui_border_v(s));
    }

    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->style->display == QL_UI_DISPLAY_NONE || !is_out_of_flow(child->style))
            continue;
        QLRect cb = (child->style->position == QL_UI_POSITION_FIXED)
                        ? ql_rect_make(0, 0, s_viewport_w, s_viewport_h)
                        : node->abs_cb;
        layout_absolute(child, cb, s_viewport_w);
    }

    /* 滚动：把越界的内容整体平移上去（放在 relative 之后，按最终位置算） */
    apply_scroll(node, content_x, content_y, content_w, content_h);

    /* relative：布局完成后整体平移，不影响兄弟节点 */
    if (s->position == QL_UI_POSITION_RELATIVE) {
        ql_coord_t dx = 0, dy = 0;
        if (s->inset[3].unit != QL_UI_UNIT_AUTO)      /* left */
            dx = resolve_inset(s->inset[3], parent_w_hint(node), 0);
        else if (s->inset[1].unit != QL_UI_UNIT_AUTO) /* right */
            dx = -resolve_inset(s->inset[1], parent_w_hint(node), 0);
        if (s->inset[0].unit != QL_UI_UNIT_AUTO)      /* top */
            dy = resolve_inset(s->inset[0], 0, 0);
        else if (s->inset[2].unit != QL_UI_UNIT_AUTO) /* bottom */
            dy = -resolve_inset(s->inset[2], 0, 0);
        translate_subtree(node, dx, dy);
    }

    return border_h + s->margin.top + s->margin.bottom;
}


/* ============================== Grid 布局（B6） ============================== */

/*
 * 与 flex 的分工：flex 是"一维 + 剩余空间按 grow/shrink 分"，grid 是
 * "先把轨道定尺、再逐格摆放"。两者都只做**整数**运算，结果与后端无关。
 *
 * 定尺顺序（与 CSS 的意图一致，但只做"够用"的那一档）：
 *   ① 每条轨道的 min（px / % 直接算；auto = 0）
 *   ② max 是定长的（minmax 的上限）→ 取满
 *   ③ max 是 auto → 取该列内容的最大 max-content
 *   ④ max 是 fr → 把剩下的空间按 fr 权重分（累计推进，余数不丢）
 *   ⑤ 行高：先按"列宽已定"把每项真排一遍拿到高度；auto 行取最大值；
 *      span>1 的项按差额均摊；fr 行分剩余（容器高度确定时）
 */

/* Grid 容器判定：包关掉时恒为 0，所有分支自然退回块流 */
static int is_grid_container(const QLUIStyle *s)
{
#if QL_FEAT_CSS_GRID
    return s->display == QL_UI_DISPLAY_GRID || s->display == QL_UI_DISPLAY_INLINE_GRID;
#else
    (void)s;
    return 0;
#endif
}

#if QL_FEAT_CSS_GRID

#define QL_GRID_OCC_COLS QL_CFG_UI_GRID_MAX_COLS
/* 没写 fr 权重时的默认值（与 parser 的定点一致：fr×1000） */
#define GRID_FR_ONE_DEF 1000
#define QL_GRID_OCC_ROWS QL_CFG_UI_GRID_MAX_ROWS

typedef struct {
    int16_t col, row;   /* 0-based */
    uint8_t cspan, rspan;
} QLGridPlace;

/*
 * 临时区的**栈式**分配：一次布局里 grid 可以嵌套，所以每一层都要拿到
 * 自己的那一段。`ql_ui_layout` 两端把 used 归零，内层返回时由调用方还原。
 */
static QLGridPlace *grid_take_place(QLUIRoot *ui, int nitems)
{
    if (ui->grid_occ == NULL) {
        ui->grid_occ = (uint8_t *)ql_mem_calloc(
            1, (size_t)QL_GRID_OCC_COLS * QL_GRID_OCC_ROWS);
        if (ui->grid_occ == NULL)
            return NULL;
    }
    const size_t need = (size_t)ui->grid_place_used + (size_t)nitems;
    if (need > ui->grid_place_cap) {
        const size_t cap = (need < 16u) ? 16u : need * 2u;
        void *np = ql_mem_realloc(ui->grid_place, cap * sizeof(QLGridPlace));
        if (np == NULL)
            return NULL;
        ui->grid_place = np;
        ui->grid_place_cap = (uint16_t)((cap > 0xFFFFu) ? 0xFFFFu : cap);
    }
    QLGridPlace *p = (QLGridPlace *)ui->grid_place + ui->grid_place_used;
    ui->grid_place_used = (uint16_t)(ui->grid_place_used + (uint16_t)nitems);
    return p;
}

static ql_coord_t *grid_take_coords(QLUIRoot *ui, uint32_t n)
{
    const uint32_t need = ui->grid_size_used + n;
    if (need > ui->grid_size_cap) {
        const uint32_t cap = (need < 64u) ? 64u : need * 2u;
        ql_coord_t *np = (ql_coord_t *)ql_mem_realloc(
            ui->grid_size, (size_t)cap * sizeof(ql_coord_t));
        if (np == NULL)
            return NULL;
        ui->grid_size = np;
        ui->grid_size_cap = cap;
    }
    ql_coord_t *p = ui->grid_size + ui->grid_size_used;
    ui->grid_size_used = need;
    return p;
}

/* 一条轨道的 min / max 长度值（px）；百分比按 basis 折算 */
static ql_coord_t grid_track_len(const QLUIGridTrack *t, int max_side,
                                 ql_coord_t basis)
{
    const uint8_t kind = max_side ? t->max_kind : t->min_kind;
    const int16_t val = max_side ? t->max_val : t->min_val;
    switch (kind) {
    case QL_UI_GT_FIXED:
        return (ql_coord_t)val;
    case QL_UI_GT_PERCENT:
        return (basis > 0) ? (ql_coord_t)(((int32_t)basis * (int32_t)val) / 1000)
                           : 0;
    default:
        return 0;      /* FR / AUTO 交给后面的分配阶段 */
    }
}

/* 第 j 列的轨道（超出显式轨道 → 隐式轨道，用 `grid-auto-columns`） */
static QLUIGridTrack grid_col_track(const QLUIDoc *doc, const QLUIGrid *g, int j,
                                    const QLUIStyle *cs)
{
    QLUIGridTrack t;
    memset(&t, 0, sizeof(t));
    t.min_kind = QL_UI_GT_AUTO;
    t.max_kind = QL_UI_GT_AUTO;
    if (g != NULL && j < (int)g->col_count && doc->grid_tracks != NULL)
        return doc->grid_tracks[g->col_first + j];
    if (cs->grid_auto_col > 0) {
        t.min_kind = t.max_kind = QL_UI_GT_FIXED;
        t.min_val = t.max_val = cs->grid_auto_col;
    }
    return t;
}

static QLUIGridTrack grid_row_track(const QLUIDoc *doc, const QLUIGrid *g, int i,
                                    const QLUIStyle *cs)
{
    QLUIGridTrack t;
    memset(&t, 0, sizeof(t));
    t.min_kind = QL_UI_GT_AUTO;
    t.max_kind = QL_UI_GT_AUTO;
    if (g != NULL && i < (int)g->row_count && doc->grid_tracks != NULL)
        return doc->grid_tracks[g->row_first + i];
    if (cs->grid_auto_row > 0) {
        t.min_kind = t.max_kind = QL_UI_GT_FIXED;
        t.min_val = t.max_val = cs->grid_auto_row;
    }
    return t;
}

/* 从 (r,c) 起 rs×cs 的格子是否全空 */
static int grid_cells_free(const uint8_t *occ, int r, int c, int rs, int cs)
{
    if (r < 0 || c < 0 || r + rs > QL_GRID_OCC_ROWS || c + cs > QL_GRID_OCC_COLS)
        return 0;
    for (int i = 0; i < rs; i++)
        for (int j = 0; j < cs; j++)
            if (occ[(size_t)(r + i) * QL_GRID_OCC_COLS + (size_t)(c + j)] != 0)
                return 0;
    return 1;
}

static void grid_cells_mark(uint8_t *occ, int r, int c, int rs, int cs)
{
    for (int i = 0; i < rs; i++)
        for (int j = 0; j < cs; j++)
            occ[(size_t)(r + i) * QL_GRID_OCC_COLS + (size_t)(c + j)] = 1;
}

static ql_coord_t layout_grid_children(QLUIRoot *ui, QLUINodeRT *node,
                                       ql_coord_t content_x, ql_coord_t content_y,
                                       ql_coord_t content_w, ql_coord_t avail_h,
                                       int definite_h)
{
    const QLUIStyle *cs = node->style;
    const QLUIDoc *doc = ui->doc;
    const QLUIGrid *g = NULL;
    if (doc != NULL && doc->grids != NULL && cs->grid < doc->grid_count)
        g = &doc->grids[cs->grid];

    const ql_coord_t gap_c = (ql_coord_t)cs->gap_col;
    const ql_coord_t gap_r = (ql_coord_t)cs->gap_row;

    int ncol = (g != NULL) ? (int)g->col_count : 0;
    if (ncol <= 0 && g != NULL)
        ncol = (int)g->col_hint;
    if (ncol <= 0)
        ncol = 1;
    if (ncol > QL_GRID_OCC_COLS)
        ncol = QL_GRID_OCC_COLS;
    int nrow_exp = (g != NULL) ? (int)g->row_count : 0;
    if (nrow_exp <= 0 && g != NULL)
        nrow_exp = (int)g->row_hint;
    if (nrow_exp < 1)
        nrow_exp = 1;

    sort_children_by_order(node);

    int nitems = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
            continue;
        nitems++;
    }
    if (nitems == 0)
        return 0;
    /* 记下 bump 指针：函数出口（含降级路径）都要还原，否则嵌套 grid 会互相踩 */
    const uint16_t saved_place = ui->grid_place_used;
    const uint32_t saved_coord = ui->grid_size_used;
    QLGridPlace *pl = grid_take_place(ui, nitems);
    ql_coord_t *coords = grid_take_coords(ui, 2u * QL_GRID_OCC_COLS +
                                                 2u * QL_GRID_OCC_ROWS);
    if (pl == NULL || coords == NULL) {
        QL_LOGW("ui: grid 临时区分配失败，退回块流");
        ui->grid_place_used = saved_place;
        ui->grid_size_used = saved_coord;
        goto block_fallback;
    }

    uint8_t *occ = ui->grid_occ;
    memset(occ, 0, (size_t)QL_GRID_OCC_COLS * QL_GRID_OCC_ROWS);

    /* ---------------- ① 放置 ---------------- */
    const int dense = (cs->grid_flow & 2) != 0;
    /*
     * 自动放置的列边界 = **显式列数**。少了这一条，全自动项会一路往右排到
     * 32 列上限去（隐式列），而模板里的 fr 列反而算不出宽度 ——
     * 表现是"每格都只有内容那么宽，横向被撑出屏外"。
     */
    const int flow_cmax = ncol;
    int cur_r = 0, cur_c = 0;
    int used_cols = ncol, used_rows = nrow_exp;
    {
        int k = 0;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            const QLUIStyle *is = c->style;
            if (is->display == QL_UI_DISPLAY_NONE || is_out_of_flow(is))
                continue;

            int cspan = (is->grid_col_span > 0) ? (int)is->grid_col_span : 1;
            int rspan = (is->grid_row_span > 0) ? (int)is->grid_row_span : 1;
            if (cspan > QL_GRID_OCC_COLS)
                cspan = QL_GRID_OCC_COLS;
            if (rspan > QL_GRID_OCC_ROWS)
                rspan = QL_GRID_OCC_ROWS;
            int c0 = (is->grid_col > 0) ? (int)is->grid_col - 1 : -1;
            int r0 = (is->grid_row > 0) ? (int)is->grid_row - 1 : -1;
            if (c0 >= QL_GRID_OCC_COLS)
                c0 = QL_GRID_OCC_COLS - 1;
            if (r0 >= QL_GRID_OCC_ROWS)
                r0 = QL_GRID_OCC_ROWS - 1;

            int pr = 0, pc = 0;
            if (c0 >= 0 && r0 >= 0) {
                pr = r0;
                pc = c0;
            } else if (c0 >= 0) {
                /* 只给了列：往下找第一个放得下的行 */
                pr = dense ? 0 : cur_r;
                while (pr <= QL_GRID_OCC_ROWS - rspan &&
                       !grid_cells_free(occ, pr, c0, rspan, cspan))
                    pr++;
                if (pr > QL_GRID_OCC_ROWS - rspan)
                    pr = QL_GRID_OCC_ROWS - rspan;
                pc = c0;
            } else if (r0 >= 0) {
                /* 只给了行：往右找第一个放得下的列（列边界是显式列数） */
                pc = 0;
                while (pc <= flow_cmax - cspan &&
                       !grid_cells_free(occ, r0, pc, rspan, cspan))
                    pc++;
                if (pc > flow_cmax - cspan)
                    pc = flow_cmax - cspan;
                if (pc < 0)
                    pc = 0;
                pr = r0;
            } else {
                /* 全自动：从游标开始扫（dense 从原点扫） */
                int sr = dense ? 0 : cur_r;
                int sc = dense ? 0 : cur_c;
                int found = 0;
                for (int r = sr; r <= QL_GRID_OCC_ROWS - rspan && !found; r++) {
                    const int start_c = (r == sr) ? sc : 0;
                    for (int cc = start_c; cc <= flow_cmax - cspan; cc++) {
                        if (grid_cells_free(occ, r, cc, rspan, cspan)) {
                            pr = r;
                            pc = cc;
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) {
                    /* 放不下（超出上限）：挤到最后一格，宁可重叠也不丢内容 */
                    pr = QL_GRID_OCC_ROWS - rspan;
                    pc = QL_GRID_OCC_COLS - cspan;
                    if (pr < 0)
                        pr = 0;
                    if (pc < 0)
                        pc = 0;
                }
                if (!dense) {
                    cur_r = pr;
                    cur_c = pc + cspan;
                }
            }
            grid_cells_mark(occ, pr, pc, rspan, cspan);
            pl[k].col = (int16_t)pc;
            pl[k].row = (int16_t)pr;
            pl[k].cspan = (uint8_t)cspan;
            pl[k].rspan = (uint8_t)rspan;
            k++;

            if (pc + cspan > used_cols)
                used_cols = pc + cspan;
            if (pr + rspan > used_rows)
                used_rows = pr + rspan;
            /* 显式给了位置时，游标也要跟着前进，避免后面的自动项插到前面去 */
            if (!dense && (c0 >= 0 || r0 >= 0)) {
                if (pr > cur_r) {
                    cur_r = pr;
                    cur_c = pc + cspan;
                } else if (pr == cur_r && pc + cspan > cur_c) {
                    cur_c = pc + cspan;
                }
            }
        }
    }
    if (used_cols > QL_GRID_OCC_COLS)
        used_cols = QL_GRID_OCC_COLS;
    if (used_rows > QL_GRID_OCC_ROWS)
        used_rows = QL_GRID_OCC_ROWS;
    if (used_cols < 1)
        used_cols = 1;
    if (used_rows < 1)
        used_rows = 1;

    ql_coord_t *cw = coords;                                   /* [COLS] */
    ql_coord_t *cx = coords + QL_GRID_OCC_COLS;                /* [COLS] */
    ql_coord_t *rh = coords + 2 * QL_GRID_OCC_COLS;            /* [ROWS] */
    ql_coord_t *ry = coords + 2 * QL_GRID_OCC_COLS + QL_GRID_OCC_ROWS;

    /* ---------------- ② 列定尺 ---------------- */
    {
        int32_t total_fr = 0;
        for (int j = 0; j < used_cols; j++) {
            const QLUIGridTrack t = grid_col_track(doc, g, j, cs);
            cw[j] = grid_track_len(&t, 0, content_w);
            if (t.max_kind == QL_UI_GT_FR) {
                total_fr += (t.max_val > 0) ? t.max_val : GRID_FR_ONE_DEF;
            } else if (t.max_kind == QL_UI_GT_FIXED || t.max_kind == QL_UI_GT_PERCENT) {
                const ql_coord_t mx = grid_track_len(&t, 1, content_w);
                if (mx > cw[j])
                    cw[j] = mx;
            }
        }
        /* 内容定尺：max 是 auto 的列取该项的最大 max-content */
        int k = 0;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            const QLUIStyle *is = c->style;
            if (is->display == QL_UI_DISPLAY_NONE || is_out_of_flow(is))
                continue;
            const QLGridPlace p = pl[k++];
            const QLUIGridTrack t = grid_col_track(doc, g, p.col, cs);
            if (t.max_kind != QL_UI_GT_AUTO)
                continue;
            const ql_coord_t mh = (ql_coord_t)(is->margin.left + is->margin.right);
            const ql_coord_t need = max_content_main(c, 1, content_w, 0) - mh;
            if (p.cspan == 1) {
                if (need > cw[p.col])
                    cw[p.col] = need;
            } else {
                /* 跨列：差额记到最后一列（简单且稳定；与浏览器同量级） */
                const int jl = p.col + p.cspan - 1;
                int32_t have = 0;
                for (int j = p.col; j <= jl; j++)
                    have += cw[j];
                have += (int32_t)gap_c * (p.cspan - 1);
                if (need > have)
                    cw[jl] += need - have;
            }
        }
        /* fr 列分剩余空间（累计推进） */
        int32_t fixed = (int32_t)gap_c * (used_cols > 1 ? used_cols - 1 : 0);
        for (int j = 0; j < used_cols; j++)
            fixed += cw[j];
        int32_t remaining = (int32_t)content_w - fixed;
        if (remaining > 0 && total_fr > 0) {
            int64_t num = 0;
            for (int j = 0; j < used_cols; j++) {
                const QLUIGridTrack t = grid_col_track(doc, g, j, cs);
                if (t.max_kind != QL_UI_GT_FR)
                    continue;
                const int32_t w = (t.max_val > 0) ? t.max_val : GRID_FR_ONE_DEF;
                num += (int64_t)remaining * (int64_t)w;
                ql_coord_t add = (ql_coord_t)(num / (int64_t)total_fr);
                num -= (int64_t)add * (int64_t)total_fr;
                cw[j] += add;
            }
        }
        /* 列起点 */
        ql_coord_t x = content_x;
        for (int j = 0; j < used_cols; j++) {
            cx[j] = x;
            x += cw[j] + gap_c;
        }
    }

    /* ---------------- ③ 量高（列宽已定） ---------------- */
    {
        for (int i = 0; i < used_rows; i++) {
            const QLUIGridTrack t = grid_row_track(doc, g, i, cs);
            rh[i] = grid_track_len(&t, 0, definite_h ? avail_h : 0);
            if (t.max_kind == QL_UI_GT_FIXED || t.max_kind == QL_UI_GT_PERCENT) {
                const ql_coord_t mx = grid_track_len(&t, 1, definite_h ? avail_h : 0);
                if (mx > rh[i])
                    rh[i] = mx;
            }
        }
        int k = 0;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            const QLUIStyle *is = c->style;
            if (is->display == QL_UI_DISPLAY_NONE || is_out_of_flow(is))
                continue;
            const QLGridPlace p = pl[k++];
            ql_coord_t w = 0;
            for (int j = p.col; j < p.col + p.cspan; j++)
                w += cw[j];
            w += gap_c * (p.cspan - 1);
            const ql_coord_t h = layout_node(c, content_x, content_y, w, -1);
            const QLUIGridTrack t = grid_row_track(doc, g, p.row, cs);
            if (t.max_kind != QL_UI_GT_FIXED && t.max_kind != QL_UI_GT_PERCENT &&
                p.rspan == 1) {
                if (h > rh[p.row])
                    rh[p.row] = h;
            }
        }
        /* 跨行的项：把差额均摊到它跨过的那几行 */
        k = 0;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
                continue;
            const QLGridPlace p = pl[k++];
            if (p.rspan <= 1)
                continue;
            ql_coord_t have = gap_r * (p.rspan - 1);
            for (int i = p.row; i < p.row + p.rspan; i++)
                have += rh[i];
            const ql_coord_t need =
                max_content_main(c, 0, content_w, 0) - c->style->margin.top -
                c->style->margin.bottom;
            if (need > have) {
                const ql_coord_t add = (need - have) / (ql_coord_t)p.rspan;
                for (int i = p.row; i < p.row + p.rspan; i++)
                    rh[i] += add;
            }
        }
        /* fr 行：容器高度确定时把剩余高度分掉 */
        if (definite_h) {
            int32_t total_fr = 0;
            int32_t fixed = (int32_t)gap_r * (used_rows > 1 ? used_rows - 1 : 0);
            for (int i = 0; i < used_rows; i++) {
                const QLUIGridTrack t = grid_row_track(doc, g, i, cs);
                fixed += rh[i];
                if (t.max_kind == QL_UI_GT_FR)
                    total_fr += (t.max_val > 0) ? t.max_val : GRID_FR_ONE_DEF;
            }
            int32_t remaining = (int32_t)avail_h - fixed;
            if (remaining > 0 && total_fr > 0) {
                int64_t num = 0;
                for (int i = 0; i < used_rows; i++) {
                    const QLUIGridTrack t = grid_row_track(doc, g, i, cs);
                    if (t.max_kind != QL_UI_GT_FR)
                        continue;
                    const int32_t w = (t.max_val > 0) ? t.max_val : GRID_FR_ONE_DEF;
                    num += (int64_t)remaining * (int64_t)w;
                    ql_coord_t add = (ql_coord_t)(num / (int64_t)total_fr);
                    num -= (int64_t)add * (int64_t)total_fr;
                    rh[i] += add;
                }
            }
        }
        /* 行起点 */
        ql_coord_t y = content_y;
        for (int i = 0; i < used_rows; i++) {
            ry[i] = y;
            y += rh[i] + gap_r;
        }
    }

    /* ---------------- ④ 定位 + 递归 ---------------- */
    {
        int k = 0;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            const QLUIStyle *is = c->style;
            if (is->display == QL_UI_DISPLAY_NONE || is_out_of_flow(is))
                continue;
            const QLGridPlace p = pl[k++];

            ql_coord_t aw = 0, ah = 0;
            for (int j = p.col; j < p.col + p.cspan; j++)
                aw += cw[j];
            aw += gap_c * (p.cspan - 1);
            for (int i = p.row; i < p.row + p.rspan; i++)
                ah += rh[i];
            ah += gap_r * (p.rspan - 1);

            const ql_coord_t mh = (ql_coord_t)(is->margin.left + is->margin.right);
            const ql_coord_t mv = (ql_coord_t)(is->margin.top + is->margin.bottom);
            const ql_coord_t cell_w = (aw - mh > 0) ? (aw - mh) : 0;
            const ql_coord_t cell_h = (ah - mv > 0) ? (ah - mv) : 0;

            /* 横向：`justify-self` → 容器的 `justify-items`；默认 stretch */
            const uint8_t jit = (is->justify_self != QL_UI_ALIGN_SELF_AUTO)
                                    ? is->justify_self
                                    : cs->justify_items;
            /* 纵向：`align-self` → 容器的 `align-items`；默认 stretch */
            const uint8_t ait = (is->align_self != QL_UI_ALIGN_SELF_AUTO)
                                    ? is->align_self
                                    : cs->align_items;

            ql_coord_t own_w = max_content_main(c, 1, cell_w, 0) - mh;
            if (own_w < 0)
                own_w = 0;
            ql_coord_t own_h = c->border_box.h;   /* 量高那一趟留下的 */
            if (own_h <= 0)
                own_h = cell_h;

            ql_coord_t fw = (jit == QL_UI_ALIGN_STRETCH) ? cell_w : own_w;
            ql_coord_t fh = (ait == QL_UI_ALIGN_STRETCH) ? cell_h : own_h;
            if (fw > cell_w)
                fw = cell_w;
            if (fh > cell_h && ait != QL_UI_ALIGN_STRETCH)
                fh = cell_h;

            ql_coord_t off_x = 0, off_y = 0;
            if (jit == QL_UI_ALIGN_CENTER)
                off_x = (cell_w - fw) / 2;
            else if (jit == QL_UI_ALIGN_END)
                off_x = cell_w - fw;
            if (ait == QL_UI_ALIGN_CENTER)
                off_y = (cell_h - fh) / 2;
            else if (ait == QL_UI_ALIGN_END)
                off_y = cell_h - fh;
            if (off_x < 0)
                off_x = 0;
            if (off_y < 0)
                off_y = 0;

            force_size(c, fw, 1, 1, fh, 1);
            (void)layout_node(c, cx[p.col] + is->margin.left + off_x,
                              ry[p.row] + is->margin.top + off_y, fw, -1);
        }
    }

    ui->grid_place_used = saved_place;
    ui->grid_size_used = saved_coord;
    return ry[used_rows - 1] + rh[used_rows - 1] - content_y;

block_fallback:
    {
        ql_coord_t cy = content_y;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            cy += layout_node(c, content_x, cy, content_w, definite_h ? avail_h : -1);
        }
        return cy - content_y;
    }
}

#else   /* !QL_FEAT_CSS_GRID */

static ql_coord_t layout_grid_children(QLUIRoot *ui, QLUINodeRT *node,
                                       ql_coord_t content_x, ql_coord_t content_y,
                                       ql_coord_t content_w, ql_coord_t avail_h,
                                       int definite_h)
{
    (void)ui;
    (void)avail_h;
    (void)definite_h;
    ql_coord_t cy = content_y;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        cy += layout_node(c, content_x, cy, content_w, -1);
    }
    return cy - content_y;
}

#endif  /* QL_FEAT_CSS_GRID */

/* ------------------------------ B7 表格 ------------------------------ */
/*
 * 表格布局（`table-layout: fixed` 语义）。
 *
 * CSS 的表格是**两套 display 交叉**的结构：`table` / `row-group` / `row` / `cell`，
 * 但只有 cell 有真正的盒子，row 与 row-group 只是"分组"。所以这里的做法是：
 *
 *   1. 收集行（展开 thead/tbody/tfoot），数出一共有几列；
 *   2. 定列宽：首行里显式写了 `width` 的单列单元格优先，其余列**等分**剩余宽度；
 *   3. 量行高：单行单元格各自布局一次，取最大值；跨行单元格在轮完之后再补差额；
 *   4. 摆：按列宽/行高把每个单元格布局到位，再把单元格高度撑到整行，
 *      内容按 `vertical-align` 上/中/下对齐。
 *
 * 明确不做的（见文档）：`table-layout: auto` 的内容自适应、`colgroup`/`col` 的
 * 列宽声明、列高平衡（那是多栏的事）、`caption-side: bottom`。
 */
typedef struct {
    QLUINodeRT *rows[QL_CFG_UI_TABLE_MAX_ROWS];
    int         n;
    int         truncated;
} TableRows;

/*
 * 每一列"还被上面某个 rowspan 占着几行"。
 *
 * 为什么要有它：`<td rowspan=2>` 让下一行**少一个格子**，后面所有单元格都要
 * 往右顺延一列。只看"本行第几个格子"会把它们排到错误的列上（实测：第二行
 * 的第一个单元格会跑到第一列，压住上一行跨行的那个格子）。
 *
 * 用一个 24 字节的计数器数组（不是 64×24 的矩阵）就够了：行是按顺序处理的，
 * 每行结束时把还大于 0 的减一。
 */
typedef struct {
    uint8_t live[QL_CFG_UI_TABLE_MAX_COLS];
} TableOcc;

static int is_table_container(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_TABLE;
}

static int is_table_row(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_TABLE_ROW;
}

static int is_table_row_group(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_TABLE_ROW_GROUP;
}

static int is_table_cell(const QLUIStyle *s)
{
    return s->display == QL_UI_DISPLAY_TABLE_CELL;
}

/* 收集表格的行：直接是 tr 的，和包在 row-group 里的（组本身不画，只透传） */
static void table_collect_rows(QLUINodeRT *table, TableRows *out)
{
    out->n = 0;
    out->truncated = 0;
    for (QLUINodeRT *c = table->first_child; c != NULL; c = c->next_sibling) {
        const QLUIStyle *cs = c->style;
        if (cs->display == QL_UI_DISPLAY_NONE || is_out_of_flow(cs))
            continue;
        if (is_table_row(cs)) {
            if (out->n >= (int)QL_CFG_UI_TABLE_MAX_ROWS) {
                out->truncated = 1;
                return;
            }
            out->rows[out->n++] = c;
        } else if (is_table_row_group(cs)) {
            for (QLUINodeRT *r = c->first_child; r != NULL; r = r->next_sibling) {
                if (r->style->display == QL_UI_DISPLAY_NONE || !is_table_row(r->style))
                    continue;
                if (out->n >= (int)QL_CFG_UI_TABLE_MAX_ROWS) {
                    out->truncated = 1;
                    return;
                }
                out->rows[out->n++] = r;
            }
        }
    }
}

/* 跳过被上面 rowspan 占住的列，返回第一个空列 */
static int occ_free(const TableOcc *o, int from, int ncol)
{
    while (from < ncol && o->live[from] > 0)
        from++;
    return from;
}

/* 占住 [col, col+cs) 这些列，rs 行 */
static void occ_take(TableOcc *o, int col, int cs, int rs, int ncol)
{
    for (int k = col; k < col + cs && k < ncol; k++)
        o->live[k] = (uint8_t)((rs > 1) ? rs : 0);
}

/* 行末：所有占用计数减一（下一行继续占的那些还留着） */
static void occ_end_row(TableOcc *o, int ncol)
{
    for (int k = 0; k < ncol; k++)
        if (o->live[k] > 0)
            o->live[k]--;
}

/* 单元格声明的宽度（边框盒 px）；没写或不是 px 返回 0 */
static ql_coord_t table_cell_fixed_w(const QLUINodeRT *cell)
{
    const QLUIStyle *cs = cell->style;
    if (cs->width.unit != QL_UI_UNIT_PX || cs->width.value <= 0)
        return 0;
    ql_coord_t w = (ql_coord_t)cs->width.value;
    if (cs->box_sizing == 0)
        w += (ql_coord_t)(cs->padding.left + cs->padding.right) + ql_ui_border_h(cs);
    return w;
}

/*
 * min-content 宽度的近似：**最长的一段连续非空白**。
 *
 * 我们没有"词"的概念（折行是按空格/字符判的），所以拿最长的那一段当硬下界：
 * 再窄就得从单词中间断开，而 `ql_text_wrap` 只在"一个词比整行还宽"时才硬断。
 */
static ql_coord_t text_longest_word_w(const QLFont *font, const char *text,
                                      const QLUIStyle *s)
{
    if (font == NULL || text == NULL)
        return 0;
    ql_coord_t best = 0;
    const char *p = text;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        const char *q = p;
        while (*q != '\0' && *q != ' ' && *q != '\t' && *q != '\n')
            q++;
        if (q > p) {
            const ql_coord_t w = ql_font_line_width_ls2(font, p, (int32_t)(q - p),
                                                        (ql_coord_t)s->letter_spacing,
                                                        (ql_coord_t)s->word_spacing,
                                                        s->tab_size);
            if (w > best)
                best = w;
        }
        if (*q == '\0')
            break;
        p = q + 1;
    }
    return best;
}

/*
 * min-content 宽（含 padding/border/margin）——`table-layout: auto` 给列宽的**下界**。
 *
 * 用"最长单词"而不是"逐字最小"：后者会让每列都能压到一个字宽，
 * 结果所有列一样宽，等于没做 auto。
 */
static ql_coord_t min_content_main(const QLUINodeRT *node, int depth)
{
    if (node == NULL || depth > (int)QL_CFG_UI_MAX_DEPTH)
        return 0;
    const QLUIStyle *s = node->style;
    if (s->display == QL_UI_DISPLAY_NONE)
        return 0;
    const ql_coord_t pad_border =
        (ql_coord_t)(s->padding.left + s->padding.right + ql_ui_border_h(s));
    const ql_coord_t margin = (ql_coord_t)(s->margin.left + s->margin.right);

    if (s->width.unit == QL_UI_UNIT_PX) {
        ql_coord_t w = (ql_coord_t)s->width.value;
        if (s->box_sizing == 0)
            w += pad_border;
        return w + margin;
    }
    if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG) {
        ql_coord_t iw = 0, ih = 0;
        image_node_size(node->owner, node, &iw, &ih);
        return iw + pad_border + margin;
    }
    if (node->first_child == NULL)
        return text_longest_word_w(font_of_style(node, s), node->text, s) + pad_border +
               margin;

    /* 有子节点：块级堆叠时 min-content 取子节点的最大值（行内堆叠也按最大值近似） */
    ql_coord_t best = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        const ql_coord_t v = min_content_main(c, depth + 1);
        if (v > best)
            best = v;
    }
    return best + pad_border + margin;
}

/* `<col>` / `<colgroup>` 上的 `width`（px 或表格宽度的百分比）；0 = 没写 */
static ql_coord_t table_col_hint(const QLUINodeRT *col, ql_coord_t table_w)
{
    const QLUIStyle *cs = col->style;
    if (cs->width.unit == QL_UI_UNIT_PX && cs->width.value > 0)
        return (ql_coord_t)cs->width.value;
    if (cs->width.unit == QL_UI_UNIT_PERCENT && cs->width.value > 0)
        return (ql_coord_t)(((int32_t)table_w * (int32_t)cs->width.value) / 100);
    return 0;
}

/* 把 `<colgroup>` / `<col>` 声明的列宽收进 hints[]（span 展开，按文档顺序） */
static void table_collect_hints(QLUINodeRT *table, ql_coord_t table_w,
                                ql_coord_t *hints, int ncol)
{
    for (int i = 0; i < ncol; i++)
        hints[i] = 0;
    int ci = 0;
    for (QLUINodeRT *c = table->first_child; c != NULL && ci < ncol;
         c = c->next_sibling) {
        const uint8_t d = c->style->display;
        if (d == QL_UI_DISPLAY_TABLE_COLUMN) {
            const int span = (c->src->colspan > 0) ? c->src->colspan : 1;
            const ql_coord_t w = table_col_hint(c, table_w);
            for (int k = 0; k < span && ci + k < ncol; k++)
                hints[ci + k] = w;
            ci += span;
        } else if (d == QL_UI_DISPLAY_TABLE_COLUMN_GROUP) {
            /* 组本身可以带 width（等价于组内每个 col 都用它），组内的 col 依次排 */
            const ql_coord_t gw = table_col_hint(c, table_w);
            for (QLUINodeRT *k = c->first_child; k != NULL && ci < ncol;
                 k = k->next_sibling) {
                if (k->style->display != QL_UI_DISPLAY_TABLE_COLUMN)
                    continue;
                const int span = (k->src->colspan > 0) ? k->src->colspan : 1;
                ql_coord_t w = table_col_hint(k, table_w);
                if (w == 0)
                    w = gw;
                for (int j = 0; j < span && ci + j < ncol; j++)
                    hints[ci + j] = w;
                ci += span;
            }
        }
    }
}

/*
 * 单元格**内容**自然高度（不按它自己声明的 height 算）。
 *
 * 为什么要单独算：`vertical-align: middle` 要居中的是"内容相对整行多出来的空间"，
 * 而单元格自己写的 `height: 60px` 会把 border_box 撑到 60，直接拿它当内容高
 * 就永远算不出余量（结果 middle 和 top 一模一样）。
 */
static ql_coord_t table_cell_content_h(const QLUINodeRT *cell, ql_coord_t avail_w)
{
    const QLUIStyle *cs = cell->style;
    if (cell->first_child != NULL) {
        ql_coord_t bottom = cell->content_box.y;
        for (QLUINodeRT *c = cell->first_child; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            const ql_coord_t b = c->border_box.y + c->border_box.h +
                                 (ql_coord_t)(c->style->margin.top + c->style->margin.bottom);
            if (b > bottom)
                bottom = b;
        }
        return bottom - cell->content_box.y;
    }
    if (!has_shown_text(cell))
        return 0;
    const QLFont *font = font_of_style(cell, cs);
    const ql_coord_t lh = text_line_h_of(cs, font);
    const QLTextWrapOpt wopt = wrap_opt_of(cs);
    int lines = ql_text_wrap_opt(font, cell->text,
                                 QL_UI_WS_WRAPS(cs->white_space) ? avail_w : 0, &wopt,
                                 NULL, 0, 0);
    if (lines < 1)
        lines = 1;
    return lh * (ql_coord_t)lines;
}

/* 把单元格高度撑到 `h`（边框盒），并按 vertical-align 摆内容 */
static void table_fit_cell(QLUINodeRT *cell, ql_coord_t h)
{
    const QLUIStyle *cs = cell->style;
    const ql_coord_t inner =
        (ql_coord_t)(cs->padding.top + cs->padding.bottom) + ql_ui_border_v(cs);
    const ql_coord_t used_border_h = cell->border_box.h;
    if (h < used_border_h)
        return;                       /* 内容比这一行还高：让它溢出去（不压扁） */
    const ql_coord_t used_content = table_cell_content_h(cell, cell->content_box.w);
    const ql_coord_t avail = h - inner;
    cell->border_box.h = h;
    cell->content_box.h = (avail > 0) ? avail : 0;

    ql_coord_t dy = 0;
    if (cs->valign == QL_UI_VALIGN_MIDDLE) {
        dy = (avail - used_content) / 2;
    } else if (cs->valign == QL_UI_VALIGN_BOTTOM) {
        dy = avail - used_content;
    }
    if (dy > 0) {
        if (cell->first_child != NULL) {
            for (QLUINodeRT *c = cell->first_child; c != NULL; c = c->next_sibling)
                translate_subtree(c, 0, dy);
        } else {
            /* 文字直接挂在单元格上：挪内容盒，绘制时文字跟着走 */
            cell->content_box.y += dy;
        }
    }
}

/*
 * 单元格的宽度**由列宽决定**，不是它自己写的 `width`（与浏览器一致：列宽算好之后
 * 单元格必须填满整列，`td{width:250px}` 在 fixed 下只是"这一列要 250"的申请）。
 * 所以这里用 `style_copy` 把宽度改写成列宽，再让 layout_node 照常排内容。
 */
static void table_force_cell_w(QLUINodeRT *cell, ql_coord_t w)
{
    if (w <= 0)
        return;
    const QLUIStyle *cs = cell->style;
    if (cs->box_sizing == 1 && cs->width.unit == QL_UI_UNIT_PX &&
        (ql_coord_t)cs->width.value == w)
        return;
    if (ensure_style_copy(cell) == NULL)
        return;
    cell->style_copy->box_sizing = 1;
    cell->style_copy->width.unit = QL_UI_UNIT_PX;
    cell->style_copy->width.value = (int16_t)w;
}

static ql_coord_t layout_table_children(QLUIRoot *ui, QLUINodeRT *table,
                                        ql_coord_t content_x, ql_coord_t content_y,
                                        ql_coord_t content_w, int definite_h)
{
    (void)ui;
    (void)definite_h;
    const QLUIStyle *s = table->style;

#if QL_FEAT_CSS_TABLE
    TableRows rows;
    table_collect_rows(table, &rows);
    if (rows.truncated)
        QL_LOGW("ui: 表格超过 %d 行（QL_CFG_UI_TABLE_MAX_ROWS），后面的行不参与布局",
                (int)QL_CFG_UI_TABLE_MAX_ROWS);

    /* caption（表格上方那一行说明）先排 */
    ql_coord_t y = content_y;
    for (QLUINodeRT *c = table->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_TABLE_CAPTION) {
            y += layout_node(c, content_x, y, content_w, -1);
            break;
        }
    }

    if (rows.n == 0)
        return y - content_y;

    const int collapse = (s->border_collapse != 0);
    const ql_coord_t gap = collapse ? 0 : (ql_coord_t)s->table_spacing_x;
    const int maxcol = (int)QL_CFG_UI_TABLE_MAX_COLS;

    /* ---- 1. 列数（要按 rowspan 顺延后的真实列号算） ---- */
    int ncol = 0;
    {
        TableOcc occ;
        memset(&occ, 0, sizeof(occ));
        for (int r = 0; r < rows.n; r++) {
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[r]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, maxcol);
                if (ci >= maxcol)
                    break;
                occ_take(&occ, ci, cs, rs, maxcol);
                ci += cs;
                if (ci > ncol)
                    ncol = ci;
            }
            occ_end_row(&occ, ncol < maxcol ? ncol : maxcol);
        }
    }
    if (ncol > maxcol)
        ncol = maxcol;
    if (ncol <= 0)
        return y - content_y;

    /* ---- 2. 列宽 ----
     *
     * 两条路：
     *   - `table-layout: fixed`：首行（或 `<col>`）说了算，其余列等分（**只排一遍**）；
     *   - `table-layout: auto`（默认）：先量每列的 min/max-content，再在两者之间
     *     按可用宽度插值（要多遍测量，但列宽跟着内容走）。
     *
     * `<col>` / `<colgroup>` 的 `width` 两条路都认：fixed 里它优先于首行，
     * auto 里它是那一列的**起点**（不会被内容压得更窄）。
     */
    ql_coord_t colw[QL_CFG_UI_TABLE_MAX_COLS];
    ql_coord_t hints[QL_CFG_UI_TABLE_MAX_COLS];
    for (int i = 0; i < ncol; i++)
        colw[i] = 0;
    table_collect_hints(table, content_w, hints, ncol);

    /* 可用宽度减掉列间距（separate 时有 ncol+1 条缝） */
    ql_coord_t avail = content_w - (collapse ? 0 : gap * (ncol + 1));
    if (avail < 0)
        avail = 0;

    if (s->table_layout == 0) {
        /* ---------- auto：min/max-content 之间插值 ---------- */
        ql_coord_t pref[QL_CFG_UI_TABLE_MAX_COLS];
        ql_coord_t floorw[QL_CFG_UI_TABLE_MAX_COLS];
        for (int i = 0; i < ncol; i++) {
            pref[i] = hints[i];
            floorw[i] = hints[i];
        }

        /* 1) 单列单元格的 min/max-content */
        for (int r = 0; r < rows.n; r++) {
            TableOcc occ;
            memset(&occ, 0, sizeof(occ));
            for (int k = 0; k < r; k++) {
                int ci = 0;
                for (QLUINodeRT *cell = rows.rows[k]->first_child; cell != NULL;
                     cell = cell->next_sibling) {
                    if (!is_table_cell(cell->style) ||
                        cell->style->display == QL_UI_DISPLAY_NONE)
                        continue;
                    const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                    const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                    ci = occ_free(&occ, ci, ncol);
                    if (ci >= ncol)
                        break;
                    occ_take(&occ, ci, cs, rs, ncol);
                    ci += cs;
                }
                occ_end_row(&occ, ncol);
            }
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[r]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                if (cs == 1) {
                    const ql_coord_t mx = max_content_main(cell, 1, content_w, 0);
                    const ql_coord_t mn = min_content_main(cell, 0);
                    if (mx > pref[ci])
                        pref[ci] = mx;
                    if (mn > floorw[ci])
                        floorw[ci] = mn;
                }
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
        }

        /* 2) 跨列单元格：不足的部分均摊到它覆盖的列上 */
        for (int r = 0; r < rows.n; r++) {
            TableOcc occ;
            memset(&occ, 0, sizeof(occ));
            for (int k = 0; k < r; k++) {
                int ci = 0;
                for (QLUINodeRT *cell = rows.rows[k]->first_child; cell != NULL;
                     cell = cell->next_sibling) {
                    if (!is_table_cell(cell->style) ||
                        cell->style->display == QL_UI_DISPLAY_NONE)
                        continue;
                    const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                    const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                    ci = occ_free(&occ, ci, ncol);
                    if (ci >= ncol)
                        break;
                    occ_take(&occ, ci, cs, rs, ncol);
                    ci += cs;
                }
                occ_end_row(&occ, ncol);
            }
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[r]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                if (cs > 1) {
                    const ql_coord_t inner_gap = collapse ? 0 : gap * (cs - 1);
                    const ql_coord_t mx = max_content_main(cell, 1, content_w, 0);
                    const ql_coord_t mn = min_content_main(cell, 0);
                    ql_coord_t have = inner_gap;
                    ql_coord_t havemin = inner_gap;
                    int span = 0;
                    for (int k = ci; k < ci + cs && k < ncol; k++) {
                        have += pref[k];
                        havemin += floorw[k];
                        span++;
                    }
                    if (span > 0) {
                        const ql_coord_t add = (mx > have) ? (mx - have + span - 1) / span : 0;
                        const ql_coord_t addmin =
                            (mn > havemin) ? (mn - havemin + span - 1) / span : 0;
                        for (int k = ci; k < ci + cs && k < ncol; k++) {
                            pref[k] += add;
                            floorw[k] += addmin;
                        }
                    }
                }
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
        }

        /* 3) 在 [min, max] 之间按可用宽度插值 */
        ql_coord_t sum_pref = 0, sum_floor = 0;
        for (int i = 0; i < ncol; i++) {
            if (floorw[i] > pref[i])
                pref[i] = floorw[i];
            sum_pref += pref[i];
            sum_floor += floorw[i];
        }
        if (sum_pref <= 0) {
            /* 空表格：等分 */
            for (int i = 0; i < ncol; i++)
                colw[i] = avail / ncol;
            for (int i = 0; i < (int)(avail % ncol); i++)
                colw[i] += 1;
        } else if (avail >= sum_pref) {
            /*
             * 内容比容器窄：多出来的宽度**按 max-content 的比例**分掉
             * （浏览器给 `display: table` 的宽度也来自这里；本库表格式块级，
             *   盒子仍是容器宽度，所以只能把余量摊给列）。
             */
            const ql_coord_t extra = avail - sum_pref;
            ql_coord_t given = 0;
            for (int i = 0; i < ncol; i++) {
                const ql_coord_t add = (i == ncol - 1)
                                           ? (extra - given)
                                           : (ql_coord_t)((int64_t)extra * pref[i] / sum_pref);
                colw[i] = pref[i] + add;
                given += add;
            }
        } else if (avail <= sum_floor) {
            for (int i = 0; i < ncol; i++)
                colw[i] = floorw[i];          /* 压到最小：宁可溢出 */
        } else {
            /* 中间：按 [min, max] 线性插值 */
            const ql_coord_t room = sum_pref - sum_floor;
            const ql_coord_t have = avail - sum_floor;
            ql_coord_t given = 0;
            for (int i = 0; i < ncol; i++) {
                const ql_coord_t span = pref[i] - floorw[i];
                ql_coord_t add = (span > 0)
                                     ? (ql_coord_t)((int64_t)have * span / room)
                                     : 0;
                if (i == ncol - 1)
                    add = have - given;
                colw[i] = floorw[i] + add;
                given += add;
            }
        }
    } else {
        /* ---------- fixed：首行（或 <col>）说了算，其余等分 ---------- */
        int fixed_col[QL_CFG_UI_TABLE_MAX_COLS];
        for (int i = 0; i < ncol; i++) {
            colw[i] = hints[i];
            fixed_col[i] = (hints[i] > 0) ? 1 : 0;
        }
        {
            TableOcc occ;
            memset(&occ, 0, sizeof(occ));
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[0]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                if (!fixed_col[ci]) {
                    const ql_coord_t fw = table_cell_fixed_w(cell);
                    if (cs == 1 && fw > 0) {
                        colw[ci] = fw;
                        fixed_col[ci] = 1;
                    }
                }
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
        }
        ql_coord_t fixed_sum = 0;
        int auto_n = 0;
        for (int i = 0; i < ncol; i++) {
            if (fixed_col[i])
                fixed_sum += colw[i];
            else
                auto_n++;
        }
        if (auto_n > 0) {
            ql_coord_t rest = avail - fixed_sum;
            if (rest < 0)
                rest = 0;
            const ql_coord_t each = rest / auto_n;
            ql_coord_t extra = rest - each * auto_n;      /* 余数给右边那些列 */
            for (int i = 0; i < ncol; i++) {
                if (fixed_col[i])
                    continue;
                colw[i] = each;
                if (extra > 0) {
                    colw[i] += 1;
                    extra--;
                }
            }
        } else if (fixed_sum > 0 && fixed_sum < avail) {
            /*
             * 所有列都写死了，但加起来比表格窄：余量**按各列宽度的比例**摊回去
             * （和浏览器一致；不这么做的话余量全堆到最后一列，看起来像 bug）。
             */
            const ql_coord_t extra = avail - fixed_sum;
            ql_coord_t given = 0;
            for (int i = 0; i < ncol; i++) {
                const ql_coord_t add =
                    (i == ncol - 1) ? (extra - given)
                                    : (ql_coord_t)((int64_t)extra * colw[i] / fixed_sum);
                colw[i] += add;
                given += add;
            }
        }
    }

    /* 跨列宽：覆盖的列宽之和 + 中间的列间距 */
    /* （单独抽出来，因为三处都要算） */
    /* ---- 3. 行高：先看单行单元格 ---- */
    ql_coord_t rowh[QL_CFG_UI_TABLE_MAX_ROWS];
    for (int r = 0; r < rows.n; r++)
        rowh[r] = 0;

    for (int r = 0; r < rows.n; r++) {
        TableOcc occ;
        memset(&occ, 0, sizeof(occ));
        /* 先把前面各行"占用"的状态补出来（只关心本行开始时哪些列被占） */
        for (int k = 0; k < r; k++) {
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[k]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
            occ_end_row(&occ, ncol);
        }
        int ci = 0;
        for (QLUINodeRT *cell = rows.rows[r]->first_child; cell != NULL;
             cell = cell->next_sibling) {
            if (!is_table_cell(cell->style) ||
                cell->style->display == QL_UI_DISPLAY_NONE)
                continue;
            const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
            const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
            ci = occ_free(&occ, ci, ncol);
            if (ci >= ncol)
                break;
            ql_coord_t w = 0;
            for (int k = ci; k < ci + cs && k < ncol; k++)
                w += colw[k];
            if (!collapse && cs > 1)
                w += gap * (cs - 1);
            if (rs == 1) {
                const ql_coord_t h = layout_node(cell, 0, 0, w, -1);
                if (h > rowh[r])
                    rowh[r] = h;
            }
            occ_take(&occ, ci, cs, rs, ncol);
            ci += cs;
        }
    }

    /* ---- 4. 跨行单元格：不够高就把差额补给覆盖范围内的最后一行 ---- */
    for (int r = 0; r < rows.n; r++) {
        TableOcc occ;
        memset(&occ, 0, sizeof(occ));
        for (int k = 0; k < r; k++) {
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[k]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
            occ_end_row(&occ, ncol);
        }
        int ci = 0;
        for (QLUINodeRT *cell = rows.rows[r]->first_child; cell != NULL;
             cell = cell->next_sibling) {
            if (!is_table_cell(cell->style) ||
                cell->style->display == QL_UI_DISPLAY_NONE)
                continue;
            const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
            const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
            ci = occ_free(&occ, ci, ncol);
            if (ci >= ncol)
                break;
            const int col0 = ci;
            occ_take(&occ, ci, cs, rs, ncol);
            ci += cs;
            if (rs <= 1)
                continue;
            int last = r + rs - 1;
            if (last >= rows.n)
                last = rows.n - 1;
            if (last <= r)
                continue;
            ql_coord_t w = 0;
            for (int k = col0; k < col0 + cs && k < ncol; k++)
                w += colw[k];
            if (!collapse && cs > 1)
                w += gap * (cs - 1);
            const ql_coord_t need = layout_node(cell, 0, 0, w, -1);
            ql_coord_t have = collapse ? 0 : gap * (last - r);
            for (int k = r; k <= last; k++)
                have += rowh[k];
            if (need > have)
                rowh[last] += need - have;
        }
    }

    /* ---- 5. 摆放 ---- */
    ql_coord_t row_y = y + (collapse ? 0 : gap);
    for (int r = 0; r < rows.n; r++) {
        QLUINodeRT *row = rows.rows[r];
        TableOcc occ;
        memset(&occ, 0, sizeof(occ));
        for (int k = 0; k < r; k++) {
            int ci = 0;
            for (QLUINodeRT *cell = rows.rows[k]->first_child; cell != NULL;
                 cell = cell->next_sibling) {
                if (!is_table_cell(cell->style) ||
                    cell->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
                const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
                ci = occ_free(&occ, ci, ncol);
                if (ci >= ncol)
                    break;
                occ_take(&occ, ci, cs, rs, ncol);
                ci += cs;
            }
            occ_end_row(&occ, ncol);
        }

        int ci = 0;
        for (QLUINodeRT *cell = row->first_child; cell != NULL;
             cell = cell->next_sibling) {
            if (!is_table_cell(cell->style) ||
                cell->style->display == QL_UI_DISPLAY_NONE)
                continue;
            const int cs = (cell->src->colspan > 0) ? cell->src->colspan : 1;
            const int rs = (cell->src->rowspan > 0) ? cell->src->rowspan : 1;
            ci = occ_free(&occ, ci, ncol);
            if (ci >= ncol)
                break;

            /* 起始 x：前面各列的宽度 + 列间距 */
            ql_coord_t x = content_x + (collapse ? 0 : gap);
            for (int k = 0; k < ci; k++)
                x += colw[k] + (collapse ? 0 : gap);
            ql_coord_t w = 0;
            for (int k = ci; k < ci + cs && k < ncol; k++)
                w += colw[k];
            if (!collapse && cs > 1)
                w += gap * (cs - 1);

            int last = r + rs - 1;
            if (last >= rows.n)
                last = rows.n - 1;
            ql_coord_t h = rowh[r];
            for (int k = r + 1; k <= last; k++)
                h += rowh[k] + (collapse ? 0 : gap);
            if (h <= 0)
                h = rowh[r];

            /*
             * collapse：把单元格朝左上挪"自己那半条边框"，同时把尺寸补回同样的量
             * —— 相邻两条边框重叠成一条，右/下边缘仍落在原来的网格线上。
             * 第一列/第一行不挪（否则表格外框会往外跑）。
             */
            ql_coord_t cx2 = x;
            ql_coord_t cy2 = row_y;
            ql_coord_t w2 = w;
            ql_coord_t h2 = h;
            if (collapse) {
                if (ci > 0) {
                    const ql_coord_t bl = (ql_coord_t)cell->style->border_w[3];
                    cx2 -= bl;
                    w2 += bl;
                }
                if (r > 0) {
                    const ql_coord_t bt = (ql_coord_t)cell->style->border_w[0];
                    cy2 -= bt;
                    h2 += bt;
                }
            }
            table_force_cell_w(cell, w2);
            (void)layout_node(cell, cx2, cy2, w2, -1);
            table_fit_cell(cell, h2);

            occ_take(&occ, ci, cs, rs, ncol);
            ci += cs;
        }

        /* 行本身的盒子（定位/裁剪/绘制上界要用）：整行宽度 + 这一行的高度 */
        row->border_box = ql_rect_make(content_x, row_y, content_w, rowh[r]);
        row->content_box = row->border_box;
        row_y += rowh[r] + (collapse ? 0 : gap);
    }

    /* row-group 的盒子 = 它包含的行的并集（同样只为裁剪/上界服务） */
    for (QLUINodeRT *g = table->first_child; g != NULL; g = g->next_sibling) {
        if (!is_table_row_group(g->style))
            continue;
        QLRect r = ql_rect_make(0, 0, 0, 0);
        int any = 0;
        for (int i = 0; i < rows.n; i++) {
            if (rows.rows[i]->parent != g)
                continue;
            const QLRect b = rows.rows[i]->border_box;
            r = any ? ql_rect_union(r, b) : b;
            any = 1;
        }
        g->border_box = any ? r : ql_rect_make(content_x, content_y, content_w, 0);
        g->content_box = g->border_box;
    }

    return row_y - content_y;
#else
    /* 关掉表格包：退化成普通块流（标签在构建期就已经报错了，这里只是兜底） */
    ql_coord_t cy = content_y;
    for (QLUINodeRT *c = table->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        cy += layout_node(c, content_x, cy, content_w, -1);
    }
    return cy - content_y;
#endif /* QL_FEAT_CSS_TABLE */
}

/* ------------------------------ 多栏（column-*） ------------------------------ */

static int is_multicol(const QLUIStyle *s)
{
#if QL_FEAT_CSS_COLUMNS
    return s->column_count > 0 ||
           (s->column_width.unit != QL_UI_UNIT_AUTO && s->column_width.value > 0);
#else
    (void)s;
    return 0;
#endif
}

#if QL_FEAT_CSS_COLUMNS
/* 一个参与多栏的子节点的"推进高度"（第一次量过之后就能直接算，不重排） */
static ql_coord_t column_item_adv(const QLUINodeRT *c)
{
    return c->border_box.h + c->style->margin.top + c->style->margin.bottom;
}

/* 用栏高 `target` 贪心分栏，需要几栏（空格子也算一栏；栏高变大会单调变少） */
static int columns_needed(QLUINodeRT *node, ql_coord_t target)
{
    int cols = 1;
    ql_coord_t used = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
            continue;
        const ql_coord_t adv = column_item_adv(c);
        if (used > 0 && used + adv > target) {
            cols++;
            used = 0;
        }
        used += adv;
    }
    return cols;
}

/*
 * 多栏容器：**贪心分栏**。
 *
 *   1. 先按栏宽把每个子节点在 (0,0) 排一遍，累加得到内容总高；
 *   2. 容器有确定高度就用它当栏高，否则按 `ceil(总高 / 栏数)` 均分（"等高栏"）；
 *   3. 按顺序往当前栏里塞，塞不下就换下一栏（空栏至少放一个，避免死循环）。
 *
 * 不支持的东西（明确写进文档，不做静默降级）：
 *   - **不把一个子节点劈到两栏**：一个很高的子节点会把那一栏撑高；
 *   - 行内内容不分栏（多栏容器的子节点按块级处理）；
 *   - `column-span: all` 不支持。
 *
 * 为什么这么实现：本库没有"分页"这套东西，真按行切分要做断行/断箱记录，
 * 收益（长文自动分栏）在嵌入式 UI 里很小，而"卡片列表分两三栏"是常见需求。
 */
static ql_coord_t layout_columns_children(QLUINodeRT *node, ql_coord_t content_x,
                                          ql_coord_t content_y, ql_coord_t content_w,
                                          ql_coord_t decl_h)
{
    const QLUIStyle *s = node->style;
    node->col_n = 1;
    node->col_w = (content_w > 0) ? (int16_t)content_w : 0;
    if (node->first_child == NULL || content_w <= 0)
        return 0;

    ql_coord_t gap = (ql_coord_t)s->gap_col;
    if (gap <= 0)
        gap = 16;                      /* CSS 的 `normal`，按 1em≈16px */
    if (gap > content_w / 2)
        gap = 0;                       /* 缝比容器还宽：直接不要缝 */

    /* 栏数：写了 count 就用 count，写了 width 就按宽度反推，两个都给取小的 */
    int n = (int)s->column_count;
    ql_coord_t want_w = 0;
    if (s->column_width.unit == QL_UI_UNIT_PX) {
        want_w = (ql_coord_t)s->column_width.value;
    } else if (s->column_width.unit == QL_UI_UNIT_PERCENT) {
        want_w = (ql_coord_t)(((int32_t)content_w * (int32_t)s->column_width.value) / 100);
    }
    if (want_w > 0) {
        int fit = (int)((content_w + gap) / (want_w + gap));
        if (fit < 1)
            fit = 1;
        if (n <= 0 || fit < n)
            n = fit;
    }
    if (n < 1)
        n = 1;
    if (n > QL_CFG_UI_COLUMN_MAX)
        n = QL_CFG_UI_COLUMN_MAX;

    ql_coord_t col_w = (content_w - gap * (n - 1)) / n;
    if (col_w < 1)
        col_w = 1;

    /* ---- 1. 量：在原点排一遍（顺带得到每个子节点的机械高度） ---- */
    ql_coord_t total = 0;
    ql_coord_t tallest = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
            continue;
        const ql_coord_t adv = layout_node(c, 0, 0, col_w, -1) +
                               c->style->margin.top + c->style->margin.bottom;
        total += adv;
        if (adv > tallest)
            tallest = adv;
    }

    /*
     * 栏高：容器给了确定高度就用它；否则**找最小能塞进 n 栏的栏高**（"等高栏"）。
     *
     * 为什么不是简单的 ceil(总高/n)：子节点高度不齐时，均分值会让第一栏
     * 连一个高个子都塞不下，于是每栏只放一个、剩下的全挤在最后一栏 —— 看起来
     * 就像"分栏坏了"。二分一个有单调性的谓词（栏高越大，需要的栏数越少）
     * 就能拿到真正的最小值，代价是"纯算术"重扫几遍（不重排）。
     */
    ql_coord_t target = 0;
    if (decl_h > 0) {
        target = decl_h;
    } else if (total > 0) {
        ql_coord_t lo = (tallest > 0) ? tallest : 1;
        ql_coord_t hi = (total > lo) ? total : lo;
        for (int it = 0; it < 12 && lo < hi; it++) {
            const ql_coord_t mid = lo + (hi - lo) / 2;
            if (columns_needed(node, mid) <= n)
                hi = mid;
            else
                lo = mid + 1;
        }
        target = lo;
    }
    if (target < tallest)
        target = tallest;
    if (target <= 0)
        target = 1;

    /*
     * ---- 2. 分配 ----
     * 只做平移（第一次排的位置是 (0,0)，平移量 = 目标位置 - 当前位置），
     * 不重排：多栏里没有百分比高度依赖栏高，重排没有额外信息。
     */
    ql_coord_t cursor = content_y;
    ql_coord_t col_top = content_y;
    int col = 0;
    ql_coord_t used_h = 0;             /* 已用栏数里最高的那一栏 */
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
            continue;
        const ql_coord_t mt = c->style->margin.top;
        const ql_coord_t adv = column_item_adv(c);

        /* 这一栏放不下了 → 换栏（空栏无论如何都放，免得死循环） */
        if (cursor > col_top && (cursor - col_top) + adv > target && col + 1 < n) {
            const ql_coord_t used = cursor - col_top;
            if (used > used_h)
                used_h = used;
            col++;
            col_top = content_y;
            cursor = content_y;
        }
        const ql_coord_t x = content_x + col * (col_w + gap);
        const ql_coord_t y = cursor + mt;
        translate_subtree(c, x - c->border_box.x, y - c->border_box.y);
        cursor += adv;
    }
    if (cursor - col_top > used_h)
        used_h = cursor - col_top;

    node->col_n = (int16_t)n;
    node->col_w = (int16_t)col_w;
    return (used_h > 0) ? used_h : 0;
}
#else
static ql_coord_t layout_columns_children(QLUINodeRT *node, ql_coord_t content_x,
                                          ql_coord_t content_y, ql_coord_t content_w,
                                          ql_coord_t decl_h)
{
    (void)decl_h;
    ql_coord_t cy = content_y;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        cy += layout_node(c, content_x, cy, content_w, -1);
    }
    return cy - content_y;
}
#endif /* QL_FEAT_CSS_COLUMNS */

/* ------------------------------ flex 实现 ------------------------------ */

/* 统计可见子项数；超过上限返回 -1（调用方降级为 block 流） */
static int count_flex_items(QLUINodeRT *node)
{
    int n = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        if (++n > QL_CFG_UI_FLEX_MAX_ITEMS)
            return -1;
    }
    return n;
}

/* 测量阶段：在哑位置跑一遍 layout_node，把自然尺寸写进各子节点的 border_box */
/*
 * flex 项在**主轴**上的 max-content 尺寸（含 padding/border/margin）。
 *
 * 为什么需要它：flex 项的 `width: auto` 语义是 `flex-basis: content`，
 * 也就是"内容需要多大就多大"。而 block 布局里 `width: auto` 是"撑满包含块"。
 * 早先的直接后果是：一个空的 `flex-grow: 1` 占位块会把自己的基准尺寸算成
 * 整个容器的宽度 —— 于是 `sum_base` 超过容器、白算出一份"负剩余空间"，
 * 触发 shrink，把旁边**固定宽度**的兄弟一起压扁
 * （实测：`40px` 变 29px、`56px` 变 40px，而浏览器里这两个值不该动）。
 *
 * 递归有界：树深度已经被 QL_CFG_UI_MAX_DEPTH 限住，这里再加一个深度参数
 * 兜底，避免将来某个环状结构把它拖死。
 */
static ql_coord_t max_content_main(const QLUINodeRT *node, int row, ql_coord_t avail_w,
                                   int depth)
{
    if (node == NULL || depth > (int)QL_CFG_UI_MAX_DEPTH)
        return 0;
    const QLUIStyle *s = node->style;
    if (s->display == QL_UI_DISPLAY_NONE)
        return 0;

    const ql_coord_t pad_border =
        row ? (ql_coord_t)(s->padding.left + s->padding.right + ql_ui_border_h(s))
            : (ql_coord_t)(s->padding.top + s->padding.bottom + ql_ui_border_v(s));
    const ql_coord_t margin =
        row ? (ql_coord_t)(s->margin.left + s->margin.right)
            : (ql_coord_t)(s->margin.top + s->margin.bottom);

    const QLUILength *len = row ? &s->width : &s->height;
    ql_coord_t own = 0;

    if (len->unit == QL_UI_UNIT_PX) {
        own = (ql_coord_t)len->value;
        if (s->box_sizing == 0)
            own += pad_border;
    } else if (len->unit == QL_UI_UNIT_AUTO) {
        if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG) {
            ql_coord_t iw = 0, ih = 0;
            image_node_size(node->owner, node, &iw, &ih);
            own = row ? iw : ih;
        } else if (node->text != NULL && node->first_child == NULL) {
            const QLFont *font = font_of_style(node, s);
            if (row) {
                /* 主轴水平：max-content 就是把文本摊平不折行的宽度 */
                own = (font != NULL)
                          ? ql_font_line_width_ls2(font, node->text, -1,
                                                   (ql_coord_t)s->letter_spacing,
                                                   (ql_coord_t)s->word_spacing,
                                                   s->tab_size)
                          : 0;
            } else {
                /*
                 * 主轴竖直：max-content 高度 = 行数 × 行高。
                 * 折行时行数取决于宽度，而这里只能拿到"父容器给的宽度"
                 * （column 容器 + stretch 时子项宽度就是它）——用它估算，
                 * 与浏览器"先按可得宽度折行、再取高度"的结果一致。
                 *
                 * 之前这里错用了**文本宽度**，于是 column 容器里的每个文本项
                 * 都声称自己有那么高（46 个汉字 322px），整页被撑爆。
                 */
                const ql_coord_t lh = text_line_h_of(s, font);
                int lines = 1;
                if (font != NULL) {
                    ql_coord_t maxw = 0;
                    if (QL_UI_WS_WRAPS(s->white_space)) {
                        /*
                         * 折行基准宽度 = **这一项自己的宽度**（写了 width 就用它，
                         * 否则才退回父容器给的可用宽）。
                         *
                         * 只用 avail_w 会在"column 容器 + 子项 width: 440px"时算错：
                         * 容器宽 848，于是文本按 848 折成 1 行 → 高度 20，
                         * 而绘制时按 440 折成 2 行 → 第二行压到下一个元素上
                         * （例程 36 的 justify 段落就是这么暴露出来的）。
                         */
                        const ql_coord_t pad_h =
                            (ql_coord_t)(s->padding.left + s->padding.right +
                                         ql_ui_border_h(s));
                        const ql_coord_t mar_h =
                            (ql_coord_t)(s->margin.left + s->margin.right);
                        ql_coord_t basis = avail_w;
                        if (s->width.unit == QL_UI_UNIT_PX)
                            basis = (ql_coord_t)s->width.value +
                                    ((s->box_sizing == 0) ? pad_h : 0);
                        else if (s->width.unit == QL_UI_UNIT_PERCENT && avail_w > 0)
                            basis = (ql_coord_t)(((int32_t)avail_w * s->width.value) / 100);
                        ql_coord_t inner = basis - pad_h - mar_h;
                        maxw = (inner > 0) ? inner : 0;
                    }
                    const QLTextWrapOpt wopt = wrap_opt_of(s);
                    lines = ql_text_wrap_opt(font, node->text, maxw, &wopt, NULL, 0, 0);
                    if (lines < 1)
                        lines = 1;
                }
                own = lh * (ql_coord_t)lines;
            }
            own += pad_border;
        } else if (node->first_child == NULL) {
            own = pad_border;              /* 空盒子：内容 0 */
        } else if (is_grid_container(s)) {
            /*
             * 网格容器的 max-content：
             *   主轴水平 → 前 `列数` 个子项的 max-content 之和 + 间距（一行摆得下几个）；
             *   主轴竖直 → 子项里最高的那个（列宽由容器决定）。
             * 这是**估算**，只在"网格容器自己作为别人（flex/auto 列）的子项"时才用到。
             */
            const QLUIDoc *doc = node->owner != NULL ? node->owner->doc : NULL;
            int gcol = 1;
            if (doc != NULL && doc->grids != NULL && s->grid < doc->grid_count) {
                const QLUIGrid *gg = &doc->grids[s->grid];
                if (gg->col_count > 0)
                    gcol = (int)gg->col_count;
                else if (gg->col_hint > 0)
                    gcol = (int)gg->col_hint;
            }
            const ql_coord_t gap = row ? (ql_coord_t)s->gap_col : (ql_coord_t)s->gap_row;
            ql_coord_t sum = 0, mx = 0;
            int n = 0;
            for (QLUINodeRT *c = node->first_child; c != NULL && n < gcol;
                 c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
                    continue;
                const ql_coord_t v =
                    max_content_main(c, row, avail_w - pad_border, depth + 1);
                sum += v;
                if (v > mx)
                    mx = v;
                n++;
            }
            if (row) {
                if (n > 1)
                    sum += gap * (n - 1);
                own = sum + pad_border;
            } else {
                own = mx + pad_border;
            }
        } else if (is_flex_container(s) && flex_is_row(s) == row) {
            /* 同方向的 flex 容器：主轴上是"子项之和 + 间距" */
            ql_coord_t sum = 0;
            int n = 0;
            const ql_coord_t gap = row ? (ql_coord_t)s->gap_col : (ql_coord_t)s->gap_row;
            for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                sum += max_content_main(c, row,
                                        avail_w - pad_border - s->margin.left -
                                            s->margin.right,
                                        depth + 1);
                n++;
            }
            if (n > 1)
                sum += gap * (n - 1);
            own = sum + pad_border;
        } else if (row) {
            /* 主轴水平、子节点纵向堆叠：主轴尺寸 = 子项的**最大宽度** */
            ql_coord_t mx = 0;
            for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
                    continue;
                ql_coord_t v = max_content_main(c, row,
                                                avail_w - pad_border - s->margin.left -
                                                    s->margin.right,
                                                depth + 1);
                if (v > mx)
                    mx = v;
            }
            own = mx + pad_border;
        } else {
            /*
             * 主轴竖直（column flex 的项 / 普通块）：块流子节点是**堆叠**的，
             * 所以主轴尺寸是子项之和，不是最大值。
             *
             * 以前这里对两个方向都取最大值 —— 于是"带 padding、里面有两行文字"的
             * 卡片在 column 布局里的高度被算成**一行**，后续兄弟会压上来
             * （@media 把一排三个改成一列时一眼就能看到）。行方向取最大是对的，
             * 列方向必须求和。
             */
            /*
             * 但**行内**子节点（`display: inline`）是横排一行的 ——
             * 它们要取"行内最高的那个"，不能逐个相加。
             * 不区分的话 `<div><span>a</span><span>b</span></div>`
             * 在 column 布局里的高度会变成 a+b（实测 23+19+23+19=84），
             * 后面的兄弟节点就被顶下去一大截（例程 36 里一眼可见）。
             */
            ql_coord_t sum = 0, row_max = 0;
            int in_row = 0;
            for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE || is_out_of_flow(c->style))
                    continue;
                const ql_coord_t v =
                    max_content_main(c, row,
                                     avail_w - pad_border - s->margin.left -
                                         s->margin.right,
                                     depth + 1);
                if (c->style->display == QL_UI_DISPLAY_BLOCK) {
                    if (in_row) {
                        sum += row_max;
                        row_max = 0;
                        in_row = 0;
                    }
                    sum += v;
                } else {
                    if (v > row_max)
                        row_max = v;
                    in_row = 1;
                }
            }
            if (in_row)
                sum += row_max;
            own = sum + pad_border;
        }
    } else {
        /* 百分比：要先知道包含块，测量阶段拿不到 → 按 0 处理（与浏览器"缩到最小"一致） */
        own = pad_border;
    }

    return own + margin;
}

/*
 * 测量 flex 项：把"自动宽度"的项先按 **max-content** 落成显式尺寸，再跑布局。
 *
 * 不这么做的话 `border_width_of()` 会把 `width: auto` 当成"撑满包含块"
 * （那是 block 布局的语义），于是每个 auto 宽度的 flex 项都声称要整个容器宽度。
 */
static void measure_flex_items(QLUINodeRT *node, ql_coord_t content_w)
{
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;

        /*
         * 只有"没有显式主轴尺寸"的项才需要这一步。它自己还有子节点时，
         * 先按 max-content 定下主轴尺寸，之后 `layout_node` 会按这个尺寸
         * 把内部排完（子节点的换行/撑满都以它为包含块）。
         */
        const QLUILength *len = flex_is_row(node->style) ? &c->style->width
                                                        : &c->style->height;
        if (len->unit == QL_UI_UNIT_AUTO) {
            const ql_coord_t mc =
                max_content_main(c, flex_is_row(node->style), content_w, 0);
            const ql_coord_t mb = flex_is_row(node->style)
                                      ? (ql_coord_t)(c->style->margin.left +
                                                     c->style->margin.right)
                                      : (ql_coord_t)(c->style->margin.top +
                                                     c->style->margin.bottom);
            ql_coord_t box = mc - mb;          /* 去掉 margin 后的 border-box 尺寸 */
            if (box < 0)
                box = 0;

            /*
             * `width` 的解释取决于 box-sizing：
             *   border-box → 直接写 border-box 尺寸；
             *   content-box → 要退掉 padding 与 border（否则布局时再加一遍，
             *                 带边框的项会胖 2 个 border_width）。
             * 只能改这一个字段，**不能顺手把 box_sizing 改成 1** ——
             * 那会把同一项的交叉轴尺寸也一起改掉（踩过：带 1px 边框的
             * 20px 高格子被算成 20 而不是 22）。
             */
            const int row_axis = flex_is_row(node->style);
            const ql_coord_t pad_border =
                row_axis ? (ql_coord_t)(c->style->padding.left + c->style->padding.right +
                                        ql_ui_border_h(c->style))
                         : (ql_coord_t)(c->style->padding.top + c->style->padding.bottom +
                                        ql_ui_border_v(c->style));
            const ql_coord_t len_value =
                (c->style->box_sizing == 1) ? box : (box - pad_border);

            if (ensure_style_copy(c) == NULL)

                return;
            if (row_axis) {
                c->style_copy->width.value = (int16_t)((len_value < 0) ? 0 : len_value);
                c->style_copy->width.unit = QL_UI_UNIT_PX;
            } else {
                c->style_copy->height.value = (int16_t)((len_value < 0) ? 0 : len_value);
                c->style_copy->height.unit = QL_UI_UNIT_PX;
            }
        }

        /*
         * 主轴上的百分比 / `calc()` 尺寸先按**容器的内容尺寸**折成 px 存回去。
         *
         * 不折的话会被解析两遍：`measure` 一遍（基准 = 容器内容宽），真正的
         * `layout_node(c, …, inner_sz, …)` 又拿"这一项自己的尺寸"当基准解析一遍 ——
         * 表现是 `width: 50%` 变成四分之一、`calc(100% - 40px)` 被减两次。
         * 交叉轴的百分比不动（它的基准是行宽，语义还在处理中）。
         */
        else if (len->unit == QL_UI_UNIT_PERCENT || len->unit == QL_UI_UNIT_CALC) {
            const int row_axis = flex_is_row(node->style);
            if (row_axis) {
                const ql_coord_t pad_border =
                    (ql_coord_t)(c->style->padding.left + c->style->padding.right +
                                 ql_ui_border_h(c->style));
                ql_coord_t v = resolve_length(*len, content_w, content_w);
                if (c->style->box_sizing == 0)
                    v -= pad_border;
                if (v < 0)
                    v = 0;
                if (v > 32767)
                    v = 32767;
                if (ensure_style_copy(c) == NULL)
                    return;
                c->style_copy->width.value = (int16_t)v;
                c->style_copy->width.unit = QL_UI_UNIT_PX;
            }
        }

        (void)layout_node(c, 0, 0, content_w, -1);  /* 位置无关紧要，只取尺寸 */
    }
}

/*
 * 按 `order` 稳定排序容器**直接子节点**的链表（CSS 的 flex `order`）。
 *
 * 为什么要就地改链表而不是"遍历时按 order 挑下一个"：布局里有六七处都在
 * 遍历子项（测量、断行、权重、摆放），每处都实现一遍"跳过已处理的"很容易漏。
 * 排序放到最前面，后面所有遍历自动是对的；绘制/命中测试也走同一条链表，
 * 所以视觉顺序与布局顺序天然一致（z-index 相同时）。
 *
 * 稳定性靠插入排序的 `<=`：order 相等的项保持 HTML 顺序（与 CSS 一致）。
 * 每帧布局都会重排一次，但同样的 order 序列排第二次结果不变（幂等）。
 */
static void sort_children_by_order(QLUINodeRT *parent)
{
    QLUINodeRT *cur = parent->first_child;
    int any = 0;
    for (; cur != NULL; cur = cur->next_sibling)
        if (cur->style->order != 0) {
            any = 1;
            break;
        }
    if (!any)
        return;                    /* 没人写 order：一点开销都不花 */

    QLUINodeRT *sorted = NULL;
    cur = parent->first_child;
    while (cur != NULL) {
        QLUINodeRT *next = cur->next_sibling;
        const int o = cur->style->order;
        QLUINodeRT *prev = NULL;
        QLUINodeRT *p = sorted;
        while (p != NULL && p->style->order <= o) {
            prev = p;
            p = p->next_sibling;
        }
        cur->next_sibling = p;
        if (prev == NULL) {
            sorted = cur;
        } else {
            prev->next_sibling = cur;
        }
        cur = next;
    }
    parent->first_child = sorted;
}

/* 项的基尺寸（含 margin）：flex-basis 优先，否则用自然主尺寸 */
static ql_coord_t flex_base_of(const QLUINodeRT *node, int row, ql_coord_t main_size)
{
    const QLUIStyle *s = node->style;
    ql_coord_t margin = row ? (ql_coord_t)(s->margin.left + s->margin.right)
                            : (ql_coord_t)(s->margin.top + s->margin.bottom);

    if (s->flex_basis_pct > 0) {
        ql_coord_t b = (main_size > 0)
                           ? (ql_coord_t)(((int32_t)main_size * (int32_t)s->flex_basis_pct) / 100)
                           : 0;
        return b + margin;
    }
    if (s->flex_basis >= 0)
        return (ql_coord_t)s->flex_basis + margin;
    return natural_main(node, row);
}

/* 汇总一行的基尺寸、grow 总量、加权 shrink 总量 */
static void line_factors(QLUINodeRT *first, QLUINodeRT *last, int row,
                         ql_coord_t main_size, ql_coord_t *sum_base,
                         ql_coord_t *sum_grow, ql_coord_t *sum_shrink_w, int *count)
{
    ql_coord_t base_sum = 0, grow_sum = 0, shrink_sum = 0;
    int n = 0;
    for (QLUINodeRT *c = first; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        ql_coord_t b = flex_base_of(c, row, main_size);
        base_sum += b;
        grow_sum += (ql_coord_t)c->style->flex_grow;
        if (c->style->flex_shrink > 0) {
            uint32_t w = (uint32_t)c->style->flex_shrink *
                         (uint32_t)((b > 0) ? b : 1) / 100u;
            shrink_sum += (ql_coord_t)w;
        }
        n++;
        if (c == last)
            break;
    }
    *sum_base = base_sum;
    *sum_grow = grow_sum;
    *sum_shrink_w = shrink_sum;
    *count = n;
}

/*
 * justify-content → (行首偏移, 待摊入的剩余空间总量, 摊成几份)
 *
 * 注意不是"每份多少"：剩下的空间要**按累计方式**摊到各项之间
 * （见 layout_flex_children 里的 extra_num），否则每份各自取整会把余数丢掉，
 * 最后一项落不到行尾——100px 分三项就是 33+33+33=99，右边留 1px 空隙。
 */
static void justify_offsets(ql_coord_t free_space, int n, uint8_t justify,
                            ql_coord_t *out_start, ql_coord_t *out_extra_total,
                            int *out_extra_div)
{
    *out_start = 0;
    *out_extra_total = 0;
    *out_extra_div = 0;
    if (free_space <= 0 || n <= 0)
        return;

    switch (justify) {
    case QL_UI_JUSTIFY_CENTER:
        *out_start = free_space / 2;
        break;
    case QL_UI_JUSTIFY_END:
        *out_start = free_space;
        break;
    case QL_UI_JUSTIFY_SPACE_BETWEEN:
        if (n > 1) {
            *out_extra_total = free_space;
            *out_extra_div = n - 1;
        }
        break;
    case QL_UI_JUSTIFY_SPACE_AROUND:
        *out_start = free_space / (2 * n);
        *out_extra_total = free_space;
        *out_extra_div = n;
        break;
    case QL_UI_JUSTIFY_SPACE_EVENLY:
        *out_start = free_space / (n + 1);
        *out_extra_total = free_space;
        *out_extra_div = n + 1;
        break;
    default:
        break;
    }
}

static ql_coord_t layout_flex_children(QLUINodeRT *node, ql_coord_t content_x,
                                       ql_coord_t content_y, ql_coord_t content_w,
                                       ql_coord_t avail_h, int definite_h)
{
    const QLUIStyle *cs = node->style;
    const int row = flex_is_row(cs);
    const int reversed = flex_reversed(cs);
    const ql_coord_t gap = row ? (ql_coord_t)cs->gap_col : (ql_coord_t)cs->gap_row;
    const ql_coord_t inner_main = row ? content_w : (definite_h ? avail_h : 0);

    /* `order` 先排好，后面所有遍历（测量/断行/摆放/绘制）自动跟着走 */
    sort_children_by_order(node);

    if (count_flex_items(node) < 0) {
        QL_LOGW("ui: flex has more than %d children, falling back to block flow",
                QL_CFG_UI_FLEX_MAX_ITEMS);
        ql_coord_t cy = content_y;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            cy += layout_node(c, content_x, cy, content_w,
                              definite_h ? avail_h : -1);
        }
        return cy - content_y;
    }

    /* (1) 测量：在哑位置跑一遍，尺寸写进各子节点的 border_box */
    measure_flex_items(node, content_w);


    /*
     * (0) 先量一遍"有几行、每行多高"（只累加，不存数组 —— MCU 上没有栈可用）。
     *     这一步只为 align-content：交叉轴上怎么分配得先知道总高。
     */
    const ql_coord_t cross_gap = row ? (ql_coord_t)cs->gap_row
                                     : (ql_coord_t)cs->gap_col;
    const ql_coord_t container_cross = row ? (definite_h ? avail_h : 0) : content_w;
    ql_coord_t total_cross = 0;
    int nline = 0;
    {
        QLUINodeRT *lb = node->first_child;
        while (lb != NULL) {
            while (lb != NULL && lb->style->display == QL_UI_DISPLAY_NONE)
                lb = lb->next_sibling;
            if (lb == NULL)
                break;
            QLUINodeRT *last = NULL;
            ql_coord_t used = 0, lc = 0;
            int n = 0;
            for (QLUINodeRT *c = lb; c != NULL; c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                const ql_coord_t b = flex_base_of(c, row, inner_main);
                const ql_coord_t need = b + ((n > 0) ? gap : 0);
                if (n > 0 && cs->flex_wrap == 1 && inner_main > 0 &&
                    used + need > inner_main)
                    break;
                const ql_coord_t cr = natural_cross(c, row);
                if (cr > lc)
                    lc = cr;
                last = c;
                used += need;
                n++;
            }
            if (last == NULL)
                break;
            total_cross += lc;
            nline++;
            lb = last->next_sibling;
        }
    }
    if (nline > 1)
        total_cross += cross_gap * (nline - 1);

    /*
     * 交叉轴的行间分配（`align-content`）。
     *
     *   单行 → 行撑满容器（CSS 对单行的特例，也是 align-items 能生效的前提）
     *   多行 → 按 align-content：stretch 把剩余空间平分给各行，其余是"起点 + 间距"
     *
     * 间距一律用**累计推进**，与 justify-content 同一套（余数不丢）。
     */
    ql_coord_t cross_start = 0;
    ql_coord_t cross_extra_total = 0;     /* 行与行之间要摊开的总额 */
    int cross_extra_div = 0;              /* 摊成几份（0 = 不摊） */
    ql_coord_t cross_stretch_extra = 0;   /* 每行要拉伸的总额（stretch） */
    if (nline == 1 && cs->flex_wrap != 1) {
        if (container_cross > total_cross)
            total_cross = container_cross;
    } else if (nline > 1) {
        const ql_coord_t free_cross =
            (container_cross > total_cross) ? (container_cross - total_cross) : 0;
        switch (cs->align_content) {
        case QL_UI_ALIGN_CONTENT_CENTER:
            cross_start = free_cross / 2;
            break;
        case QL_UI_ALIGN_CONTENT_END:
            cross_start = free_cross;
            break;
        case QL_UI_ALIGN_CONTENT_SPACE_BETWEEN:
            cross_extra_total = free_cross;
            cross_extra_div = nline - 1;
            break;
        case QL_UI_ALIGN_CONTENT_SPACE_AROUND:
            cross_start = free_cross / (2 * nline);
            cross_extra_total = free_cross;
            cross_extra_div = nline;
            break;
        case QL_UI_ALIGN_CONTENT_SPACE_EVENLY:
            cross_start = free_cross / (nline + 1);
            cross_extra_total = free_cross;
            cross_extra_div = nline + 1;
            break;
        case QL_UI_ALIGN_CONTENT_STRETCH:
        default:
            cross_stretch_extra = free_cross;
            break;
        }
    }

    ql_coord_t cross_cursor = cross_start;
    int64_t cross_num = 0;      /* 行间距的累计分子 */
    int64_t stretch_num = 0;    /* stretch 份额的累计分子 */
    QLUINodeRT *line_begin = node->first_child;

    while (line_begin != NULL) {
        while (line_begin != NULL && line_begin->style->display == QL_UI_DISPLAY_NONE)
            line_begin = line_begin->next_sibling;
        if (line_begin == NULL)
            break;

        /* (2) 划出本行范围（wrap 时超出就断行） */
        QLUINodeRT *first = NULL;
        QLUINodeRT *last = NULL;
        ql_coord_t used = 0;
        int n = 0;

        for (QLUINodeRT *c = line_begin; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            ql_coord_t b = flex_base_of(c, row, inner_main);
            ql_coord_t need = b + ((n > 0) ? gap : 0);
            if (n > 0 && cs->flex_wrap == 1 && inner_main > 0 && used + need > inner_main)
                break;
            if (first == NULL)
                first = c;
            last = c;
            used += need;
            n++;
        }
        if (first == NULL)
            break;

        /* (3) 本行的 grow / shrink 权重 */
        ql_coord_t sum_base = 0, sum_grow = 0, sum_shrink_w = 0;
        int cnt = 0;
        line_factors(first, last, row, inner_main, &sum_base, &sum_grow, &sum_shrink_w,
                     &cnt);

        const ql_coord_t gaps = gap * (cnt > 0 ? cnt - 1 : 0);
        const ql_coord_t free_space = (inner_main > 0) ? (inner_main - sum_base - gaps) : 0;

        /* (4) 本行交叉尺寸 = 各项外尺寸最大值 */
        ql_coord_t line_cross = 0;
        for (QLUINodeRT *c = first; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            ql_coord_t cr = natural_cross(c, row);
            if (cr > line_cross)
                line_cross = cr;
            if (c == last)
                break;
        }
        /*
         * 单行 + 交叉轴确定 → 行撑满容器（CSS 对单行的特例）。
         *
         * 不撑的话 `align-items: center/flex-end` 在"容器有确定高度、只有一行"
         * 这个最常见的情况下**完全没有效果**（room 恒为 0）。以前就是这样：
         * 表现得像"垂直对齐没实现"。
         */
        if (nline == 1 && cs->flex_wrap != 1) {
            if (container_cross > line_cross)
                line_cross = container_cross;
        } else if (cross_stretch_extra > 0) {
            /* 多行 + align-content: stretch：剩余空间平分给各行（累计推进） */
            stretch_num += (int64_t)cross_stretch_extra;
            const int nrow = (nline > 0) ? nline : 1;
            const ql_coord_t add = (ql_coord_t)(stretch_num / nrow);
            stretch_num -= (int64_t)add * nrow;
            line_cross += add;
        }

        /* (5) justify 只在 grow 没吸收掉剩余空间时生效 */
        ql_coord_t j_start = 0, j_extra_total = 0;
        int j_extra_div = 0;
        if (sum_grow <= 0)
            justify_offsets(free_space, cnt, cs->justify_content, &j_start,
                            &j_extra_total, &j_extra_div);

        /*
         * (6) 分配 + 定位
         *
         * grow / shrink / justify 的份额一律**累计推进**：
         *     num += 总额 * 权重;  份额 = num / 权重和;  num %= 权重和;
         * 这样各项份额之和恒等于总额，不会因为"每项各自整除"把余数丢掉。
         * 100px 三分 flex:1 从 33+33+33=99 变成 33+34+33=100，右边界严丝合缝。
         */
        ql_coord_t cursor = j_start;
        int64_t grow_num = 0;
        int64_t shrink_num = 0;
        int64_t extra_num = 0;
        for (QLUINodeRT *c = first; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;

            const QLUIStyle *is = c->style;
            ql_coord_t base = flex_base_of(c, row, inner_main);
            ql_coord_t final_main = base;

            if (free_space > 0 && sum_grow > 0) {
                grow_num += (int64_t)free_space * (int64_t)is->flex_grow;
                ql_coord_t add = (ql_coord_t)(grow_num / (int64_t)sum_grow);
                grow_num -= (int64_t)add * (int64_t)sum_grow;
                final_main = base + add;
            } else if (free_space < 0 && sum_shrink_w > 0 && is->flex_shrink > 0) {
                ql_coord_t w = (ql_coord_t)((uint32_t)is->flex_shrink *
                                            (uint32_t)((base > 0) ? base : 1) / 100u);
                shrink_num += (int64_t)(-free_space) * (int64_t)w;
                ql_coord_t cut = (ql_coord_t)(shrink_num / (int64_t)sum_shrink_w);
                shrink_num -= (int64_t)cut * (int64_t)sum_shrink_w;
                final_main = base - cut;
                if (final_main < 0)
                    final_main = 0;
            }

            const ql_coord_t margin_main =
                row ? (ql_coord_t)(is->margin.left + is->margin.right)
                    : (ql_coord_t)(is->margin.top + is->margin.bottom);
            const ql_coord_t margin_cross =
                row ? (ql_coord_t)(is->margin.top + is->margin.bottom)
                    : (ql_coord_t)(is->margin.left + is->margin.right);

            ql_coord_t inner_sz = final_main - margin_main;
            if (inner_sz < 0)
                inner_sz = 0;

            /* 交叉轴对齐：`align-self` 覆盖自己，没写（auto）就跟容器的 align-items */
            const uint8_t align =
                (is->align_self != QL_UI_ALIGN_SELF_AUTO) ? is->align_self
                                                          : cs->align_items;
            const int stretch = (align == QL_UI_ALIGN_STRETCH) &&
                                (row ? is->height.unit == QL_UI_UNIT_AUTO
                                     : is->width.unit == QL_UI_UNIT_AUTO);
            ql_coord_t cross_sz = natural_cross(c, row);
            ql_coord_t cross_pos = 0;
            if (stretch) {
                cross_sz = line_cross - margin_cross;
                if (cross_sz < 0)
                    cross_sz = 0;
            } else {
                ql_coord_t room = line_cross - cross_sz;
                if (room < 0)
                    room = 0;
                if (align == QL_UI_ALIGN_CENTER)
                    cross_pos = room / 2;
                else if (align == QL_UI_ALIGN_END)
                    cross_pos = room;
            }

            int size_changed = (inner_sz != (row ? c->border_box.w : c->border_box.h));
            force_size(c, inner_sz, row, size_changed, cross_sz, stretch);

            /*
             * reverse 从主轴末端往回排。inner_main <= 0（column + auto 高度）时
             * 无从得知末端，退回正向（CSS 里这种情况也几乎不出现）。
             */
            ql_coord_t main_pos = cursor;
            if (reversed && inner_main > 0)
                main_pos = inner_main - cursor - final_main;
            if (main_pos < 0)
                main_pos = 0;

            const ql_coord_t cross_at = cross_cursor + cross_pos;
            if (row)
                (void)layout_node(c, content_x + main_pos, content_y + cross_at,
                                  inner_sz, definite_h ? avail_h : -1);
            else
                (void)layout_node(c, content_x + cross_at, content_y + main_pos,
                                  content_w, definite_h ? avail_h : -1);

            /* justify 的额外间距同样累计分配：最后一项刚好落到行尾 */
            ql_coord_t extra = 0;
            if (j_extra_div > 0) {
                extra_num += (int64_t)j_extra_total;
                extra = (ql_coord_t)(extra_num / (int64_t)j_extra_div);
                extra_num -= (int64_t)extra * (int64_t)j_extra_div;
            }
            cursor += final_main + gap + extra;
            if (c == last)
                break;
        }

        cross_cursor += line_cross;
        line_begin = last->next_sibling;
        if (line_begin != NULL) {
            ql_coord_t extra = 0;
            if (cross_extra_div > 0) {
                cross_num += (int64_t)cross_extra_total;
                extra = (ql_coord_t)(cross_num / (int64_t)cross_extra_div);
                cross_num -= (int64_t)extra * (int64_t)cross_extra_div;
            }
            cross_cursor += cross_gap + extra;
        }

        if (cs->flex_wrap != 1)
            break;      /* nowrap：一行到底 */
    }

    if (row) {
        /* 有显式高度时，容器至少有这么高（justify/align 的剩余空间才有意义） */
        if (definite_h && avail_h > cross_cursor)
            return avail_h;
        return cross_cursor;
    }

    /* column：内容高 = 子项高度之和 + gap */
    ql_coord_t used_main = 0;
    int total = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
        used_main += c->border_box.h + c->style->margin.top + c->style->margin.bottom;
        total++;
    }
    if (total > 1)
        used_main += gap * (total - 1);
    if (definite_h && avail_h > used_main)
        used_main = avail_h;
    return used_main;
}

/*
 * ------------------------- 绘制包围盒（paint_box） -------------------------
 *
 * 布局结束后自下而上算一次，供绘制做子树剪枝。
 *
 * 三个坑，都靠"取上界"躲过去：
 *   1. 伪类可以改 box-shadow —— 阴影会把包围盒向外扩 blur+1 并平移 (dx,dy)，
 *      所以要把 :hover/:active/:focus 三份样式也并进去，只算基础样式会漏画；
 *   2. 伪类也可以改 display / overflow —— 任一变体不是 none 就不能把包围盒清空；
 *      任一变体是 visible 就不能按 overflow 裁剪子树（并集必须偏大）；
 *   3. overflow != visible 的子树被裁到本节点边框盒，此时子包围盒并进来也不会变大，
 *      但仍然要递归算下去：子节点自己的 paint_box 不能是上一轮的陈旧值。
 */

/* 取本节点可能用到的全部样式（基础 + 三个状态变体），最多 4 份 */
static int collect_node_styles(const QLUINodeRT *node, const QLUIStyle *out[6])
{
    out[0] = node->style;
    int n = (node->style != NULL) ? 1 : 0;

    const QLUIDoc *doc = (node->owner != NULL) ? node->owner->doc : NULL;
    const QLUINode *src = node->src;
    if (doc == NULL || src == NULL)
        return n;

    /* 五份变体：hover / active / focus / disabled / checked（后两个是 B2 加的） */
    const uint16_t idx[5] = {
        (node->m_hover != 0xFFFF) ? node->m_hover : src->style_hover,
        (node->m_active != 0xFFFF) ? node->m_active : src->style_active,
        (node->m_focus != 0xFFFF) ? node->m_focus : src->style_focus,
        src->style_disabled,
        src->style_checked,
    };
    for (int i = 0; i < 5; i++) {
        if (idx[i] != 0xFFFF && idx[i] < doc->style_count && n < 6)
            out[n++] = &doc->styles[idx[i]];
    }
    return n;
}

/* 阴影可能画到的矩形（不算阴影时返回空矩形） */
static QLRect shadow_extent(QLRect box, const QLUIStyle *s)
{
    if (s == NULL || ql_rect_is_empty(box))
        return ql_rect_make(0, 0, 0, 0);
    QLRect acc = ql_rect_make(0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        if (s->shadow[i].alpha == 0 || s->shadow[i].inset)
            continue;   /* 内阴影画在盒子里面，不扩张绘制范围 */
        const ql_coord_t pad = s->shadow[i].blur + 1;
        const QLRect r = ql_rect_make(box.x + s->shadow[i].dx - pad,
                                      box.y + s->shadow[i].dy - pad,
                                      box.w + 2 * pad, box.h + 2 * pad);
        acc = (acc.w == 0) ? r : ql_rect_union(acc, r);
    }
    return acc;
}

/*
 * 本节点 transform 的取值区间（相对基础静态值）。
 * 三个来源都要算进去，否则 p paint_box 会算小、脏区漏画：
 *   1. 状态变体（:hover 里平移）；
 *   2. @keyframes 里插值的 translate；
 *   3. 0（动画结束回落静态值）。
 */
static void move_extent(const QLUINodeRT *node, ql_coord_t *lo_x, ql_coord_t *lo_y,
                        ql_coord_t *hi_x, ql_coord_t *hi_y)
{
    *lo_x = 0;
    *lo_y = 0;
    *hi_x = 0;
    *hi_y = 0;

    const QLUIStyle *styles[6];
    const int n = collect_node_styles(node, styles);
    if (n <= 0 || styles[0] == NULL)
        return;
    const QLUIStyle *base = styles[0];

    for (int i = 1; i < n; i++) {
        ql_coord_t dx = (ql_coord_t)styles[i]->tf_x - (ql_coord_t)base->tf_x;
        ql_coord_t dy = (ql_coord_t)styles[i]->tf_y - (ql_coord_t)base->tf_y;
        if (dx < *lo_x)
            *lo_x = dx;
        if (dx > *hi_x)
            *hi_x = dx;
        if (dy < *lo_y)
            *lo_y = dy;
        if (dy > *hi_y)
            *hi_y = dy;
    }

    const QLUIDoc *doc = (node->owner != NULL) ? node->owner->doc : NULL;
    if (doc == NULL || base->anim == 0xFFFF || base->anim >= doc->anim_count)
        return;

    const QLUIAnimation *def = &doc->anims[base->anim];
    const uint32_t first = def->first_frame;
    const uint32_t count = def->frame_count;
    if (count == 0 || first + count > doc->keyframe_count)
        return;

    for (uint32_t k = 0; k < count; k++) {
        const QLUIKeyframe *f = &doc->keyframes[first + k];
        if ((f->props & QL_UI_KF_MOVE) == 0)
            continue;
        if ((ql_coord_t)f->tf_x < *lo_x)
            *lo_x = f->tf_x;
        if ((ql_coord_t)f->tf_x > *hi_x)
            *hi_x = f->tf_x;
        if ((ql_coord_t)f->tf_y < *lo_y)
            *lo_y = f->tf_y;
        if ((ql_coord_t)f->tf_y > *hi_y)
            *hi_y = f->tf_y;
    }
}

/*
 * 自上而下累加 transform：静态值（含祖先）存进 tf_abs_*，
 * 动画/变体带来的额外位移（同样含祖先）存进 tf_dev_*。
 * 绘制时不用它——绘制按生效样式逐层加偏移；这里只为 paint_box 服务。
 */
static void resolve_transform(QLUINodeRT *node, ql_coord_t ax, ql_coord_t ay,
                              ql_coord_t lo_x, ql_coord_t lo_y,
                              ql_coord_t hi_x, ql_coord_t hi_y)
{
    /*
     * 入参 ax/ay 是**祖先**的平移，lo/hi 是祖先的动画位移区间。
     * 本节点自己也要算进去：自己的 transform 同样平移自己（渲染时先设偏移再画自己）。
     */
    const QLUIStyle *s = node->style;
    const ql_coord_t sdx = (s != NULL) ? s->tf_x : 0;
    const ql_coord_t sdy = (s != NULL) ? s->tf_y : 0;

    ql_coord_t nx = 0, ny = 0, px = 0, py = 0;
    move_extent(node, &nx, &ny, &px, &py);

    const ql_coord_t self_x = ax + sdx;
    const ql_coord_t self_y = ay + sdy;
    const ql_coord_t self_lo_x = lo_x + nx;
    const ql_coord_t self_lo_y = lo_y + ny;
    const ql_coord_t self_hi_x = hi_x + px;
    const ql_coord_t self_hi_y = hi_y + py;

    node->tf_abs_x = (int16_t)self_x;
    node->tf_abs_y = (int16_t)self_y;
    node->tf_dev_lo_x = (int16_t)self_lo_x;
    node->tf_dev_lo_y = (int16_t)self_lo_y;
    node->tf_dev_hi_x = (int16_t)self_hi_x;
    node->tf_dev_hi_y = (int16_t)self_hi_y;

    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling)
        resolve_transform(child, self_x, self_y, self_lo_x, self_lo_y, self_hi_x,
                          self_hi_y);
}

/*
 * transform 是绘制期属性，paint_box 必须按"实际画到的位置"给上界：
 *   - 静态部分：祖先 + 自己的 translate 之和（tf_abs_*，布局末尾自上而下算好）
 *   - 动画部分：整段动画里 translate 的取值区间（tf_dev_*），取 Minkowski 包围盒
 * 只膨胀不收缩，所以脏区永远偏大一点，不会漏画。
 */
/*
 * 本节点 scale 可能取到的最大值（千分比），下界 1000。
 * 三个来源与 move_extent 一致：基础样式、状态变体、@keyframes。
 * 漏掉任何一个都会让放大的元素把脏区撑破——缩小回去时留下残影。
 */
static void scale_max(const QLUINodeRT *node, int32_t *mx, int32_t *my)
{
    *mx = 1000;
    *my = 1000;

    const QLUIStyle *styles[6];
    const int n = collect_node_styles(node, styles);
    for (int i = 0; i < n; i++) {
        if (styles[i] == NULL)
            continue;
        {
            const int ax = (styles[i]->sc_x < 0) ? -styles[i]->sc_x : styles[i]->sc_x;
            const int ay = (styles[i]->sc_y < 0) ? -styles[i]->sc_y : styles[i]->sc_y;
            if (ax > *mx)
                *mx = ax;
            if (ay > *my)
                *my = ay;
        }
    }

    const QLUIDoc *doc = (node->owner != NULL) ? node->owner->doc : NULL;
    if (doc == NULL || n <= 0 || styles[0] == NULL || styles[0]->anim == 0xFFFF ||
        styles[0]->anim >= doc->anim_count)
        return;

    const QLUIAnimation *def = &doc->anims[styles[0]->anim];
    const uint32_t first = def->first_frame;
    const uint32_t count = def->frame_count;
    if (count == 0 || first + count > doc->keyframe_count)
        return;

    for (uint32_t k = 0; k < count; k++) {
        const QLUIKeyframe *f = &doc->keyframes[first + k];
        if ((f->props & QL_UI_KF_MOVE) == 0)
            continue;
        {
            const int ax = (f->sc_x < 0) ? ((f->sc_x == QL_UI_KF_UNSET) ? 0 : -f->sc_x)
                                         : f->sc_x;
            const int ay = (f->sc_y < 0) ? ((f->sc_y == QL_UI_KF_UNSET) ? 0 : -f->sc_y)
                                         : f->sc_y;
            if (ax > *mx)
                *mx = ax;
            if (ay > *my)
                *my = ay;
        }
    }
}

/* transform: scale() 之后，元素自身 paint 的范围（绕中心放大） */
static QLRect scale_extent(const QLUINodeRT *node, QLRect box)
{
    if (ql_rect_is_empty(box))
        return box;

    int32_t mx = 1000, my = 1000;
    scale_max(node, &mx, &my);
    if (mx <= 1000 && my <= 1000)
        return box;

    ql_coord_t cx = box.x + box.w / 2;
    ql_coord_t cy = box.y + box.h / 2;
    /* 放大时向外撑，缩小时保持原范围（宁可多画，不能漏画） */
    ql_coord_t hw = box.w / 2;
    ql_coord_t hh = box.h / 2;
    if (mx > 1000)
        hw = (ql_coord_t)((int64_t)box.w * mx / 2000);
    if (my > 1000)
        hh = (ql_coord_t)((int64_t)box.h * my / 2000);
    return ql_rect_make(cx - hw, cy - hh, hw * 2, hh * 2);
}

static QLRect paint_box_own(const QLUINodeRT *node, QLRect box)
{
    box = scale_extent(node, box);
    if (ql_rect_is_empty(box))
        return box;

    ql_coord_t dx = (ql_coord_t)node->tf_abs_x + (ql_coord_t)node->tf_dev_lo_x;
    ql_coord_t dy = (ql_coord_t)node->tf_abs_y + (ql_coord_t)node->tf_dev_lo_y;
    ql_coord_t dw = (ql_coord_t)node->tf_dev_hi_x - (ql_coord_t)node->tf_dev_lo_x;
    ql_coord_t dh = (ql_coord_t)node->tf_dev_hi_y - (ql_coord_t)node->tf_dev_lo_y;
    return ql_rect_make(box.x + dx, box.y + dy, box.w + dw, box.h + dh);
}

static QLRect compute_paint_box(QLUINodeRT *node)
{
    const QLUIStyle *styles[6];
    int nstyle = collect_node_styles(node, styles);

    int any_visible = 0;
    int any_unclipped = 0;
    for (int i = 0; i < nstyle; i++) {
        if (styles[i]->display != QL_UI_DISPLAY_NONE)
            any_visible = 1;
        if (styles[i]->overflow == QL_UI_OVERFLOW_VISIBLE)
            any_unclipped = 1;
    }

    QLRect box = node->border_box;
    QLRect acc = ql_rect_make(0, 0, 0, 0);

    if (any_visible && !ql_rect_is_empty(box)) {
        acc = paint_box_own(node, box);
        for (int i = 0; i < nstyle; i++)
            acc = ql_rect_union(acc, paint_box_own(node, shadow_extent(box, styles[i])));
    }

    /* 子节点始终递归（见上面第 3 条），是否并进本节点取决于 overflow */
    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        QLRect cb = compute_paint_box(child);
        if (ql_rect_is_empty(cb))
            continue;
        if (!any_unclipped) {
            if (ql_rect_is_empty(box))
                continue;
            /* 裁剪区在子树的绘制空间里：子节点已经带着祖先偏移，这里也要带 */
            cb = ql_rect_intersect(cb, paint_box_own(node, box));
        }
        acc = ql_rect_union(acc, cb);
    }

    node->paint_box = acc;
    return acc;
}

void ql_ui_layout(QLUIRoot *ui, ql_coord_t width, ql_coord_t height)
{
    if (ui == NULL || ui->node_count == 0)
        return;

    if (getenv("QL_TRACE_LAYOUT") != NULL)
        fprintf(stderr, "[layout] %dx%d\n", (int)width, (int)height);
    ui->width = width;
    ui->height = height;
    /* Grid 临时区的 bump 指针每次布局从头开始（嵌套由各层自己还原） */
    ui->grid_place_used = 0;
    ui->grid_size_used = 0;
    s_viewport_w = width;
    s_viewport_h = height;
    s_calc_ops = ui->doc->calc_ops;
    s_calc_op_count = ui->doc->calc_op_count;

    /*
     * 清掉上一轮遗留的 style_copy：flex 分配和绝对定位都会往 style_copy 里写尺寸，
     * 不清就会把上一帧的强制尺寸带到这一帧（视口变窄时表现为"尺寸不更新"）。
     */
    for (uint16_t i = 0; i < ui->node_count; i++)
        ui->nodes[i].col_n = 0;      /* 上一帧可能是多栏，这一帧未必（媒体查询会变） */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *n = &ui->nodes[i];
        if (!n->used_copy)
            continue;
        n->style = n->src != NULL
                       ? ((n->src->style < ((ui->doc->style_count != 0)
                                               ? ui->doc->style_count
                                               : ui->doc->node_count))
                              ? &ui->doc->styles[n->src->style]
                              : &ui->doc->styles[0])
                       : n->style;
        if (n->style_copy != NULL) {
            ql_mem_free(n->style_copy);
            n->style_copy = NULL;
        }
        n->used_copy = 0;
    }

    /*
     * `@media`：按视口宽选断点区间，把这个区间的样式变体装回节点。
     *
     * 区间在构建期就切好了（见 qlui_parse.c 的区间推导），所以这里只是
     * "线性扫一个短数组 + 三次数组取值"，没有条件表达式求值。
     * 状态变体（:hover/:active/:focus）也一起装 —— 每区间各有一套，
     * 否则一悬停就会把响应式尺寸丢掉。
     *
     * **必须在 style_copy 清理之后**：清理会把 `n->style` 复位成 doc 里的基础值
     * （也就是最窄区间那份），放在它前面会被当场覆盖 —— 表现为"宽的窗口里
     * 只有 flex 子项还是窄布局"（踩过）。
     */
    if (ui->doc->media_count > 0 && ui->doc->node_media != NULL) {
        int bucket = -1;
        for (uint16_t m = 0; m < ui->doc->media_count; m++) {
            const QLUIMedia *rg = &ui->doc->media[m];
            if (width >= (ql_coord_t)rg->lo &&
                (rg->hi == 0xFFFFu || width <= (ql_coord_t)rg->hi)) {
                bucket = (int)m;
                break;
            }
        }
        if (bucket >= 0) {
            for (uint16_t i = 0; i < ui->node_count; i++) {
                const uint16_t ofs = ui->doc->node_media[i];
                if (ofs == 0xFFFFu)
                    continue;                 /* 这个节点在所有断点下都一样 */
                const uint16_t *blk = &ui->doc->media_styles[ofs + (size_t)bucket * 4u];
                QLUINodeRT *n = &ui->nodes[i];
                /* 变体下标 0xFFFF 表示"与该区间的 base 相同" */
                const uint16_t base = blk[0];
                n->style = (base != 0xFFFFu) ? &ui->doc->styles[base]
                                             : &ui->doc->styles[n->src->style];
                n->m_hover = blk[1];
                n->m_active = blk[2];
                n->m_focus = blk[3];
            }
        }
    }

    QLUINodeRT *root = &ui->nodes[0];

    /*
     * 根元素的 `width: 100%` / `height: 100%` 按**视口**解析。
     *
     * 根节点的包含块就是视口（CSS 语义），但下面的百分比解析只认"父内容宽度"，
     * 根没有父，所以百分比会落到 0。这里在排版前把它换成确定的像素值。
     * 这是"窗口缩放后页面跟着变"的前提：页面根写成 100%×100% 就自适应了。
     */
    if (root->style->width.unit == QL_UI_UNIT_PERCENT ||
        root->style->height.unit == QL_UI_UNIT_PERCENT) {
        if (ensure_style_copy(root) == NULL)
            return;
        if (root->style_copy->width.unit == QL_UI_UNIT_PERCENT) {
            root->style_copy->width.value =
                (int16_t)((int32_t)width * root->style_copy->width.value / 100);
            root->style_copy->width.unit = QL_UI_UNIT_PX;
        }
        if (root->style_copy->height.unit == QL_UI_UNIT_PERCENT) {
            root->style_copy->height.value =
                (int16_t)((int32_t)height * root->style_copy->height.value / 100);
            root->style_copy->height.unit = QL_UI_UNIT_PX;
        }
    }

    root->abs_cb = ql_rect_make(0, 0, width, height);
    layout_node(root, 0, 0, width, height);

    /*
     * 贴边（sticky）必须在滚动平移之后、算 paint_box 之前：
     * 它会改 border_box，后面所有消费者（绘制/命中/脏区）就都对了。
     */
    sticky_walk(root, NULL);

    /* z-index 排序放在最后：之后绘制与命中测试都自动遵循 z 序 */
    sort_tree_by_z(root);

    /* 浮层居中：必须在算 paint_box 之前（平移会改坐标） */
    dialog_center(ui);

    /* transform 的绝对偏移必须自上而下算，再算绘制包围盒（尺寸 + 位移都定稿） */
    resolve_transform(root, 0, 0, 0, 0, 0, 0);
    compute_paint_box(root);
}

/* 矩形中心到最远角的距离（圆形揭示的 100% 半径） */
static ql_coord_t ql_len_far_corner(ql_coord_t w, ql_coord_t h)
{
    float hw = (float)w * 0.5f;
    float hh = (float)h * 0.5f;
    return (ql_coord_t)(sqrtf(hw * hw + hh * hh) + 0.5f);
}

/* ------------------------------ 绘制 ------------------------------ */

/* 状态过渡槽查找（实现放在交互那一段，绘制要先用） */
static QLUITrans *trans_find(QLUIRoot *ui, const QLUINodeRT *node);
/* 关键帧动画槽查找（同上） */
static const QLUIAnimRun *anim_find(const QLUIRoot *ui, const QLUINodeRT *node);

static const QLFont *font_of(const QLUINodeRT *node)
{
    return font_of_style(node, node->style);
}

const QLFont *ql_ui_node_font(const QLUINodeRT *node)
{
    return (node != NULL) ? font_of(node) : NULL;
}

/*
 * 状态变体解析：ACTIVE > FOCUS > HOVER > 基础。
 * **不看过渡**——过渡的目标值必须由它算出来，否则会"以自己为目标"。
 */
static const QLUIStyle *style_for_state(const QLUINodeRT *node)
{
    if (node == NULL)
        return NULL;

    /* 优先级与 draw_default 一致：ACTIVE > FOCUS > HOVER */
    const QLUINode *src = node->src;
    if (src != NULL) {
        /* 媒体断点下的变体优先（没有就用 doc 里那份） */
        const uint16_t m_hover = (node->m_hover != 0xFFFF) ? node->m_hover
                                                          : src->style_hover;
        const uint16_t m_active = (node->m_active != 0xFFFF) ? node->m_active
                                                            : src->style_active;
        const uint16_t m_focus = (node->m_focus != 0xFFFF) ? node->m_focus
                                                          : src->style_focus;
        uint16_t idx = 0xFFFF;
        /*
         * 优先级：disabled > active > focus > checked > hover。
         * 禁用元素不该再"悬停高亮"，所以让它排最前；
         * checked 排在 hover 之前 —— 勾选状态比悬停更"实"（与浏览器观感一致）。
         */
        const uint16_t m_dis = src->style_disabled;
        const uint16_t m_chk = src->style_checked;
        if ((node->flags & QL_UI_RT_DISABLED) != 0 && m_dis != 0xFFFF)
            idx = m_dis;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_ACTIVE) != 0)
            idx = m_active;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_FOCUS) != 0)
            idx = m_focus;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_CHECKED) != 0 && m_chk != 0xFFFF)
            idx = m_chk;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_HOVER) != 0)
            idx = m_hover;
        if (idx != 0xFFFF && node->owner != NULL && node->owner->doc != NULL)
            return &node->owner->doc->styles[idx];
    }
    return node->style;
}

const QLUIStyle *ql_ui_node_style(const QLUINodeRT *node)
{
    if (node == NULL)
        return NULL;

    /*
     * 关键帧动画优先：它播的是"元素的进场/循环效果"，状态过渡不该跟它抢同一个通道。
     * （生成期不改，运行期一个节点同时有两者时动画赢；trans_retarget 里会跳过。）
     */
    if (node->owner != NULL && node->owner->runs != NULL) {
        const QLUIAnimRun *run = anim_find(node->owner, node);
        if (run != NULL)
            return &run->style;
    }

    /* 正在过渡：返回插值中的那一份（其它属性已经是目标样式） */
    if (node->owner != NULL && node->owner->trans != NULL) {
        QLUITrans *t = trans_find(node->owner, node);
        if (t != NULL)
            return &t->style;
    }
    return style_for_state(node);
}

QLRect ql_ui_node_rect(const QLUINodeRT *node)
{
    return (node != NULL) ? node->border_box : ql_rect_make(0, 0, 0, 0);
}

const char *ql_ui_node_text(const QLUINodeRT *node)
{
    if (node == NULL || node->text == NULL)
        return "";
    /* <img> 的 text 是资源名，不是显示文字：对外一律返回空串 */
    if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG)
        return "";
    return node->text;
}

/* ------------------------------ 滚动条 ------------------------------ */

#define SCROLLBAR_W     4      /* 滑块宽度（px） */
#define SCROLLBAR_INSET 2      /* 距右边/下边的内缩 */

/* 滑块的尺寸与位置：可视高 ch、内容超出 max_y、当前偏移 scroll_y */
static void scrollbar_thumb(ql_coord_t ch, ql_coord_t max_y, ql_coord_t scroll_y,
                            ql_coord_t *out_h, ql_coord_t *out_y)
{
    const ql_coord_t track = ch - 2 * SCROLLBAR_INSET;
    if (track <= 0 || max_y <= 0) {
        *out_h = 0;
        *out_y = 0;
        return;
    }
    ql_coord_t th = (ql_coord_t)((int64_t)track * ch / (ch + max_y));
    if (th < 16)
        th = 16;
    if (th > track)
        th = track;
    const ql_coord_t room = track - th;
    const ql_coord_t ty =
        SCROLLBAR_INSET + (ql_coord_t)((int64_t)room * scroll_y / max_y);
    *out_h = th;
    *out_y = ty;
}

/*
 * 画滚动条（覆盖式，不占布局空间）。
 *
 * 只在 `overflow: auto` 且真的滚得动时画；`overflow: hidden` 永远不画 ——
 * 这正是 hidden 与 auto 的唯一区别（两者都裁，只有 auto 能滚）。
 * 颜色用中灰 + 半透明，深浅主题都看得见。
 */
static void draw_scrollbar(QLUINodeRT *node, QLDraw2D *d2d)
{
    if (!ql_ui_scrollable(node) || d2d->fill_round_rect == NULL)
        return;

    const QLUIStyle *s = ql_ui_node_style(node);
    /* `scrollbar-width`：0 = 默认，1 = thin，2 = auto，3 = none 不画 */
    if (s != NULL && s->scrollbar_width == 3)
        return;
    ql_coord_t w = SCROLLBAR_W;
    if (s != NULL && s->scrollbar_width == 1)
        w = 2;
    else if (s != NULL && s->scrollbar_width == 2)
        w = SCROLLBAR_W;

    const QLRect box = ql_ui_node_rect(node);
    ql_coord_t th = 0, ty = 0;
    scrollbar_thumb(box.h, node->scroll_max_y, node->scroll_y, &th, &ty);
    if (th <= 0)
        return;

    QLRect bar = ql_rect_make(ql_rect_right(box) - SCROLLBAR_INSET - w, box.y + ty, w,
                              th);
    ql_color_t col = 0x808080;
    uint8_t alpha = 0x96;
    if (s != NULL && s->scrollbar_color != 0)
        col = s->scrollbar_color;
    d2d->fill_round_rect(d2d, bar, w / 2, col, alpha);
}


/* ------------------------------ 下拉框 ------------------------------ */


/* 第 i 个 <option> 子节点（display:none 的不算） */
/*
 * <option> 可以是 <select> 的直接子节点，也可以包在 <optgroup> 里 —— 所以
 * 往下找**一层**（optgroup 的组名现在不画，见 QLUI阅读指南 §5.4.7）。
 */
static QLUINodeRT *select_option_at(const QLUINodeRT *sel, int index)
{
    int i = 0;
    for (QLUINodeRT *c = sel->first_child; c != NULL; c = c->next_sibling) {
        if (c->src == NULL)
            continue;
        if (c->src->tag == QL_UI_TAG_OPTION) {
            if (i == index)
                return c;
            i++;
        } else if (c->src->tag == QL_UI_TAG_OPTGROUP) {
            for (QLUINodeRT *o = c->first_child; o != NULL; o = o->next_sibling) {
                if (o->src == NULL || o->src->tag != QL_UI_TAG_OPTION)
                    continue;
                if (i == index)
                    return o;
                i++;
            }
        }
    }
    return NULL;
}

static int select_option_count(const QLUINodeRT *sel)
{
    int n = 0;
    for (QLUINodeRT *c = sel->first_child; c != NULL; c = c->next_sibling) {
        if (c->src == NULL)
            continue;
        if (c->src->tag == QL_UI_TAG_OPTION) {
            n++;
        } else if (c->src->tag == QL_UI_TAG_OPTGROUP) {
            for (QLUINodeRT *o = c->first_child; o != NULL; o = o->next_sibling)
                if (o->src != NULL && o->src->tag == QL_UI_TAG_OPTION)
                    n++;
        }
    }
    return n;
}

static struct QLUISelectState *select_of(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->selects == NULL || node->src == NULL)
        return NULL;
    if (node->src->tag != QL_UI_TAG_SELECT)
        return NULL;
    for (uint16_t i = 0; i < ui->select_count; i++)
        if (ui->selects[i].node == node)
            return &ui->selects[i];
    return NULL;
}

/* 把 select 显示的文字换成选中项的（node->text 指过去即可，不复制） */
static void select_sync_text(struct QLUISelectState *st)
{
    QLUINodeRT *opt = select_option_at(st->node, (int)st->sel);
    st->node->text = (opt != NULL) ? opt->text : NULL;
    st->node->flags |= QL_UI_RT_DIRTY;
}

/* 面板几何：贴在 select 下沿，宽度与它一致；太高就截到 QL_CFG_UI_SELECT_PANEL_H */
static QLRect select_panel_rect(const struct QLUISelectState *st)
{
    const QLRect box = ql_ui_node_rect(st->node);
    const int n = select_option_count(st->node);
    const ql_coord_t row_h = (ql_coord_t)(st->node->style->line_h > 0
                                             ? st->node->style->line_h
                                             : 24);
    ql_coord_t h = row_h * (ql_coord_t)n;
    if (h > (ql_coord_t)QL_CFG_UI_SELECT_PANEL_H)
        h = (ql_coord_t)QL_CFG_UI_SELECT_PANEL_H;
    return ql_rect_make(box.x, ql_rect_bottom(box), box.w, h);
}

/* 画展开的面板（覆盖在整棵树之上，不受 overflow 裁剪） */
static void draw_select_panel(QLUIRoot *ui, QLDraw2D *d2d)
{
    struct QLUISelectState *st = ui->open_select;
    if (st == NULL || !st->open || d2d->fill_rect == NULL)
        return;

    const QLRect panel = select_panel_rect(st);
    const QLFont *font = font_of_style(st->node, st->node->style);
    const ql_coord_t row_h = (ql_coord_t)(st->node->style->line_h > 0
                                             ? st->node->style->line_h
                                             : 24);

    /* 底板 + 选中的那一行高亮 */
    d2d->fill_rect(d2d, panel, 0xFFFFFF, 255);
    int idx = 0;
    for (QLUINodeRT *c = st->node->first_child; c != NULL; c = c->next_sibling) {
        if (c->src == NULL || c->src->tag != QL_UI_TAG_OPTION)
            continue;
        const ql_coord_t y = panel.y + (ql_coord_t)idx * row_h;
        if (y + row_h > ql_rect_bottom(panel))
            break;
        QLRect row = ql_rect_make(panel.x, y, panel.w, row_h);
        if ((uint16_t)idx == st->sel)
            d2d->fill_rect(d2d, row, 0x2E6BE6, 255);

        if (c->text != NULL && c->text[0] != '\0') {
            QLTextStyle ts;
            memset(&ts, 0, sizeof(ts));
            ts.color = ((uint16_t)idx == st->sel) ? 0xFFFFFF : 0x000000;
            ts.line_h = row_h;
            QLRect tb = ql_rect_make(panel.x + 8, y, panel.w - 16, row_h);
            d2d->draw_text(d2d, font, c->text, -1, tb, &ts);
        }
        idx++;
    }
}

void ql_ui_set_select_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user)
{
    if (ui == NULL)
        return;
    ui->input_cb = cb;      /* 与输入框共用一个回调位：两者不会同时用 */
    ui->input_user = user;
}

int ql_ui_select_index(const QLUIRoot *ui, const QLUINodeRT *node)
{
    const struct QLUISelectState *st = select_of(ui, node);
    return (st != NULL) ? (int)st->sel : -1;
}

ql_err_t ql_ui_select_set_index(QLUIRoot *ui, QLUINodeRT *node, int index)
{
    struct QLUISelectState *st = select_of(ui, node);
    if (st == NULL)
        return QL_ERR_INVAL;
    const int n = select_option_count(node);
    if (n <= 0)
        return QL_ERR_INVAL;
    if (index < 0)
        index = 0;
    if (index >= n)
        index = n - 1;
    if ((uint16_t)index != st->sel) {
        st->sel = (uint16_t)index;
        select_sync_text(st);
        if (ui->input_cb != NULL) {
            const QLUINodeRT *opt = select_option_at(node, index);
            ui->input_cb(ui, node, (opt != NULL && opt->text != NULL) ? opt->text : "",
                         ui->input_user);
        }
    }
    return QL_OK;
}

int ql_ui_progress_permille(const QLUIRoot *ui, const QLUINodeRT *node)
{
    const struct QLUIProgress *pg = progress_of(ui, node);
    if (pg == NULL)
        return -1;
    return pg->known ? (int)pg->permille : -2;
}

ql_err_t ql_ui_progress_set(QLUIRoot *ui, QLUINodeRT *node, int value, int max)
{
    struct QLUIProgress *pg = progress_of(ui, node);
    if (pg == NULL)
        return QL_ERR_INVAL;
    const int m = (max > 0) ? max : 100;
    int v = value;
    if (v < 0)
        v = 0;
    if (v > m)
        v = m;
    pg->permille = (uint16_t)(((int32_t)v * 1000) / m);
    pg->known = 1;
    pg->node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* 点在下拉框上：展开/收起；点开着的面板上：选中那一项 */
static int select_pointer_down(QLUIRoot *ui, QLUINodeRT *hit, ql_coord_t x, ql_coord_t y)
{
    /* 先看有没有开着的面板（它在最上层，优先命中） */
    struct QLUISelectState *open = ui->open_select;
    if (open != NULL && open->open) {
        const QLRect panel = select_panel_rect(open);
        if (x >= panel.x && x < ql_rect_right(panel) && y >= panel.y &&
            y < ql_rect_bottom(panel)) {
            const ql_coord_t row_h = (ql_coord_t)(open->node->style->line_h > 0
                                                     ? open->node->style->line_h
                                                     : 24);
            const int idx = (row_h > 0) ? (int)((y - panel.y) / row_h) : 0;
            (void)ql_ui_select_set_index(ui, open->node, idx);
            open->open = 0;
            ui->open_select = NULL;
            open->node->flags |= QL_UI_RT_DIRTY;
            return 1;
        }
        /*
         * 点在面板外：收起。
         * 如果点的就是这个 select 自己，那就是"再点一次关掉"，到此为止；
         * 点的是别处，就继续走普通点击流程（一箭双雕：关面板 + 正常交互）。
         */
        const int self = (select_of(ui, hit) == open);
        open->open = 0;
        ui->open_select = NULL;
        open->node->flags |= QL_UI_RT_DIRTY;
        if (self)
            return 1;
        return 0;  /* 面板关了，这次点击交给下面的普通流程 */
    }

    struct QLUISelectState *st = select_of(ui, hit);
    if (st == NULL)
        return 0;
    st->open = 1;
    ui->open_select = st;
    st->node->flags |= QL_UI_RT_DIRTY;
    ql_ui_set_focus_node(ui, st->node);  /* 让 :focus 与后续键盘都跟得上 */
    return 1;
}

/* 焦点在下拉框上时的键盘：上下改选中，空格/回车展开或收起 */
static int select_key(QLUIRoot *ui, struct QLUISelectState *st, const QLInputEvent *ev)
{
    const int n = select_option_count(st->node);
    switch (ev->key) {
    case QL_KEY_UP:
        if (n > 0)
            (void)ql_ui_select_set_index(ui, st->node, (int)st->sel - 1);
        return 1;
    case QL_KEY_DOWN:
        if (n > 0)
            (void)ql_ui_select_set_index(ui, st->node, (int)st->sel + 1);
        return 1;
    case QL_KEY_SPACE:
    case QL_KEY_ENTER:
        if (n <= 0)
            return 1;
        st->open = (uint8_t)(!st->open);
        ui->open_select = st->open ? st : NULL;
        st->node->flags |= QL_UI_RT_DIRTY;
        return 1;
    default:
        return 0;
    }
}

/* -------------------------------- 浮层 -------------------------------- */

/* 找最后一个打开的 <dialog>（= 最上层）；没有返回 NULL */
static QLUINodeRT *dialog_top(const QLUIRoot *ui)
{
    if (ui == NULL || ui->dialog_count == 0)
        return NULL;
    QLUINodeRT *top = NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *n = &ui->nodes[i];
        if (n->src != NULL && n->src->tag == QL_UI_TAG_DIALOG &&
            (n->flags & QL_UI_RT_OPEN) != 0)
            top = n;
    }
    return top;
}

static void dialog_apply(QLUIRoot *ui, QLUINodeRT *nd, int open)
{
    if (open) {
        nd->flags |= QL_UI_RT_OPEN;
        nd->style = &ui->doc->styles[nd->src->style];
    } else {
        nd->flags = (uint8_t)(nd->flags & ~(uint8_t)QL_UI_RT_OPEN);
        nd->style = &s_style_hidden;
    }
    nd->flags |= QL_UI_RT_DIRTY;
}

int ql_ui_dialog_open(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || node->src == NULL ||
        node->src->tag != QL_UI_TAG_DIALOG)
        return -1;
    return (node->flags & QL_UI_RT_OPEN) != 0;
}

ql_err_t ql_ui_dialog_set_open(QLUIRoot *ui, QLUINodeRT *node, int open)
{
    if (ql_ui_dialog_open(ui, node) < 0)
        return QL_ERR_INVAL;
    const int want = (open != 0);
    if (want == ql_ui_dialog_open(ui, node))
        return QL_OK;
    dialog_apply(ui, node, want);
    ql_ui_layout(ui, ui->width, ui->height);   /* 居中与可见性都变了 */
    ui->nodes[0].flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

int ql_ui_dialog_close_top(QLUIRoot *ui)
{
    QLUINodeRT *top = dialog_top(ui);
    if (top == NULL)
        return 0;
    (void)ql_ui_dialog_set_open(ui, top, 0);
    return 1;
}

/* 布局末尾：把打开的对话框平移到视口中央（平移子树 = 滚动那一招） */
static void dialog_center(QLUIRoot *ui)
{
    if (ui->dialog_count == 0)
        return;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *n = &ui->nodes[i];
        if (n->src == NULL || n->src->tag != QL_UI_TAG_DIALOG ||
            (n->flags & QL_UI_RT_OPEN) == 0)
            continue;
        const ql_coord_t dx = (ui->width - n->border_box.w) / 2 - n->border_box.x;
        const ql_coord_t dy = (ui->height - n->border_box.h) / 2 - n->border_box.y;
        translate_subtree(n, dx, dy);
    }
}

/*
 * 浮层绘制：整棵树画完之后，压暗视口，再把对话框子树画一遍盖上去。
 *
 * 为什么要画第二遍：遮罩必须盖住**除了对话框以外**的一切，而树是父先子后画的。
 * 先画遮罩会盖掉后画的兄弟节点，所以顺序只能是"先全部、再遮罩、再对话框"。
 * 代价：开着对话框时那个子树每帧多画一次（它是 opaque 的，结果一致）。
 */
static void draw_dialog_layer(QLUIRoot *ui, QLDraw2D *d2d)
{
    QLUINodeRT *top = dialog_top(ui);
    if (top == NULL)
        return;
    const QLRect vp = ql_rect_make(0, 0, ui->width, ui->height);
    if (!ql_rect_is_empty(vp))
        d2d->fill_rect(d2d, vp, 0x000000, 96);
    render_node(top, d2d);
}

/* ------------------------------ 折叠面板 ------------------------------ */

/*
 * 收起时把"非 summary 的子节点"指向共享隐藏样式；展开时指回 doc 里的原样式。
 *
 * 为什么能做到零额外状态：`node->flags` 里的 QL_UI_NODE_OPEN 是 doc 带过来的，
 * 而"原样式是哪个"由 `src->style` 唯一决定（doc 里存的是下标），不需要记指针。
 */
static void details_apply(QLUIRoot *ui, QLUINodeRT *nd, int open)
{
    for (QLUINodeRT *c = nd->first_child; c != NULL; c = c->next_sibling) {
        if (c->src == NULL || c->src->tag == QL_UI_TAG_SUMMARY)
            continue;
        c->style = open ? &ui->doc->styles[c->src->style] : &s_style_hidden;
    }
    /*
     * 注意：doc 里的 `QL_UI_NODE_OPEN` 是**初始值**（字节与 RT 位不共用），
     * 运行期状态存在 `QL_UI_RT_OPEN` 里 —— 同一个字节既装 NODE 又装 RT 会撞位。
     */
    if (open)
        nd->flags |= QL_UI_RT_OPEN;
    else
        nd->flags = (uint8_t)(nd->flags & ~(uint8_t)QL_UI_RT_OPEN);
    nd->flags |= QL_UI_RT_DIRTY;
}

int ql_ui_details_open(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || node->src == NULL ||
        node->src->tag != QL_UI_TAG_DETAILS)
        return -1;
    return (node->flags & QL_UI_RT_OPEN) != 0;
}

ql_err_t ql_ui_details_set_open(QLUIRoot *ui, QLUINodeRT *node, int open)
{
    if (ql_ui_details_open(ui, node) < 0)
        return QL_ERR_INVAL;
    const int want = (open != 0);
    if (want == ql_ui_details_open(ui, node))
        return QL_OK;                    /* 状态没变：不重排 */
    details_apply(ui, node, want);
    ql_ui_layout(ui, ui->width, ui->height);   /* 高度变了，必须重排 */
    return QL_OK;
}

/* 点中 <summary>（或 details 自己）时切换；返回 1 表示这次点击被吃掉了 */
static int details_pointer_down(QLUIRoot *ui, QLUINodeRT *hit, ql_coord_t x,
                               ql_coord_t y)
{
    (void)x;
    (void)y;
    if (hit == NULL || hit->src == NULL)
        return 0;
    /* 从命中点往上找最近的 <summary>，它的父节点就是 <details> */
    QLUINodeRT *n = hit;
    while (n != NULL && n->src != NULL && n->src->tag != QL_UI_TAG_SUMMARY)
        n = n->parent;
    if (n == NULL || n->parent == NULL || n->parent->src == NULL ||
        n->parent->src->tag != QL_UI_TAG_DETAILS)
        return 0;
    QLUINodeRT *details = n->parent;
    (void)ql_ui_details_set_open(ui, details, !ql_ui_details_open(ui, details));
    return 1;
}

/* -------------------------------- 图像 -------------------------------- */

static struct QLUIImageRT *image_of(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->images == NULL)
        return NULL;
    for (uint16_t i = 0; i < ui->image_count; i++)
        if (ui->images[i].node == node)
            return &ui->images[i];
    return NULL;
}

/* 按资源名找（背景图用；B4） */
static const QLImage *image_by_name(const QLUIRoot *ui, const char *name)
{
    if (ui == NULL || name == NULL || ui->images == NULL)
        return NULL;
    for (uint16_t i = 0; i < ui->image_count; i++) {
        const struct QLUIImageRT *ir = &ui->images[i];
        const char *nm = (ir->node != NULL) ? ir->node->text : ir->name;
        if (nm != NULL && strcmp(nm, name) == 0 && ir->img.px != NULL)
            return &ir->img;
    }
    return NULL;
}

const QLImage *ql_ui_node_image(const QLUIRoot *ui, const QLUINodeRT *node)
{
    const struct QLUIImageRT *ir = image_of(ui, node);
    if (ir == NULL || ir->img.px == NULL)
        return NULL;
    return &ir->img;
}

/* ------------------------------ 图像热区 ------------------------------ */

/* 点是否在矩形内（右/下边界不含，与布局的半开区间一致） */
static int rect_has_point(QLRect r, ql_coord_t x, ql_coord_t y)
{
    return x >= r.x && y >= r.y && x < ql_rect_right(r) && y < ql_rect_bottom(r);
}

/*
 * 热区命中需要两个矩形：
 *   dst  —— 图片**画在哪**（object-fit 之后，可能比内容盒大）
 *   clip —— `dst ∩ 内容盒`，超出内容盒的部分在屏幕上看不见
 * 坐标换算以 dst 为准（原点 = 图片左上角），可见性判定以 clip 为准。
 */
static int image_draw_box(const QLUIRoot *ui, const QLUINodeRT *node, QLRect *dst,
                          QLRect *clip, ql_coord_t *iw, ql_coord_t *ih)
{
    if (node == NULL || node->src == NULL || node->src->tag != QL_UI_TAG_IMG)
        return 0;
    ql_coord_t nw = 0, nh = 0;
    image_node_size(ui, node, &nw, &nh);
    if (nw <= 0 || nh <= 0)
        return 0;
    QLRect src;
    if (!image_fit_rect((int32_t)nw, (int32_t)nh, node->content_box,
                        ql_ui_style_object_fit(ql_ui_node_style(node)), dst, &src))
        return 0;
    *clip = ql_rect_intersect(*dst, node->content_box);
    *iw = nw;
    *ih = nh;
    return 1;
}

const QLUIArea *ql_ui_area_get(const QLUIRoot *ui, int index)
{
    if (ui == NULL || ui->doc == NULL || index < 0 || index >= (int)ui->doc->area_count)
        return NULL;
    return &ui->doc->areas[index];
}

int ql_ui_area_count(const QLUIRoot *ui)
{
    if (ui == NULL || ui->doc == NULL)
        return 0;
    return (int)ui->doc->area_count;
}

const QLUINodeRT *ql_ui_area_image(const QLUIRoot *ui, int index)
{
    const QLUIArea *a = ql_ui_area_get(ui, index);
    if (a == NULL || a->img >= ui->node_count)
        return NULL;
    return &ui->nodes[a->img];
}

static const char *area_str(const QLUIRoot *ui, uint16_t off)
{
    if (off == 0xFFFFu || ui->doc->text_pool == NULL || ui->doc->text_offsets == NULL)
        return NULL;
    return ui->doc->text_pool + ui->doc->text_offsets[off];
}

const char *ql_ui_area_href(const QLUIRoot *ui, int index)
{
    const QLUIArea *a = ql_ui_area_get(ui, index);
    return (a != NULL) ? area_str(ui, a->href) : NULL;
}

const char *ql_ui_area_alt(const QLUIRoot *ui, int index)
{
    const QLUIArea *a = ql_ui_area_get(ui, index);
    return (a != NULL) ? area_str(ui, a->alt) : NULL;
}

/* 奇偶环绕规则（crossing number）：点在多边形内 ⇔ 向右射线与边界的交点个数是奇数 */
static int poly_contains(const int16_t *c, int n, int32_t px, int32_t py)
{
    int inside = 0;
    for (int i = 0, j = n / 2 - 1; i < n / 2; j = i++) {
        const int32_t xi = c[2 * i], yi = c[2 * i + 1];
        const int32_t xj = c[2 * j], yj = c[2 * j + 1];
        if (((yi > py) != (yj > py)) &&
            ((int64_t)px < (int64_t)(xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

/* 热区在**图片像素坐标系**里的包围盒（DEFAULT 用整张图） */
static void area_bbox(const QLUIArea *a, const int16_t *c, int32_t iw, int32_t ih,
                      int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    if (a->ncoord == 0) {
        *x0 = 0;
        *y0 = 0;
        *x1 = iw - 1;
        *y1 = ih - 1;
        return;
    }
    if (a->shape == QL_UI_AREA_CIRCLE) {
        *x0 = (int32_t)c[0] - c[2];
        *y0 = (int32_t)c[1] - c[2];
        *x1 = (int32_t)c[0] + c[2];
        *y1 = (int32_t)c[1] + c[2];
        return;
    }
    int32_t minx = c[0], maxx = c[0], miny = c[1], maxy = c[1];
    for (int i = 2; i + 1 < a->ncoord; i += 2) {
        if (c[i] < minx) minx = c[i];
        if (c[i] > maxx) maxx = c[i];
        if (c[i + 1] < miny) miny = c[i + 1];
        if (c[i + 1] > maxy) maxy = c[i + 1];
    }
    *x0 = minx;
    *y0 = miny;
    *x1 = maxx;
    *y1 = maxy;
}

/* 单个热区是否包含"点"（点在屏幕空间，映射到图片像素后再判） */
static int area_contains(const QLUIArea *a, const int16_t *c, QLRect dst, ql_coord_t iw,
                         ql_coord_t ih, ql_coord_t x, ql_coord_t y)
{
    /* 屏幕 → 图片像素（整数除法，除数为 0 已在调用方挡住） */
    const int32_t lx = (int32_t)(x - dst.x) * (int32_t)iw / (int32_t)dst.w;
    const int32_t ly = (int32_t)(y - dst.y) * (int32_t)ih / (int32_t)dst.h;

    switch (a->shape) {
    case QL_UI_AREA_CIRCLE: {
        const int64_t dx = lx - c[0], dy = ly - c[1], r = c[2];
        return dx * dx + dy * dy <= r * r;
    }
    case QL_UI_AREA_POLY:
        return poly_contains(c, a->ncoord, lx, ly);
    case QL_UI_AREA_DEFAULT:
        return 1;
    default: { /* RECT：允许 x0 > x1 的写法，交换一下就行 */
        int32_t x0 = c[0], y0 = c[1], x1 = c[2], y1 = c[3];
        if (x0 > x1) { const int32_t t = x0; x0 = x1; x1 = t; }
        if (y0 > y1) { const int32_t t = y0; y0 = y1; y1 = t; }
        return lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1;
    }
    }
}

int ql_ui_area_at(const QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL || ui->doc == NULL || ui->doc->area_count == 0)
        return -1;
    for (uint16_t i = 0; i < ui->doc->area_count; i++) {
        const QLUIArea *a = &ui->doc->areas[i];
        if (a->img >= ui->node_count)
            continue;
        QLRect dst, clip;
        ql_coord_t iw = 0, ih = 0;
        if (!image_draw_box(ui, &ui->nodes[a->img], &dst, &clip, &iw, &ih))
            continue;
        if (!rect_has_point(clip, x, y))
            continue;
        const int16_t *c = (a->ncoord > 0) ? ui->doc->area_coords + a->coord_off : NULL;
        if (area_contains(a, c, dst, iw, ih, x, y))
            return (int)i;
    }
    return -1;
}

QLRect ql_ui_area_rect(const QLUIRoot *ui, int index)
{
    const QLUIArea *a = ql_ui_area_get(ui, index);
    if (a == NULL || a->img >= ui->node_count)
        return ql_rect_make(0, 0, 0, 0);
    if (a->ncoord > 0 && ui->doc->area_coords == NULL)
        return ql_rect_make(0, 0, 0, 0);
    QLRect dst, clip;
    ql_coord_t iw = 0, ih = 0;
    if (!image_draw_box(ui, &ui->nodes[a->img], &dst, &clip, &iw, &ih))
        return ql_rect_make(0, 0, 0, 0);

    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const int16_t *c = (a->ncoord > 0) ? ui->doc->area_coords + a->coord_off : NULL;
    area_bbox(a, c, (int32_t)iw, (int32_t)ih, &x0, &y0, &x1, &y1);

    /* 图片像素 → 屏幕（与 area_contains 的反向换算一致）。
     * 远端用 x1 + 1 算"排他边界"，这样 4 个图片像素在 4 倍缩放下正好 16 px 宽。 */
    const ql_coord_t sx0 = dst.x + (ql_coord_t)(x0 * (int32_t)dst.w / (int32_t)iw);
    const ql_coord_t sy0 = dst.y + (ql_coord_t)(y0 * (int32_t)dst.h / (int32_t)ih);
    const ql_coord_t sx1 = dst.x + (ql_coord_t)((x1 + 1) * (int32_t)dst.w / (int32_t)iw);
    const ql_coord_t sy1 = dst.y + (ql_coord_t)((y1 + 1) * (int32_t)dst.h / (int32_t)ih);

    const QLRect r = ql_rect_make(sx0, sy0, sx1 - sx0, sy1 - sy0);
    return ql_rect_intersect(r, clip);
}

void ql_ui_set_area_cb(QLUIRoot *ui, ql_ui_area_cb cb, void *user)
{
    if (ui == NULL)
        return;
    ui->area_cb = cb;
    ui->area_user = user;
}

/*
 * 布局需要的"固有尺寸"。**不看解码是否成功** —— 尺寸来自 doc 的资源条目，
 * 所以图没解出来也能占对位置（只是画不出来）。
 */
static void image_node_size(const QLUIRoot *ui, const QLUINodeRT *node, ql_coord_t *w,
                            ql_coord_t *h)
{
    *w = 0;
    *h = 0;
    if (node == NULL || node->src == NULL || node->src->tag != QL_UI_TAG_IMG)
        return;
    const struct QLUIImageRT *ir = image_of(ui, node);
    if (ir != NULL && ir->img.w > 0) {
        *w = (ql_coord_t)ir->img.w;
        *h = (ql_coord_t)ir->img.h;
        return;
    }
    /* 没注册资源：退回 doc 里的资源条目（blob 路径下这里才有值） */
    if (node->text == NULL || ui->doc == NULL)
        return;
    for (uint16_t j = 0; j < ui->doc->image_count; j++) {
        const QLUIImageSrc *src = &ui->doc->images[j];
        const char *nm = ui->doc->text_pool + ui->doc->text_offsets[src->name];
        if (strcmp(nm, node->text) == 0) {
            *w = (ql_coord_t)src->w;
            *h = (ql_coord_t)src->h;
            return;
        }
    }
}

ql_err_t ql_ui_register_image(QLUIRoot *ui, const QLUIImageInput *in)
{
    if (ui == NULL || in == NULL || in->name == NULL || in->data == NULL)
        return QL_ERR_INVAL;
    if (ui->images == NULL)
        return QL_ERR_INVAL;           /* 这个界面里没有 <img> */

    int hit = 0;
    ql_err_t first_err = QL_OK;
    for (uint16_t i = 0; i < ui->image_count; i++) {
        struct QLUIImageRT *ir = &ui->images[i];
        const char *nm = (ir->node != NULL) ? ir->node->text : ir->name;
        if (nm == NULL || strcmp(nm, in->name) != 0)
            continue;
        hit = 1;

        QLImageDecodeReq req;
        memset(&req, 0, sizeof(req));
        req.data = in->data;
        req.len = in->len;
        req.src_format = in->format;
        req.pixel_format = in->pixel_format;
        req.w = in->w;
        req.h = in->h;

        QLImage img;
        const ql_err_t err = ql_image_decode(&req, &img);
        if (err != QL_OK) {
            ir->failed = 1;
            ir->img.px = NULL;
            if (first_err == QL_OK)
                first_err = err;
            continue;
        }
        if (ir->img.owned)
            ql_mem_free((void *)ir->img.px);
        ir->img = img;
        ir->failed = 0;
        /* 背景图资源没有对应节点（node == NULL）：整屏标脏就够了 */
        if (ir->node != NULL)
            ir->node->flags |= QL_UI_RT_DIRTY;
        else if (ui->node_count > 0)
            ui->nodes[0].flags |= QL_UI_RT_DIRTY;
    }
    if (!hit)
        return QL_ERR_INVAL;           /* 名字对不上任何 <img src> */

    ql_ui_layout(ui, ui->width, ui->height);   /* 固有尺寸变了，必须重排 */
    return first_err;
}

/*
 * 把图片按 object-fit 摆进内容盒。全部**整数运算**（避免浮点在 MCU 上的代价
 * 与两端不一致）：比较用交叉相乘，尺寸用整数除法。
 *
 * 返回 0 表示不用画。
 */
static int image_fit_rect(int32_t iw, int32_t ih, QLRect box, uint8_t fit,
                          QLRect *out_dst, QLRect *out_src)
{
    if (iw <= 0 || ih <= 0 || ql_rect_is_empty(box))
        return 0;

    switch (fit) {
    case QL_UI_FIT_NONE:
        /* 原始像素尺寸，居中；超出部分由调用方按内容盒裁 */
        out_dst->x = box.x + (box.w - (ql_coord_t)iw) / 2;
        out_dst->y = box.y + (box.h - (ql_coord_t)ih) / 2;
        out_dst->w = (ql_coord_t)iw;
        out_dst->h = (ql_coord_t)ih;
        *out_src = ql_rect_make(0, 0, (ql_coord_t)iw, (ql_coord_t)ih);
        return 1;

    case QL_UI_FIT_CONTAIN:
    case QL_UI_FIT_SCALE_DOWN: {
        /*
         * 等比装进去：scale = min(box.w/iw, box.h/ih)。
         * 用交叉相乘比较大小，避免浮点：box.w*ih <= box.h*iw 表示受宽度限制。
         */
        int64_t bw = (int64_t)box.w * ih;
        int64_t bh = (int64_t)box.h * iw;
        int shrink = 0;
        if (fit == QL_UI_FIT_SCALE_DOWN)
            shrink = (box.w < iw || box.h < ih);
        if (bw <= bh) {
            out_dst->w = box.w;
            out_dst->h = (ql_coord_t)((int64_t)box.w * ih / iw);
        } else {
            out_dst->h = box.h;
            out_dst->w = (ql_coord_t)((int64_t)box.h * iw / ih);
        }
        if (fit == QL_UI_FIT_SCALE_DOWN && !shrink) {
            /* 不缩小：等同于 none（原始尺寸），但不超过内容盒 */
            out_dst->w = (ql_coord_t)((iw < box.w) ? iw : box.w);
            out_dst->h = (ql_coord_t)((ih < box.h) ? ih : box.h);
        }
        out_dst->x = box.x + (box.w - out_dst->w) / 2;
        out_dst->y = box.y + (box.h - out_dst->h) / 2;
        *out_src = ql_rect_make(0, 0, (ql_coord_t)iw, (ql_coord_t)ih);
        return 1;
    }

    case QL_UI_FIT_COVER: {
        /*
         * 等比盖满：目标就是内容盒，源取中间那一块。
         * 受宽度限制时源高 = box.h * iw / box.w（小于 ih），居中裁上下。
         */
        out_dst->x = box.x;
        out_dst->y = box.y;
        out_dst->w = box.w;
        out_dst->h = box.h;
        /*
         * scale = max(box.w/iw, box.h/ih)，用交叉相乘比较：
         *   bw > bh  ⟺  box.w/iw > box.h/ih  ⟺  受**高度**限制 → 裁上下
         *   bw <= bh                          ⟺  受宽度限制 → 裁左右
         */
        int64_t bw = (int64_t)box.w * ih;
        int64_t bh = (int64_t)box.h * iw;
        if (bw > bh) {
            const ql_coord_t sh = (ql_coord_t)((int64_t)box.h * iw / box.w);
            *out_src = ql_rect_make(0, (ql_coord_t)((ih - sh) / 2), (ql_coord_t)iw,
                                    (sh > 0) ? sh : 1);
        } else {
            const ql_coord_t sw = (ql_coord_t)((int64_t)box.w * ih / box.h);
            *out_src = ql_rect_make((ql_coord_t)((iw - sw) / 2), 0,
                                    (sw > 0) ? sw : 1, (ql_coord_t)ih);
        }
        return 1;
    }

    default:   /* fill */
        *out_dst = box;
        *out_src = ql_rect_make(0, 0, (ql_coord_t)iw, (ql_coord_t)ih);
        return 1;
    }
}

/* 绘制：object-fit 五态 + 跟随 border-radius 的圆角裁剪 */
/*
 * EMBED 包：这些标签**没有内容可画**，画的是"此处本应有东西"的提示。
 *
 * 用它自己的标签名当提示文字（`iframe` / `video` / `svg` …）而不是中文
 * "未支持"，有两个原因：内建位图字体只有 ASCII（中文会变成 `???`），
 * 而且标签名本身就是排查线索 —— 看到 `iframe` 三个字就知道该改哪儿。
 *
 * 只画在盒子够大的时候：太小的盒子里文字只会糊成一团。
 */
static int tag_is_embed_box(uint16_t tag)
{
    return tag == QL_UI_TAG_IFRAME || tag == QL_UI_TAG_EMBED ||
           tag == QL_UI_TAG_OBJECT || tag == QL_UI_TAG_AUDIO ||
           tag == QL_UI_TAG_VIDEO || tag == QL_UI_TAG_CANVAS ||
           tag == QL_UI_TAG_SVG || tag == QL_UI_TAG_MATH;
}

static void draw_embed_placeholder(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);
    const QLRect box = node->content_box;
    if (ql_rect_is_empty(box) || box.w < 32 || box.h < 12)
        return;

    const char *name = ql_ui_tag_name(node->src->tag);
    if (name == NULL)
        return;

    /* 竖直居中：把盒子收成一行文本的高度再交给 draw_text */
    const ql_coord_t lh = (s->line_h > 0) ? s->line_h : font_of(node)->line_height;
    QLRect tb = box;
    if (lh > 0 && tb.h > lh) {
        tb.y += (ql_coord_t)((tb.h - lh) / 2);
        tb.h = lh;
    }

    QLTextStyle ts;
    memset(&ts, 0, sizeof(ts));
    ts.color = s->text_color;
    ts.line_h = s->line_h;
    ts.align = s->text_align;
    d2d->draw_text(d2d, font_of(node), name, -1, tb, &ts);
}

/*
 * `background-image: url(...)` 的绘制（B4 收尾）。
 *
 * `object-fit` 的同一套几何计算拿来用：
 *   - `background-size: cover/contain` → 分别对应 QL_UI_FIT_COVER / CONTAIN
 *   - `background-size: <w> <h>`       → 先把目标矩形按给定尺寸算出来，再用 fill 语义
 *   - `background-position`            → 在"多出来的余量"里摆（与 object-position 同式）
 * `background-repeat` 则把目标矩形按平铺的步长复制，**裁剪到边框盒**（含圆角由 blit 处理）。
 */
static void draw_bg_image(QLUINodeRT *node, QLRect box, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);
    if (s == NULL || s->bg_image == QL_UI_NO_IMAGE || d2d->blit_image == NULL)
        return;
    const QLUIRoot *ui = node->owner;
    if (ui == NULL || ui->doc == NULL || ui->doc->text_pool == NULL ||
        ui->doc->text_offsets == NULL)
        return;
    const char *nm = ui->doc->text_pool + ui->doc->text_offsets[s->bg_image];
    const QLImage *img = image_by_name(ui, nm);
    if (img == NULL || ql_rect_is_empty(box))
        return;

    const ql_coord_t iw = (ql_coord_t)img->w;
    const ql_coord_t ih = (ql_coord_t)img->h;
    if (iw <= 0 || ih <= 0)
        return;

    /* 目标尺寸 */
    ql_coord_t dw = iw, dh = ih;
    if (s->bg_size_mode == 1 || s->bg_size_mode == 2) {
        /* cover：等比放大到盖满；contain：等比缩到装进去 */
        const int64_t bw = (int64_t)box.w * ih;
        const int64_t bh = (int64_t)box.h * iw;
        const int cover_wins = (s->bg_size_mode == 1) ? (bw >= bh) : (bw <= bh);
        if (cover_wins) {
            dw = box.w;
            dh = (ql_coord_t)((int64_t)box.w * ih / iw);
        } else {
            dh = box.h;
            dw = (ql_coord_t)((int64_t)box.h * iw / ih);
        }
        if (dw <= 0)
            dw = 1;
        if (dh <= 0)
            dh = 1;
    } else {
        const int w_set = (s->bg_size[0].unit != QL_UI_UNIT_AUTO);
        const int h_set = (s->bg_size[1].unit != QL_UI_UNIT_AUTO);
        if (w_set && h_set) {
            dw = resolve_length(s->bg_size[0], box.w, iw);
            dh = resolve_length(s->bg_size[1], box.h, ih);
        } else if (w_set) {
            dw = resolve_length(s->bg_size[0], box.w, iw);
            dh = (ql_coord_t)((int64_t)dw * ih / iw);
        } else if (h_set) {
            dh = resolve_length(s->bg_size[1], box.h, ih);
            dw = (ql_coord_t)((int64_t)dh * iw / ih);
        }
        if (dw <= 0)
            dw = iw;
        if (dh <= 0)
            dh = ih;
    }

    /* 起始位置：默认居中；`background-position` 按余量摆 */
    ql_coord_t ox = (box.w - dw) / 2;
    ql_coord_t oy = (box.h - dh) / 2;
    const QLUILength *px = &s->bg_pos[0];
    const QLUILength *py = &s->bg_pos[1];
    if (px->unit != 0 || px->value != 500)
        ox = (px->unit == QL_UI_UNIT_PERCENT)
                 ? (ql_coord_t)((int32_t)(box.w - dw) * px->value / 1000)
                 : (ql_coord_t)px->value;
    if (py->unit != 0 || py->value != 500)
        oy = (py->unit == QL_UI_UNIT_PERCENT)
                 ? (ql_coord_t)((int32_t)(box.h - dh) * py->value / 1000)
                 : (ql_coord_t)py->value;

    const QLRect src = ql_rect_make(0, 0, iw, ih);
    const ql_coord_t rad = ql_ui_radius_max(s);
    d2d->image_pixelated = (s->image_rendering != 0);

    const int rep_x = (s->bg_repeat == 0 || s->bg_repeat == 2);
    const int rep_y = (s->bg_repeat == 0 || s->bg_repeat == 3);

    /*
     * 平铺：先把起点挪到"第一个完全/部分可见的瓦片"，再逐块 blit。
     * 不做这一步的话，`no-repeat` 之外的图会从盒子外开始画（多画一堆看不见的）。
     */
    ql_coord_t start_x = box.x + ox;
    ql_coord_t start_y = box.y + oy;
    if (rep_x && dw > 0) {
        const ql_coord_t rel = ((start_x - box.x) % dw + dw) % dw;
        start_x = box.x + rel - ((rel > 0) ? dw : 0);
    }
    if (rep_y && dh > 0) {
        const ql_coord_t rel = ((start_y - box.y) % dh + dh) % dh;
        start_y = box.y + rel - ((rel > 0) ? dh : 0);
    }

    const QLRect saved_clip = node->owner->clip;
    int clipped = 0;
    if ((rep_x || rep_y) && d2d->set_clip != NULL) {
        /* 平铺必须裁到盒子，否则会画到兄弟节点上去 */
        const QLRect c = ql_rect_intersect(box, saved_clip);
        if (!ql_rect_is_empty(c)) {
            d2d->set_clip(d2d, c);
            node->owner->clip = c;
            clipped = 1;
        }
    }

    for (ql_coord_t ty = start_y; ty < ql_rect_bottom(box); ty += dh) {
        for (ql_coord_t tx = start_x; tx < ql_rect_right(box); tx += dw) {
            const QLRect dst = ql_rect_make(tx, ty, dw, dh);
            d2d->blit_image(d2d, img, dst, &src, rad, 255);
            if (!rep_x)
                break;
        }
        if (!rep_y)
            break;
    }

    if (clipped) {
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, saved_clip);
        node->owner->clip = saved_clip;
    }
}

static void draw_img(QLUIRoot *ui, QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLImage *img = ql_ui_node_image(ui, node);
    const QLUIStyle *s = ql_ui_node_style(node);
    if (img == NULL || ql_rect_is_empty(node->content_box) || d2d->blit_image == NULL)
        return;

    QLRect dst, src;
    if (!image_fit_rect((int32_t)img->w, (int32_t)img->h, node->content_box,
                        ql_ui_style_object_fit(s), &dst, &src))
        return;

    /*
     * `object-position`：图片在"多出来的余量"里怎么摆。
     *   - contain / none / scale-down：余量在**目标盒**里（改 dst 的偏移）；
     *   - cover：目标盒已经盖满，余量在**源**里（改 src 的起点，决定露哪一块）。
     * 百分比按千分比存（500 = 居中），px 就是绝对偏移（CSS 语义）。
     */
    if (s->object_position[0].unit != 0 || s->object_position[0].value != 500 ||
        s->object_position[1].unit != 0 || s->object_position[1].value != 500) {
        const QLUILength *px = &s->object_position[0];
        const QLUILength *py = &s->object_position[1];
        const ql_coord_t slack_x = node->content_box.w - dst.w;
        const ql_coord_t slack_y = node->content_box.h - dst.h;
        const ql_coord_t ox = (px->unit == QL_UI_UNIT_PERCENT)
                                  ? (ql_coord_t)((int32_t)slack_x * px->value / 1000)
                                  : (ql_coord_t)px->value;
        const ql_coord_t oy = (py->unit == QL_UI_UNIT_PERCENT)
                                  ? (ql_coord_t)((int32_t)slack_y * py->value / 1000)
                                  : (ql_coord_t)py->value;
        if (slack_x != 0)
            dst.x = node->content_box.x + ox;
        if (slack_y != 0)
            dst.y = node->content_box.y + oy;
        /* cover：源也按同样比例挪（否则 object-position 对 cover 无效） */
        if (ql_ui_style_object_fit(s) == QL_UI_FIT_COVER) {
            const ql_coord_t ssx = (ql_coord_t)img->w - src.w;
            const ql_coord_t ssy = (ql_coord_t)img->h - src.h;
            if (ssx > 0 && px->unit == QL_UI_UNIT_PERCENT)
                src.x = (ql_coord_t)((int32_t)ssx * px->value / 1000);
            if (ssy > 0 && py->unit == QL_UI_UNIT_PERCENT)
                src.y = (ql_coord_t)((int32_t)ssy * py->value / 1000);
        }
    }

    /*
     * `object-fit: none`/`contain` 时图可能比内容盒大（或位置超出），
     * 按内容盒裁一下 —— 与 overflow: hidden 的效果一致。
     * 圆角继续交给 blit 的 radius（用它自己的 SDF，边缘才平滑）。
     */
    const QLRect saved = node->owner->clip;
    const int need_clip = dst.w > node->content_box.w || dst.h > node->content_box.h;
    if (need_clip) {
        const QLRect c = ql_rect_intersect(node->content_box, saved);
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, c);
        node->owner->clip = c;
    }
    /* 图片的圆角只能用单半径（blit 接口如此），四角不同时取最大 */
    const ql_coord_t radius = ql_ui_radius_max(s);
    if (d2d != NULL)
        d2d->image_pixelated = (s->image_rendering != 0);
    d2d->blit_image(d2d, img, dst, &src, radius, 255);
    if (need_clip) {
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, saved);
        node->owner->clip = saved;
    }
}

/* ------------------------------ 进度条 ------------------------------ */

static struct QLUIProgress *progress_of(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->progresses == NULL)
        return NULL;
    for (uint16_t i = 0; i < ui->progress_count; i++)
        if (ui->progresses[i].node == node)
            return &ui->progresses[i];
    return NULL;
}

/*
 * 进度条只有两笔：轨道（普通背景，已经画过了）与填充（这里）。
 * 填充用 `color`，所以换配色不需要碰 C 代码。
 *
 * 不确定进度（没有 value）画一个固定 1/3 宽的条 —— **不做动画**：
 * 会跑的进度条要求主循环一直醒着，本库的省电优先原则不允许。
 */
static void draw_progress(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);
    if (s->text_color == 0 && s->bg_alpha == 0)
        return;                       /* 没说要不要画，交给 CSS */
    const struct QLUIProgress *pg = progress_of(node->owner, node);
    if (pg == NULL || ql_rect_is_empty(node->content_box))
        return;

    QLRect fill = node->content_box;
    const int permille = pg->known ? (int)pg->permille : 333;
    fill.w = (ql_coord_t)((int32_t)fill.w * permille / 1000);
    if (fill.w <= 0)
        return;
    fill.h = node->content_box.h;
    d2d->fill_rect(d2d, fill, s->text_color, 255);
}

/* ------------------------------ 输入框绘制 ------------------------------ */

/*
 * 输入框在文字之后多画两样：选区（半透明高亮）与光标（1px 竖线）。
 *
 * 密码框：文本已经在上一步画过了 —— 所以这里不重复处理，
 * 而是在 draw_text 之前把显示用的字符串换掉（见 render_node 里 password_text）。
 */
static void draw_input_decor(QLUINodeRT *node, QLDraw2D *d2d)
{
    QLUIRoot *ui = node->owner;
    struct QLUIInput *in = input_of(ui, node);
    if (in == NULL)
        return;
    const QLFont *font = font_of_style(node, node->style);
    if (font == NULL)
        return;

    const QLRect cb = node->content_box;
    const ql_coord_t base_x = cb.x - in->scroll_x;

    /*
     * 光标与选区也要裁到内容盒：多行输入框滚出可见区之后，
     * 光标会画到框下面去（截图里"竖线跑出文本框"就是这个）。
     */
    QLRect deco_saved = cb;
    int deco_clipped = 0;
    if (d2d->fill_rect != NULL && !ql_rect_is_empty(cb)) {
        deco_saved = node->owner->clip;
        const QLRect c = ql_rect_intersect(deco_saved, cb);
        node->owner->clip = c;
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, c);
        deco_clipped = 1;
    }

    /* 密码框显示成圆点，所以宽度必须按**显示文本**量，否则光标会飘 */
    char dots[QL_CFG_UI_INPUT_LEN * 3 + 4];
    const int pw = (node->src->flags & QL_UI_NODE_PASSWORD) != 0;
    const char *disp = input_display_text(in, pw, dots, sizeof(dots));

    /* 原字节偏移 → 显示文本的字节偏移 */
    #define DISP_OFF(o)                                                        \
        ((uint16_t)((pw) ? (uint16_t)(cp_count(in->buf, (o)) * 3) : (o)))

    /*
     * 多行（textarea）：选区与光标都得按**视觉行**摆。
     * 单行走下面那条老路径（逐字节量宽度），保持像素级不变。
     */
    const ql_coord_t lh = text_line_h_of(node->style, font);
    const ql_coord_t base_y = cb.y - in->scroll_y;
    /*
     * 光标要盖在**字形**上：文本绘制有半行距（line-height 大于 ascent+descent
     * 时上下各分一半，小于时是负的），光标必须跟着挪同样的距离，
     * 否则行距和字体行高不等时，越往下偏得越多。
     */
    const ql_coord_t ink_h = (ql_coord_t)(font->ascent + font->descent);
    const ql_coord_t half_lead =
        (node->style->line_h > 0) ? (ql_coord_t)(node->style->line_h - ink_h) / 2 : 0;

    uint16_t a = in->sel;
    uint16_t b = in->caret;
    if (a > b) {
        const uint16_t t = a;
        a = b;
        b = t;
    }

    if (in->multiline && d2d->fill_rect != NULL) {
        QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
        int count = input_visual_lines(font, in, cb.w, lines, QL_CFG_TEXT_MAX_LINES);
        if (count > QL_CFG_TEXT_MAX_LINES)
            count = QL_CFG_TEXT_MAX_LINES;
        if (count > 0) {
            /* 选区：逐行铺（跨行时每行一段，行尾到行首是两段，不是一个大矩形） */
            if (a != b) {
                for (int i = 0; i < count; i++) {
                    const uint16_t ls = (uint16_t)(lines[i].start - in->buf);
                    const uint16_t le = (uint16_t)(ls + lines[i].len);
                    if (b <= ls || a >= le)
                        continue;
                    const uint16_t s0 = (a > ls) ? a : ls;
                    const uint16_t s1 = (b < le) ? b : le;
                    const ql_coord_t x0 = base_x + ql_font_line_width(font, in->buf + ls,
                                                                     (int32_t)(s0 - ls));
                    const ql_coord_t x1 = base_x + ql_font_line_width(font, in->buf + ls,
                                                                     (int32_t)(s1 - ls));
                    if (x1 > x0) {
                        QLRect r = ql_rect_make(x0, base_y + (ql_coord_t)i * lh,
                                                x1 - x0, lh);
                        d2d->fill_rect(d2d, r, node->style->text_color, 0x50);
                    }
                }
            }
            if (ui->focus == node) {
                ql_coord_t cx = 0, cy = 0;
                caret_px(font, in, lines, count, lh, &cx, &cy);
                if (ui->preedit_len > 0)
                    cx += ql_font_line_width(font, ui->preedit,
                                             (int32_t)ui->preedit_caret);
                QLRect r = ql_rect_make(base_x + cx, base_y + cy + half_lead, 1,
                                        (ink_h > 0) ? ink_h : lh);
                d2d->fill_rect(d2d, r, node->style->text_color, 255);
            }

            /*
             * 内容比盒子高就画一根滑块。它跟容器滚动条长得一样，但位置由
             * `in->scroll_y` 决定 —— 多行输入框的内容是自己画的，没有子节点。
             */
            const ql_coord_t content_h = (ql_coord_t)count * lh;
            if (content_h > cb.h && d2d->fill_round_rect != NULL) {
                ql_coord_t th = 0, ty = 0;
                scrollbar_thumb(cb.h, content_h - cb.h, in->scroll_y, &th, &ty);
                if (th > 0) {
                    QLRect bar = ql_rect_make(ql_rect_right(cb) - SCROLLBAR_INSET - SCROLLBAR_W,
                                              cb.y + ty, SCROLLBAR_W, th);
                    d2d->fill_round_rect(d2d, bar, SCROLLBAR_W / 2, 0x808080, 0x96);
                }
            }
        }
        /* 提前返回前**必须**恢复裁剪：否则后面所有节点都被裁在输入框里 */
        if (deco_clipped) {
            node->owner->clip = deco_saved;
            if (d2d->set_clip != NULL)
                d2d->set_clip(d2d, deco_saved);
        }
        return;
    }

    /* 选区（单行） */
    if (a != b && d2d->fill_rect != NULL) {
        const ql_coord_t x0 = base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(a));
        const ql_coord_t x1 = base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(b));
        if (x1 > x0) {
            QLRect r = ql_rect_make(x0, cb.y, x1 - x0, cb.h);
            d2d->fill_rect(d2d, r, node->style->text_color, 0x50);
        }
    }

    /*
     * 光标：只在聚焦时画（不闪 —— 闪烁要主循环一直醒着，省电优先）。
     * 正在组字时画的是**预编辑文本**（输入法交给客户端画），光标跟着它走。
     */
    if (ui->focus == node && d2d->fill_rect != NULL) {
        ql_coord_t cx =
            base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(in->caret));
        /* 组字中：光标跟着预编辑里的插入点走（预编辑本身已经拼进正文画了） */
        if (ui->preedit_len > 0)
            cx += ql_font_line_width(font, ui->preedit, (int32_t)ui->preedit_caret);
        const ql_coord_t ch = (cb.h > 4) ? cb.h - 4 : cb.h;
        QLRect r = ql_rect_make(cx, cb.y + 2, 1, ch);
        d2d->fill_rect(d2d, r, node->style->text_color, 255);
    }
    #undef DISP_OFF

    if (deco_clipped) {
        node->owner->clip = deco_saved;
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, deco_saved);
    }
}

/*
 * 画子节点：**先画普通流，再画 sticky**。
 *
 * CSS 的绘制顺序里，定位元素（relative/absolute/fixed/sticky）画在普通流之后。
 * 不这么做的话，`position: sticky` 的表头会被"滚上来的下一个兄弟"盖住
 * （我们按兄弟顺序画，而 sticky 头通常是第一个子节点）——实测就是
 * "表头上半截透出下一行的文字"。
 *
 * 只做 sticky：其它定位元素的历史行为（兄弟顺序）暂时不动，避免影响既有基准；
 * 需要时把判断换成 `position != STATIC` 即可（见 CSS3 覆盖 §19.1）。
 */
static void render_children(QLUINodeRT *node, QLDraw2D *d2d)
{
    int has_sticky = 0;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style != NULL && c->style->position == QL_UI_POSITION_STICKY) {
            has_sticky = 1;
            continue;
        }
        render_node(c, d2d);
    }
    if (!has_sticky)
        return;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style != NULL && c->style->position == QL_UI_POSITION_STICKY)
            render_node(c, d2d);
    }
}

/*
 * 虚线 / 点线的边框与轮廓。
 *
 * 做法是**沿四条边切段填矩形**，不碰 SDF：这样 CPU 与 GL 两条后端天然一致
 * （都用同一个 fill_rect），也不给着色器加分支。
 * 代价是 `border-radius` 在这两种样式下按直角处理（真实浏览器会沿着圆角绕，
 * 见《CSS3 覆盖与裁剪》§18.5 的已知差异）。
 *
 * 图案尺寸按边框宽度的整数倍走（dashed: 段 3w + 缝 2w；dotted: 点 1w + 缝 1w），
 * 与浏览器"随线宽缩放"的行为一致，1px 边框也不会糊成一条灰线。
 */
static void paint_dash_run(QLDraw2D *d2d, ql_coord_t x, ql_coord_t y, ql_coord_t len,
                           ql_coord_t w, int vertical, uint8_t style, ql_color_t color)
{
    ql_coord_t dash = (style == QL_UI_BORDER_DOTTED) ? w : 3 * w;
    ql_coord_t gap = (style == QL_UI_BORDER_DOTTED) ? w : 2 * w;
    if (dash < 1)
        dash = 1;
    if (gap < 1)
        gap = 1;
    for (ql_coord_t p = 0; p < len; p += dash + gap) {
        ql_coord_t d = len - p;
        if (d > dash)
            d = dash;
        const QLRect seg = vertical ? ql_rect_make(x, y + p, w, d)
                                    : ql_rect_make(x + p, y, d, w);
        if (style == QL_UI_BORDER_DOTTED && w >= 4)
            d2d->fill_round_rect(d2d, seg, w / 2, color, 255);
        else
            d2d->fill_rect(d2d, seg, color, 255);
    }
}

/* 四条边都切成段（rect 是边框盒外侧那一圈所占的区域） */
static void paint_dash_frame(QLDraw2D *d2d, QLRect rect, ql_coord_t w, uint8_t style,
                             ql_color_t color)
{
    if (w <= 0 || rect.w <= 2 * w || rect.h <= 2 * w)
        return;
    const ql_coord_t right = ql_rect_right(rect) - w;
    const ql_coord_t bottom = ql_rect_bottom(rect) - w;
    paint_dash_run(d2d, rect.x, rect.y, rect.w, w, 0, style, color);          /* 上 */
    paint_dash_run(d2d, rect.x, bottom, rect.w, w, 0, style, color);          /* 下 */
    paint_dash_run(d2d, rect.x, rect.y + w, rect.h - 2 * w, w, 1, style,       /* 左 */
                   color);
    paint_dash_run(d2d, right, rect.y + w, rect.h - 2 * w, w, 1, style,        /* 右 */
                   color);
}

/* 四角半径 → 设备/逻辑坐标下的数组（给 4 半径接口用） */
static void style_radii4(const QLUIStyle *s, ql_coord_t r4[4])
{
    /*
     * 椭圆圆角（`border-radius: h / v`，blob v34）：两个后端都用**单半径**
     * 的圆角 SDF（CPU 的 `round_row_span` / GL 的 `sdRoundBox` 各接一个标量），
     * 要精确画椭圆角得给每角再加 4 个顶点属性并改写两个 SDF。
     * 这里取两者的**算术平均**作为等价半径：视觉上最接近，且不会像 max()
     * 那样溢出盒子。精确度由《CSS3 覆盖与裁剪》§5 明确说明。
     */
    for (int i = 0; i < 4; i++) {
        ql_coord_t r = (ql_coord_t)s->radius[i];
        if (s->radius_v[i] > 0)
            r = (r + (ql_coord_t)s->radius_v[i]) / 2;
        r4[i] = r;
    }
}

/* 圆角描边（自动选 4 半径 / 单半径 / 直角三条路） */
static void stroke_rect_r4(QLDraw2D *d2d, QLRect rect, const ql_coord_t r4[4],
                           ql_coord_t thickness, ql_color_t color)
{
    if (r4[0] == 0 && r4[1] == 0 && r4[2] == 0 && r4[3] == 0) {
        d2d->stroke_round_rect(d2d, rect, 0, thickness, color);
        return;
    }
    if (r4[0] == r4[1] && r4[0] == r4[2] && r4[0] == r4[3]) {
        d2d->stroke_round_rect(d2d, rect, r4[0], thickness, color);
        return;
    }
    if (d2d->stroke_round_rect4 != NULL) {
        d2d->stroke_round_rect4(d2d, rect, r4, thickness, color);
        return;
    }
    ql_coord_t m = r4[0];
    for (int i = 1; i < 4; i++)
        if (r4[i] > m)
            m = r4[i];
    d2d->stroke_round_rect(d2d, rect, m, thickness, color);
}

static void fill_rect_r4(QLDraw2D *d2d, QLRect rect, const ql_coord_t r4[4],
                         ql_color_t color, uint8_t alpha)
{
    if (r4[0] == 0 && r4[1] == 0 && r4[2] == 0 && r4[3] == 0) {
        d2d->fill_round_rect(d2d, rect, 0, color, alpha);
        return;
    }
    if (r4[0] == r4[1] && r4[0] == r4[2] && r4[0] == r4[3]) {
        d2d->fill_round_rect(d2d, rect, r4[0], color, alpha);
        return;
    }
    if (d2d->fill_round_rect4 != NULL) {
        d2d->fill_round_rect4(d2d, rect, r4, color, alpha);
        return;
    }
    ql_coord_t m = r4[0];
    for (int i = 1; i < 4; i++)
        if (r4[i] > m)
            m = r4[i];
    d2d->fill_round_rect(d2d, rect, m, color, alpha);
}


#if QL_FEAT_CSS_BORDER_IMAGE

/*
 * `border-image` 的九宫格绘制。
 *
 * 一张图切 9 块：四角原样、四边单向缩放/平铺、中间双向缩放/平铺。
 * 全部走 `blit_image(dst, src)`（后端已有子矩形采样），所以 CPU 与 GL
 * 天然一致，也不需要新的着色器。
 *
 * 与 CSS 的差异（写进覆盖表）：不支持 `border-image-outset`（不向外扩），
 * `border-image-width` 的百分比按**边框盒**对应边解释，`space` 按 `repeat` 处理。
 */
static void blit_band(QLDraw2D *d2d, const QLImage *img, QLRect dst, QLRect src,
                      uint8_t repeat_mode, int horizontal)
{
    const ql_coord_t len = horizontal ? dst.w : dst.h;
    const ql_coord_t tile = horizontal ? src.w : src.h;
    if (len <= 0 || tile <= 0)
        return;
    if (repeat_mode == QL_UI_BI_STRETCH) {
        d2d->blit_image(d2d, img, dst, &src, 0, 255);
        return;
    }
    int n = 0;
    ql_coord_t step = tile;
    if (repeat_mode == QL_UI_BI_ROUND) {
        n = (int)((len + tile / 2) / tile);
        if (n < 1)
            n = 1;
        step = len / n;
        if (step < 1)
            step = 1;
        n = (int)((len + step - 1) / step);
    } else {
        n = (int)((len + tile - 1) / tile);   /* repeat / space 都按满格铺 */
    }
    for (int i = 0; i < n; i++) {
        const ql_coord_t off = (ql_coord_t)i * step;
        ql_coord_t w = (horizontal) ? step : dst.w;
        ql_coord_t h = (horizontal) ? dst.h : step;
        if (off + w > len)
            w = len - off;
        if (w <= 0)
            break;
        QLRect d = horizontal ? ql_rect_make(dst.x + off, dst.y, w, dst.h)
                              : ql_rect_make(dst.x, dst.y + off, dst.w, h);
        d2d->blit_image(d2d, img, d, &src, 0, 255);
    }
}

static void draw_border_image(QLUINodeRT *node, QLRect box, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);
    if (s == NULL || d2d == NULL || d2d->blit_image == NULL ||
        s->border_image == QL_UI_NO_BORDER_IMAGE || ql_rect_is_empty(box))
        return;
    const QLUIRoot *ui = node->owner;
    if (ui == NULL || ui->doc == NULL || ui->doc->text_pool == NULL ||
        ui->doc->text_offsets == NULL)
        return;
    const char *nm = ui->doc->text_pool + ui->doc->text_offsets[s->border_image];
    const QLImage *img = image_by_name(ui, nm);
    if (img == NULL || img->w == 0 || img->h == 0)
        return;

    const ql_coord_t iw = (ql_coord_t)img->w;
    const ql_coord_t ih = (ql_coord_t)img->h;

    /* 切片（源图上的像素数） */
    ql_coord_t sl[4];
    for (int i = 0; i < 4; i++) {
        int v = s->bi_slice[i];
        if (v <= 0)
            sl[i] = 0;
        else if (s->bi_slice_pct)
            sl[i] = (ql_coord_t)(((int32_t)((i == 1 || i == 3) ? iw : ih) * v) / 1000);
        else
            sl[i] = (ql_coord_t)v;
    }
    /* 左+右 / 上+下 不能超过图片本身 */
    if (sl[1] + sl[3] > iw) {
        const ql_coord_t over = sl[1] + sl[3] - iw;
        sl[1] -= over / 2;
        sl[3] -= over - over / 2;
    }
    if (sl[0] + sl[2] > ih) {
        const ql_coord_t over = sl[0] + sl[2] - ih;
        sl[0] -= over / 2;
        sl[2] -= over - over / 2;
    }
    if (sl[0] < 0)
        sl[0] = 0;
    if (sl[1] < 0)
        sl[1] = 0;
    if (sl[2] < 0)
        sl[2] = 0;
    if (sl[3] < 0)
        sl[3] = 0;

    /* 目标边框宽度（布局 px） */
    ql_coord_t wl[4];
    for (int i = 0; i < 4; i++) {
        int v = s->bi_width[i];
        if (v <= 0)
            wl[i] = sl[i];          /* 默认用切片值 */
        else if (s->bi_width_pct)
            wl[i] = (ql_coord_t)(((int32_t)((i == 1 || i == 3) ? box.w : box.h) * v) / 1000);
        else
            wl[i] = (ql_coord_t)v;
        if (wl[i] < 0)
            wl[i] = 0;
    }
    /* 目标也不能超过盒子 */
    if (wl[1] + wl[3] > box.w) {
        wl[1] = box.w / 2;
        wl[3] = box.w - wl[1];
    }
    if (wl[0] + wl[2] > box.h) {
        wl[0] = box.h / 2;
        wl[2] = box.h - wl[0];
    }

    const ql_coord_t inner_w = box.w - wl[1] - wl[3];
    const ql_coord_t inner_h = box.h - wl[0] - wl[2];
    const ql_coord_t src_mid_w = iw - sl[1] - sl[3];
    const ql_coord_t src_mid_h = ih - sl[0] - sl[2];
    const uint8_t rep = s->bi_repeat;

    /* 四角：原样缩放 */
    const ql_coord_t sw[4] = { sl[3], sl[1], sl[3], sl[1] };  /* 左上 右上 左下 右下 */
    const ql_coord_t sh[4] = { sl[0], sl[0], sl[2], sl[2] };
    const ql_coord_t sx[4] = { 0, iw - sl[1], 0, iw - sl[1] };
    const ql_coord_t sy[4] = { 0, 0, ih - sl[2], ih - sl[2] };
    const ql_coord_t dx[4] = { box.x, box.x + box.w - wl[1], box.x,
                               box.x + box.w - wl[1] };
    const ql_coord_t dy[4] = { box.y, box.y, box.y + box.h - wl[2],
                               box.y + box.h - wl[2] };
    const ql_coord_t dw[4] = { wl[3], wl[1], wl[3], wl[1] };
    const ql_coord_t dh[4] = { wl[0], wl[0], wl[2], wl[2] };
    for (int i = 0; i < 4; i++) {
        if (sw[i] <= 0 || sh[i] <= 0 || dw[i] <= 0 || dh[i] <= 0)
            continue;
        const QLRect src = ql_rect_make(sx[i], sy[i], sw[i], sh[i]);
        const QLRect dst = ql_rect_make(dx[i], dy[i], dw[i], dh[i]);
        d2d->blit_image(d2d, img, dst, &src, 0, 255);
    }

    /* 四条边 */
    if (src_mid_w > 0) {
        const QLRect src_t = ql_rect_make(sl[3], 0, src_mid_w, sl[0]);
        const QLRect src_b = ql_rect_make(sl[3], ih - sl[2], src_mid_w, sl[2]);
        if (wl[0] > 0 && inner_w > 0)
            blit_band(d2d, img, ql_rect_make(box.x + wl[3], box.y, inner_w, wl[0]),
                      src_t, rep, 1);
        if (wl[2] > 0 && inner_w > 0)
            blit_band(d2d, img,
                      ql_rect_make(box.x + wl[3], box.y + box.h - wl[2], inner_w, wl[2]),
                      src_b, rep, 1);
    }
    if (src_mid_h > 0) {
        const QLRect src_l = ql_rect_make(0, sl[0], sl[3], src_mid_h);
        const QLRect src_r = ql_rect_make(iw - sl[1], sl[0], sl[1], src_mid_h);
        if (wl[3] > 0 && inner_h > 0)
            blit_band(d2d, img, ql_rect_make(box.x, box.y + wl[0], wl[3], inner_h),
                      src_l, rep, 0);
        if (wl[1] > 0 && inner_h > 0)
            blit_band(d2d, img,
                      ql_rect_make(box.x + box.w - wl[1], box.y + wl[0], wl[1], inner_h),
                      src_r, rep, 0);
    }

    /* 中间：有源区域就填（留空说明切片已经把图切完了） */
    if (src_mid_w > 0 && src_mid_h > 0 && inner_w > 0 && inner_h > 0) {
        const QLRect src_c = ql_rect_make(sl[3], sl[0], src_mid_w, src_mid_h);
        const QLRect dst_c = ql_rect_make(box.x + wl[3], box.y + wl[0], inner_w, inner_h);
        if (rep == QL_UI_BI_STRETCH) {
            d2d->blit_image(d2d, img, dst_c, &src_c, 0, 255);
        } else {
            /* 平铺：先裁到中间区域，再按行列铺 */
            const QLRect save = node->owner->clip;
            if (d2d->set_clip != NULL)
                d2d->set_clip(d2d, dst_c);
            node->owner->clip = dst_c;
            const ql_coord_t step_x = (rep == QL_UI_BI_ROUND)
                                          ? (src_mid_w > 0 ? src_mid_w : 1)
                                          : src_mid_w;
            const ql_coord_t step_y = (rep == QL_UI_BI_ROUND)
                                          ? (src_mid_h > 0 ? src_mid_h : 1)
                                          : src_mid_h;
            for (ql_coord_t y = 0; y < inner_h; y += step_y)
                for (ql_coord_t x = 0; x < inner_w; x += step_x) {
                    QLRect d = ql_rect_make(dst_c.x + x, dst_c.y + y, step_x, step_y);
                    d2d->blit_image(d2d, img, d, &src_c, 0, 255);
                }
            node->owner->clip = save;
            if (d2d->set_clip != NULL)
                d2d->set_clip(d2d, save);
        }
    }
}

#endif  /* QL_FEAT_CSS_BORDER_IMAGE */

/*
 * 画边框。三种情形：
 *
 *   1. 四边宽度/样式/颜色全同 → 一次整圈描边（最常见，也是最快的一条）；
 *   2. 有 dashed/dotted → 逐边切段（角上按直角处理，见覆盖表）；
 *   3. 各边不同（颜色/宽度） → 把整圈描边**按边裁四次**，每次换一种颜色。
 *      第 3 条是 CSS 里"上/下边优先占角"的简化：浏览器会沿 45° 斜线切分角部，
 *      这里让上/下边整块吃掉角。视觉差异只在"四边颜色都不同 + 有圆角"时可见。
 */
static void paint_border(QLDraw2D *d2d, QLRect box, const QLUIStyle *s)
{
    ql_coord_t r4[4];
    style_radii4(s, r4);

    /* ---- 1. 全同：整圈 ---- */
    if (ql_ui_border_uniform(s)) {
        const ql_coord_t w = (ql_coord_t)s->border_w[0];
        if (w <= 0)
            return;
        if (s->border_style4[0] == QL_UI_BORDER_SOLID) {
            stroke_rect_r4(d2d, box, r4, w, s->border_color4[0]);
        } else {
            paint_dash_frame(d2d, box, w, s->border_style4[0], s->border_color4[0]);
        }
        return;
    }

    /* ---- 2. 有虚线/点线：逐边切段（各边自己的宽度与颜色） ---- */
    int any_dash = 0;
    for (int i = 0; i < 4; i++)
        if (s->border_w[i] > 0 && s->border_style4[i] != QL_UI_BORDER_SOLID &&
            s->border_style4[i] != QL_UI_BORDER_NONE)
            any_dash = 1;
    if (any_dash) {
        const ql_coord_t wt = (ql_coord_t)s->border_w[0];
        const ql_coord_t wr = (ql_coord_t)s->border_w[1];
        const ql_coord_t wb = (ql_coord_t)s->border_w[2];
        const ql_coord_t wl = (ql_coord_t)s->border_w[3];
        const ql_coord_t right = ql_rect_right(box);
        const ql_coord_t bottom = ql_rect_bottom(box);
        if (wt > 0 && s->border_style4[0] != QL_UI_BORDER_NONE)
            paint_dash_run(d2d, box.x + wl, box.y, box.w - wl - wr, wt, 0,
                           s->border_style4[0], s->border_color4[0]);
        if (wb > 0 && s->border_style4[2] != QL_UI_BORDER_NONE)
            paint_dash_run(d2d, box.x + wl, bottom - wb, box.w - wl - wr, wb, 0,
                           s->border_style4[2], s->border_color4[2]);
        if (wl > 0 && s->border_style4[3] != QL_UI_BORDER_NONE)
            paint_dash_run(d2d, box.x, box.y + wt, box.h - wt - wb, wl, 1,
                           s->border_style4[3], s->border_color4[3]);
        if (wr > 0 && s->border_style4[1] != QL_UI_BORDER_NONE)
            paint_dash_run(d2d, right - wr, box.y + wt, box.h - wt - wb, wr, 1,
                           s->border_style4[1], s->border_color4[1]);
        return;
    }

    /* ---- 3. 各边不同色/宽：整圈描边 + 按边裁剪 ---- */
    const QLRect saved_clip = d2d->user != NULL ? ql_rect_make(0, 0, 0, 0) : ql_rect_make(0, 0, 0, 0);
    (void)saved_clip;
    /*
     * 每条边一圈：厚度取**这条边的宽度**，用裁剪矩形把那一条带切出来。
     * 顺序 上 右 下 左：先上/下，后左/右 —— 左右只在"中间段"画，角归上/下。
     */
    const ql_coord_t wt = (ql_coord_t)s->border_w[0];
    const ql_coord_t wr = (ql_coord_t)s->border_w[1];
    const ql_coord_t wb = (ql_coord_t)s->border_w[2];
    const ql_coord_t wl = (ql_coord_t)s->border_w[3];

    struct {
        ql_coord_t w;
        QLRect     clip;
        uint32_t   color;
    } sides[4];

    sides[0].w = wt;
    sides[0].clip = ql_rect_make(box.x, box.y, box.w, wt);
    sides[0].color = s->border_color4[0];
    sides[2].w = wb;
    sides[2].clip = ql_rect_make(box.x, ql_rect_bottom(box) - wb, box.w, wb);
    sides[2].color = s->border_color4[2];
    sides[1].w = wr;
    sides[1].clip = ql_rect_make(ql_rect_right(box) - wr, box.y + wt, wr,
                                 box.h - wt - wb);
    sides[1].color = s->border_color4[1];
    sides[3].w = wl;
    sides[3].clip = ql_rect_make(box.x, box.y + wt, wl, box.h - wt - wb);
    sides[3].color = s->border_color4[3];

    for (int i = 0; i < 4; i++) {
        if (sides[i].w <= 0 || ql_rect_is_empty(sides[i].clip))
            continue;
        /* 整圈描边被裁到这一条带上 ⇒ 只剩这一边的颜色 */
        const QLRect c = ql_rect_intersect(sides[i].clip, box);
        if (ql_rect_is_empty(c))
            continue;
        d2d->set_clip(d2d, c);
        stroke_rect_r4(d2d, box, r4, sides[i].w, sides[i].color);
    }
    /*
     * 恢复裁剪由调用方负责（render_node_inner 传进来、用完再设回去）——
     * 这里**不能**自己设成 box：那会把后续兄弟节点一起裁掉（真踩过）。
     */
}

/*
 * `outline`：贴**边框盒外侧**画一圈，不占布局。
 * `outline-offset` 为负时那圈线会压进边框里面（CSS 语义）。
 * 用 stroke_round_rect 实现：外沿 = 边框盒外扩 (offset + width)，
 * 厚度 = width，于是这圈线正好落在 [offset, offset + width] 之间。
 */
static void paint_outline(QLDraw2D *d2d, QLRect box, ql_coord_t w, ql_coord_t off,
                          const ql_coord_t r4[4], uint8_t style, ql_color_t color)
{
    if (w <= 0)
        return;
    const ql_coord_t grow = off + w;
    const QLRect outer = ql_rect_make(box.x - grow, box.y - grow, box.w + 2 * grow,
                                      box.h + 2 * grow);
    if (outer.w <= 0 || outer.h <= 0)
        return;
    ql_coord_t ro[4];
    for (int i = 0; i < 4; i++) {
        ro[i] = r4[i] + grow;
        if (ro[i] < 0)
            ro[i] = 0;
    }
    if (style == QL_UI_BORDER_SOLID) {
        stroke_rect_r4(d2d, outer, ro, w, color);
        return;
    }
    paint_dash_frame(d2d, outer, w, style, color);
}

static void render_node_inner(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);

    if (s->display == QL_UI_DISPLAY_NONE)
        return;

    /*
     * `visibility: hidden`：**占位但不画自己**（子节点照画 —— 它是继承属性，
     * 子节点自己写了 `visible` 就该显示）。与 `display: none` 的区别是不重排。
     */
    const int inv = (s->visibility != QL_UI_VISIBLE);

    /*
     * 脏区剪枝：paint_box 是整个子树可能画到的范围的上界，
     * 与当前裁剪不相交就整棵跳过——一次矩形判断替代一次子树遍历。
     */
    const QLRect clip = node->owner->clip;
    if (!ql_rect_overlaps(node->paint_box, clip))
        return;

    /*
     * transform：加到绘制偏移上（不进布局，所以移动元素零重排）。
     * 子树跟着一起移，与 CSS 的"transform 建立包含块"语义一致；
     * clip 是设备空间的不变矩形，所以平移后的内容照样会被父级 overflow 裁掉。
     */
    const ql_coord_t saved_ox = d2d->offset_x_fx;
    const ql_coord_t saved_oy = d2d->offset_y_fx;
    if ((s->tf_x != 0 || s->tf_y != 0) && s->xform < 0) {
        /* 用乘法不用左移：tf 可能是负数，负数左移是 UB（UBSan 会报） */
        ql_draw2d_set_offset(d2d,
                             saved_ox + (ql_coord_t)s->tf_x * (ql_coord_t)QL_FX_ONE,
                             saved_oy + (ql_coord_t)s->tf_y * (ql_coord_t)QL_FX_ONE);
    }

    QLRect box = node->border_box;

    /*
     * clip-path: circle()：圆遮罩是**子树级**的（等价 CSS 会裁掉越界的子元素）。
     * 半径是"最远角距离"的千分比，所以在元素尺寸变化时也自动合适。
     */
    /*
     * transform: scale()：绘制期缩放（绕元素中心），不影响布局。
     * 因为是"绕元素中心"，子元素也跟着放大——与 CSS 的 transform 语义一致。
     */
    const ql_coord_t saved_efx = d2d->elem_x_fx;
    const ql_coord_t saved_efy = d2d->elem_y_fx;
    const ql_coord_t elem_cx = box.x + box.w / 2;
    const ql_coord_t elem_cy = box.y + box.h / 2;
    if ((s->sc_x != 1000 || s->sc_y != 1000) && s->xform < 0)
        ql_draw2d_set_elem_scale_xy(
            d2d, (ql_coord_t)((int64_t)s->sc_x * QL_SCALE_ONE / 1000),
            (ql_coord_t)((int64_t)s->sc_y * QL_SCALE_ONE / 1000), elem_cx, elem_cy);

    const ql_coord_t saved_mx = d2d->mask_cx, saved_my = d2d->mask_cy;
    const ql_coord_t saved_mr = d2d->mask_r, saved_mry = d2d->mask_ry;
    const uint8_t saved_pn = d2d->mask_pn;
    const QLRect clip_before_inset = node->owner->clip;
    int inset_clipped = 0;
    if (d2d->set_mask != NULL && s->clip_kind != QL_UI_CLIP_NONE) {
        if (s->clip_kind == QL_UI_CLIP_CIRCLE) {
            ql_coord_t dmax = ql_len_far_corner(box.w, box.h);
            ql_coord_t r = (ql_coord_t)(((int32_t)dmax * (int32_t)s->clip_r) / 1000);
            if (saved_mr >= 0)
                QL_LOGW("ui: nested clip-path is not supported (node #%u), keeping the "
                        "outer circle", (unsigned)(node - node->owner->nodes));
            else
                ql_draw2d_set_mask(d2d, box.x + box.w / 2, box.y + box.h / 2, r);
        } else if (s->clip_kind == QL_UI_CLIP_ELLIPSE) {
            /*
             * `ellipse(rx ry)`：与圆同一条解析遮罩，只是两个轴分开。
             * rx/ry 是边框盒宽/高的百分比（px 也支持）。
             */
            const ql_coord_t rx = clip_len_to_px(s->clip_box[0], box.w);
            const ql_coord_t ry = clip_len_to_px(s->clip_box[1], box.h);
            if (saved_mr >= 0)
                QL_LOGW("ui: nested clip-path is not supported (node #%u), keeping the "
                        "outer shape", (unsigned)(node - node->owner->nodes));
            else if (rx > 0 && ry > 0)
                ql_draw2d_set_mask_ellipse(d2d, box.x + box.w / 2, box.y + box.h / 2,
                                           rx, ry);
        } else if (s->clip_kind == QL_UI_CLIP_POLYGON) {
            /*
             * `polygon()`：同样是一条解析遮罩（顶点换算成绝对坐标交给后端）。
             * 覆盖率按"到最近边的距离"算，所以边是抗锯齿的、和元素尺寸无关 ——
             * 早先版本走"64×64 蒙版图拉伸"，几百像素宽的元素上边会糊成 3~4px。
             */
            if (saved_mr >= 0 || saved_pn >= 3) {
                QL_LOGW("ui: nested clip-path is not supported (node #%u), keeping the "
                        "outer shape", (unsigned)(node - node->owner->nodes));
            } else if (ui_has_clip_pts(node->owner, s)) {
                ql_coord_t px[QL_CFG_UI_CLIP_PTS_MAX];
                ql_coord_t py[QL_CFG_UI_CLIP_PTS_MAX];
                const QLClipPt *pt = &node->owner->doc->clip_pts[s->clip_pts];
                int n = (int)s->clip_pt_n;
                if (n > QL_CFG_UI_CLIP_PTS_MAX)
                    n = QL_CFG_UI_CLIP_PTS_MAX;
                for (int i = 0; i < n; i++) {
                    px[i] = box.x + clip_len_to_px(pt[i].x, box.w);
                    py[i] = box.y + clip_len_to_px(pt[i].y, box.h);
                }
                (void)ql_draw2d_set_mask_polygon(d2d, px, py, n);
            }
        }
    }
    /*
     * `clip-path: inset(...)`：纯矩形裁剪，直接与当前裁剪求交 —— 两个后端都精确、
     * 零额外成本（不需要蒙版、不需要图层）。
     */
    if (s->clip_kind == QL_UI_CLIP_INSET && d2d->set_clip != NULL) {
        const ql_coord_t t = clip_len_to_px(s->clip_box[0], box.h);
        const ql_coord_t r = clip_len_to_px(s->clip_box[1], box.w);
        const ql_coord_t b = clip_len_to_px(s->clip_box[2], box.h);
        const ql_coord_t l = clip_len_to_px(s->clip_box[3], box.w);
        const QLRect ins = ql_rect_make(box.x + l, box.y + t, box.w - l - r,
                                        box.h - t - b);
        const QLRect nc = ql_rect_intersect(node->owner->clip, ins);
        node->owner->clip = nc;
        d2d->set_clip(d2d, nc);
        inset_clipped = 1;
    }

    /*
     * 毛玻璃：必须在**画自己的背景之前**做，否则会连自己一起糊掉。
     * 模糊的是"这一层背后已经画好的内容"，所以它天然跟随 z 序。
     */
    /*
     * 带上 border-radius：模糊是方框运算，圆角缺口里不该渗出方形的模糊块。
     * 四角不同时取最大的那个（模糊只看外轮廓，差异肉眼不可见）。
     */
    if (!inv && s->backdrop_blur > 0 && !ql_rect_is_empty(box))
        (void)ql_draw2d_backdrop_blur_round(d2d, box, (int)s->backdrop_blur,
                                            ql_ui_radius_max(s));

    ql_coord_t r4[4];
    style_radii4(s, r4);

    /*
     * 阴影先画（在背景之下）；轮廓取边框盒，与 CSS box-shadow 一致。
     * 两层按书写顺序：先写的在下面（CSS 的语义是"第一层在最上面"，
     * 但我们的绘制是后画的盖住先画的，所以这里**倒序**画，
     * 让第一层最后画 → 视觉上位于最上）。
     */
    if (!inv && !ql_rect_is_empty(box)) {
        for (int i = 1; i >= 0; i--) {
            const QLUIShadow *sh = &s->shadow[i];
            if (sh->alpha == 0 || sh->inset)
                continue;
            if (d2d->fill_shadow != NULL) {
                /*
                 * `spread`：阴影轮廓整体外扩（负值内缩）。做法就是把矩形与圆角
                 * 一起按 spread 调整 —— 与浏览器一致（spread 作用于"阴影的形状"，
                 * 不是模糊半径）。
                 */
                QLRect sb = box;
                ql_coord_t sr = ql_ui_radius_max(s);
                if (sh->spread != 0) {
                    const ql_coord_t sp = (ql_coord_t)sh->spread;
                    sb.x -= sp;
                    sb.y -= sp;
                    sb.w += 2 * sp;
                    sb.h += 2 * sp;
                    sr += sp;
                    if (sr < 0)
                        sr = 0;
                }
                if (sb.w > 0 && sb.h > 0)
                    d2d->fill_shadow(d2d, sb, sr, sh->blur, sh->dx, sh->dy, sh->color,
                                     sh->alpha);
            }
        }
    }

    if (!inv && s->bg_alpha > 0 && !ql_rect_is_empty(box)) {
        const int uniform = (r4[0] == r4[1] && r4[0] == r4[2] && r4[0] == r4[3]);
        if (s->bg_grad_dir != 0 && d2d->fill_round_rect_gradient != NULL) {
            if (uniform || d2d->fill_round_rect_gradient4 == NULL)
                d2d->fill_round_rect_gradient(
                    d2d, box, uniform ? r4[0] : ql_ui_radius_max(s), s->bg_color,
                    s->bg_color2, (uint8_t)(s->bg_grad_dir - 1), s->bg_alpha);
            else
                d2d->fill_round_rect_gradient4(d2d, box, r4, s->bg_color, s->bg_color2,
                                               (uint8_t)(s->bg_grad_dir - 1), s->bg_alpha);
        } else {
            fill_rect_r4(d2d, box, r4, s->bg_color, s->bg_alpha);
        }
    }

    /* 背景图铺在背景色之上、边框之下（CSS 的绘制顺序） */
    if (!inv && !ql_rect_is_empty(box) && s->bg_image != QL_UI_NO_IMAGE)
        draw_bg_image(node, box, d2d);

#if QL_FEAT_CSS_BORDER_IMAGE
    if (!inv && !ql_rect_is_empty(box) && s->border_image != QL_UI_NO_BORDER_IMAGE) {
        /*
         * CSS 语义：写了 `border-image` 就不再画 `border-style`。
         * 同样要**原样恢复裁剪**（与 paint_border 的坑一样）。
         */
        const QLRect bi_clip = node->owner->clip;
        draw_border_image(node, box, d2d);
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, bi_clip);
        node->owner->clip = bi_clip;
    } else
#endif
    if (!inv && !ql_rect_is_empty(box) && ql_ui_has_border(s)) {
        /*
         * 各边颜色/宽度不同时，paint_border 会临时改裁剪（每条边裁一次）。
         * 必须原样恢复：否则后面的兄弟节点全被裁到这一个盒子里 ——
         * 表现就是"某一行之后整页空白"。
         */
        const QLRect border_clip = node->owner->clip;
        paint_border(d2d, box, s);
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, border_clip);
        node->owner->clip = border_clip;
    }

    /*
     * 多栏的栏间分隔线：画在背景/边框之上、内容之下。
     * 位置在每条缝的正中间（浏览器也是这么放的），高度取内容盒高。
     */
    if (!inv && node->col_n > 1 && s->column_rule_w > 0 &&
        s->column_rule_style != QL_UI_BORDER_NONE && d2d->fill_rect != NULL) {
        const ql_coord_t gap = (s->gap_col > 0) ? s->gap_col : 16;
        const QLRect cb = node->content_box;
        const ql_color_t rule_color =
            (s->column_rule_color != 0) ? s->column_rule_color : s->text_color;
        const ql_coord_t rw = (ql_coord_t)s->column_rule_w;
        const int cols = node->col_n;
        for (int i = 1; i < cols; i++) {
            const ql_coord_t x = cb.x + i * (node->col_w + gap) - gap / 2 -
                                 ((rw > 1) ? (rw / 2) : 0);
            if (s->column_rule_style == QL_UI_BORDER_SOLID)
                d2d->fill_rect(d2d, ql_rect_make(x, cb.y, rw, cb.h), rule_color, 255);
            else
                paint_dash_run(d2d, x, cb.y, cb.h, rw, 1, s->column_rule_style,
                               rule_color);
        }
    }

    /* 内阴影：画在背景/边框之上、文本之下（CSS 里 inset 阴影盖在背景之上） */
    if (!inv && !ql_rect_is_empty(box)) {
        for (int i = 1; i >= 0; i--) {
            const QLUIShadow *sh = &s->shadow[i];
            if (sh->alpha == 0 || !sh->inset)
                continue;
            if (d2d->fill_inset_shadow != NULL) {
                QLRect sb = box;
                ql_coord_t sr = ql_ui_radius_max(s);
                if (sh->spread != 0) {
                    const ql_coord_t sp = (ql_coord_t)sh->spread;
                    sb.x -= sp;
                    sb.y -= sp;
                    sb.w += 2 * sp;
                    sb.h += 2 * sp;
                    sr += sp;
                    if (sr < 0)
                        sr = 0;
                }
                if (sb.w > 0 && sb.h > 0)
                    d2d->fill_inset_shadow(d2d, sb, sr, sh->blur, sh->dx, sh->dy,
                                           sh->color, sh->alpha);
            }
        }
    }

    /* outline 画在边框之后：它在最外圈，不该被自己的边框盖住 */
    if (!inv && s->outline_width > 0 && s->outline_style != QL_UI_BORDER_NONE &&
        !ql_rect_is_empty(box))
        paint_outline(d2d, box, (ql_coord_t)s->outline_width,
                      (ql_coord_t)s->outline_offset, r4, s->outline_style,
                      s->outline_color);

    /* <img>：背景/边框画完之后贴图（P3a：object-fit: fill） */
    if (!inv && node->src != NULL && node->src->tag == QL_UI_TAG_IMG)
        draw_img(node->owner, node, d2d);

    /* EMBED 包：画标签名当占位提示（这些节点自身没有文本） */
    if (!inv && node->src != NULL && tag_is_embed_box(node->src->tag))
        draw_embed_placeholder(node, d2d);

    /* <progress>/<meter>：轨道就是背景（已经画完），这里补填充 */
    if (!inv && node->src != NULL && (node->src->tag == QL_UI_TAG_PROGRESS ||
                              node->src->tag == QL_UI_TAG_METER))
        draw_progress(node, d2d);

    /* 复选框（B2）：勾选时画一个 √（两条线，不引入新图元） */
    if (!inv && node->src != NULL && (node->src->flags & QL_UI_NODE_CHECKBOX) != 0 &&
        (node->flags & QL_UI_RT_CHECKED) != 0 && d2d->draw_line != NULL) {
        const QLRect cb = node->content_box;
        const ql_color_t tick = (s->text_color != 0) ? s->text_color : 0xFFFFFF;
        const ql_coord_t th = (cb.w >= 14) ? 2 : 1;
        const ql_coord_t x0 = cb.x + cb.w / 4;
        const ql_coord_t y0 = cb.y + cb.h / 2;
        d2d->draw_line(d2d, x0, y0, cb.x + cb.w / 2 - 1, cb.y + cb.h * 3 / 4, th, tick);
        d2d->draw_line(d2d, cb.x + cb.w / 2 - 1, cb.y + cb.h * 3 / 4,
                       ql_rect_right(cb) - cb.w / 4, cb.y + cb.h / 4, th, tick);
    }

    /*
     * <img> 的 `node->text` 是**资源名**（"logo.png"），不是要显示的文字 ——
     * 这一条必须挡住，否则资源名会被当成正文画在图片上。
     */
    /*
     * 输入框是例外：**空值也要进这一段**。
     * 光标和选区不是"文字"，空输入框照样得有光标（否则清空内容后就看不见焦点在哪）。
     * 所以判据是"有文字" 或 "这个节点是输入框"。
     */
    struct QLUIInput *in = input_of(node->owner, node);
    /* 空输入框也要进这一段：光标、以及输入法组字文本都在这里画 */
    const int pre_active = (in != NULL && node->owner->preedit_len > 0 &&
                            node->owner->focus == node);
    if (!inv && (has_shown_text(node) || pre_active) &&
        ((node->text != NULL && node->text[0] != '\0') || in != NULL) &&
        !ql_rect_is_empty(node->content_box)) {
        /*
         * 输入框：显示文本可能被换成圆点（密码），而且比盒子宽时要横向滚。
         * 这两件事都只在输入框上发生，普通文本节点走的是同一条 draw_text。
         */
        char dots[QL_CFG_UI_INPUT_LEN * 3 + 4];
        const char *shown = (node->text != NULL) ? node->text : "";
        QLRect tbox = node->content_box;
        QLRect input_saved_clip = clip;
        int input_clipped = 0;
        /*
         * 输入法组字中的文本要**插进**显示串里（而不是叠在光标上另画一遍）：
         * 叠着画会和光标后面的正文重叠，看起来像"字号不对/糊成一团"。
         * 拼进去以后 draw_text 会照常排版、折行，后面的文字自然被推开。
         */
        char composed[QL_CFG_UI_INPUT_LEN * 4 + 8];
        uint16_t pre_b0 = 0, pre_b1 = 0;
        int has_pre = 0;
        if (in != NULL) {
            const int pw = (node->src->flags & QL_UI_NODE_PASSWORD) != 0;
            shown = input_display_text(in, pw, dots, sizeof(dots));
            if (node->owner->preedit_len > 0 && node->owner->focus == node) {
                const uint16_t off = (uint16_t)((pw)
                                                    ? (cp_count(in->buf, in->caret) * 3)
                                                    : in->caret);
                size_t n = 0;
                const size_t head = (off < (uint16_t)strlen(shown)) ? off : strlen(shown);
                if (head + 1u < sizeof(composed))
                    memcpy(composed, shown, head), n = head;
                pre_b0 = (uint16_t)n;
                /* 密码框：组字文本也显示成圆点，别把明文漏出去 */
                for (uint16_t k = 0; k < node->owner->preedit_len && n + 4 < sizeof(composed);
                     k++) {
                    const unsigned char c = (unsigned char)node->owner->preedit[k];
                    if (pw && ((c & 0xC0u) != 0x80u)) {
                        composed[n++] = (char)0xE2;   /* '•' = E2 80 A2 */
                        composed[n++] = (char)0x80;
                        composed[n++] = (char)0xA2;
                    } else if (pw) {
                        continue;                     /* 续字节：已经画过圆点 */
                    } else {
                        composed[n++] = (char)c;
                    }
                }
                pre_b1 = (uint16_t)n;
                const char *rest = shown + head;
                while (*rest != '\0' && n + 1 < sizeof(composed))
                    composed[n++] = *rest++;
                composed[n] = '\0';
                shown = composed;
                has_pre = 1;
            }
            tbox.x -= in->scroll_x;
            tbox.y -= in->scroll_y;      /* 多行：纵向滚动（单行恒 0） */
            /*
             * **必须裁到内容盒**：多行输入框的内容通常比盒子高（那正是要滚的原因），
             * 不裁的话文字会画到框外去（截图里那种"文字溢出输入框"）。
             * 以前这里是 tbox.h += scroll_y 让文字不被高度截断 —— 方向反了：
             * 该裁的是绘制，不是把盒子撑高。
             */
            if (shown[0] != '\0' && d2d->set_clip != NULL &&
                !ql_rect_is_empty(node->content_box)) {
                const QLRect c = ql_rect_intersect(clip, node->content_box);
                node->owner->clip = c;
                d2d->set_clip(d2d, c);
                input_clipped = 1;
            }
        }

        QLTextStyle ts;
        memset(&ts, 0, sizeof(ts));
        ts.color = s->text_color;
        ts.line_h = s->line_h;
        ts.align = s->text_align;
        ts.wrap = (uint8_t)QL_UI_WS_WRAPS(s->white_space);
        ts.baseline_dy = (ql_coord_t)s->baseline_dy;
        ts.bold = (uint8_t)((s->font_flags & QL_UI_FONT_BOLD) != 0);
        ts.italic = (uint8_t)((s->font_flags & QL_UI_FONT_ITALIC) != 0);
        /* B8：段落方向 + 连字/字距开关（整形用） */
        ts.rtl = (uint8_t)(s->direction != 0);
        ts.deco = (uint8_t)(((s->font_flags & QL_UI_FONT_UNDERLINE) ? QL_TEXT_DECO_UNDERLINE
                                                                   : 0u) |
                            ((s->font_flags & QL_UI_FONT_LINE_THROUGH)
                                 ? QL_TEXT_DECO_LINE_THROUGH
                                 : 0u) |
                            ((s->deco_style & QL_UI_DECO_OVERLINE) ? QL_TEXT_DECO_OVERLINE
                                                                  : 0u));
        /* 输入框的行尾空格要占宽（和 input_visual_lines 保持一致） */
        ts.keep_trailing = (uint8_t)(in != NULL);
        ts.ligatures = (uint8_t)(s->ligatures ? 1 : 0);
        ts.small_caps =
            (uint8_t)((s->variant_flags & QL_UI_VARIANT_SMALL_CAPS) ? 1 : 0);
        ts.tabular_nums =
            (uint8_t)((s->variant_flags & QL_UI_VARIANT_TABULAR_NUMS) ? 1 : 0);
        ts.kerning = (uint8_t)(s->kerning ? 1 : 0);
        /*
         * 文本细节：输入框/多行输入里**不套**字间距与缩进 —— 光标位置是按
         * `ql_font_line_width` 算的（见 input_visual_lines），多一个字距就会
         * 和字形错位。普通文本才吃这三项 + `text-overflow: ellipsis`。
         */
        /*
         * `::placeholder`（B2）：输入框为空时用占位样式画那句提示。
         * 样式来自 `style_placeholder`（构建期把 `input::placeholder` 的声明
         * 合并进宿主样式算出来的），所以字号/颜色都能单独写。
         */
        if (in != NULL && in->len == 0 && !has_pre && node->src != NULL &&
            node->src->placeholder != 0xFFFF && node->owner != NULL &&
            node->owner->doc != NULL && node->owner->doc->text_pool != NULL &&
            node->owner->doc->text_offsets != NULL) {
            const QLUIDoc *phdoc = node->owner->doc;
            const char *phtext =
                phdoc->text_pool + phdoc->text_offsets[node->src->placeholder];
            const QLUIStyle *phstyle = s;
            if (node->src->style_placeholder != 0xFFFF &&
                node->src->style_placeholder < phdoc->style_count)
                phstyle = &phdoc->styles[node->src->style_placeholder];
            QLTextStyle pts = ts;
            pts.color = phstyle->text_color;
            pts.deco = (uint8_t)(((phstyle->font_flags & QL_UI_FONT_UNDERLINE)
                                      ? QL_TEXT_DECO_UNDERLINE
                                      : 0u) |
                                 ((phstyle->font_flags & QL_UI_FONT_LINE_THROUGH)
                                      ? QL_TEXT_DECO_LINE_THROUGH
                                      : 0u));
            pts.bold = (uint8_t)((phstyle->font_flags & QL_UI_FONT_BOLD) != 0);
            pts.italic = (uint8_t)((phstyle->font_flags & QL_UI_FONT_ITALIC) != 0);
            const QLFont *phfont = font_of_style(node, phstyle);
            if (phtext[0] != '\0' && phfont != NULL)
                d2d->draw_text(d2d, phfont, phtext, -1, tbox, &pts);
        }

        if (in == NULL) {
            ts.letter_spacing = (ql_coord_t)s->letter_spacing;
            ts.word_spacing = (ql_coord_t)s->word_spacing;
            ts.tab_size = s->tab_size;
            ts.deco_style = (uint8_t)(s->deco_style & QL_UI_DECO_LINE_MASK);
            ts.deco_thickness = s->deco_thickness;
            ts.deco_offset = s->deco_offset;
            ts.deco_alpha = s->deco_alpha;
            ts.deco_color = s->deco_color;
            ts.indent = (ql_coord_t)s->text_indent;
            ts.break_any = (uint8_t)(s->word_break == QL_UI_WORDBREAK_ALL);
            ts.ellipsis = (uint8_t)(s->text_overflow == QL_UI_OVERFLOW_ELLIPSIS);
        }
        /*
         * `text-shadow`：先把文字画一遍"影子"（偏移 + 影子色 + alpha），
         * 再画正文盖上去。blur > 0 时用 5 次采样近似（中心 + 四斜向），
         * 每次 alpha 摊薄 —— 不是真高斯，但对 1~4px 的模糊肉眼看不出差别，
         * 而且完全不需要离屏缓冲（MCU 上也能跑）。
         */
        if (shown[0] != '\0') {   /* 空输入框里也可能有组字文本要画 */
            const QLUIShadow *tsh = &s->text_shadow;
            if (tsh->alpha > 0) {
                QLTextStyle shadow_ts = ts;
                shadow_ts.color = tsh->color;
                QLRect sbox = tbox;
                const ql_coord_t blur = (ql_coord_t)tsh->blur;
                if (blur == 0) {
                    sbox.x += tsh->dx;
                    sbox.y += tsh->dy;
                    d2d->alpha = tsh->alpha;
                    d2d->draw_text(d2d, font_of(node), shown, -1, sbox, &shadow_ts);
                    d2d->alpha = 255;
                } else {
                    /* 五采样近似：中心和四个斜向各 blur/2 */
                    const ql_coord_t d = (blur > 1) ? blur / 2 : 1;
                    static const ql_coord_t kdx[5] = { 0, -1, 1, -1, 1 };
                    static const ql_coord_t kdy[5] = { 0, -1, -1, 1, 1 };
                    for (int i = 0; i < 5; i++) {
                        sbox = tbox;
                        sbox.x += tsh->dx + kdx[i] * d;
                        sbox.y += tsh->dy + kdy[i] * d;
                        d2d->alpha = (uint8_t)(tsh->alpha / 5u + 1u);
                        d2d->draw_text(d2d, font_of(node), shown, -1, sbox,
                                       &shadow_ts);
                    }
                    d2d->alpha = 255;
                }
            }
            d2d->draw_text(d2d, font_of(node), shown, -1, tbox, &ts);

            /*
             * 组字文本加下划线（"还没上屏"的标志）。多行折行时不画：
             * 跨行的下划线要按视觉行拆开，收益不大。
             */
            if (has_pre && !in->multiline && d2d->draw_line != NULL) {
                const QLFont *tf = font_of(node);
                const ql_coord_t x0 = tbox.x + ql_font_line_width(tf, shown, (int32_t)pre_b0);
                const ql_coord_t x1 = tbox.x + ql_font_line_width(tf, shown, (int32_t)pre_b1);
                const ql_coord_t uy = ql_rect_bottom(node->content_box) - 3;
                if (x1 > x0)
                    d2d->draw_line(d2d, x0, uy, x1, uy, 1,
                                   (s->text_color != 0) ? s->text_color : 0x202020);
            }
        }
        if (input_clipped) {
            node->owner->clip = input_saved_clip;
            d2d->set_clip(d2d, input_saved_clip);
        }
        draw_input_decor(node, d2d);
    }

    /*
     * 有轴不是 visible 时，子树被裁到本节点的边框盒（含圆角）。
     * 分轴：只裁需要裁的那个方向 —— 另一轴沿用外层裁剪，
     * 所以"只裁横向"的容器里，子元素仍可以纵向画出去。
     */
    if ((s->overflow_x != QL_UI_OVERFLOW_VISIBLE ||
         s->overflow_y != QL_UI_OVERFLOW_VISIBLE) &&
        d2d->set_clip != NULL) {
        QLRect saved = clip;
        QLRect clipped = saved;
        if (s->overflow_x != QL_UI_OVERFLOW_VISIBLE) {
            clipped.x = box.x;
            clipped.w = box.w;
        }
        if (s->overflow_y != QL_UI_OVERFLOW_VISIBLE) {
            clipped.y = box.y;
            clipped.h = box.h;
        }
        clipped = ql_rect_intersect(clipped, saved);
        /* 裁剪同时记进 ui->clip：子节点的剪枝判断必须用真实裁剪，
           否则会按外层更大的裁剪放行，白跑一遍 */
        node->owner->clip = clipped;
        d2d->set_clip(d2d, clipped);
        render_children(node, d2d);
        draw_scrollbar(node, d2d);
        d2d->set_clip(d2d, saved);
        node->owner->clip = saved;
        ql_draw2d_set_offset(d2d, saved_ox, saved_oy);
        if (d2d->set_mask != NULL)
            ql_draw2d_set_mask_ellipse(d2d, saved_mx, saved_my, saved_mr, saved_mry);
        if (saved_efx != d2d->elem_x_fx || saved_efy != d2d->elem_y_fx)
            ql_draw2d_set_elem_scale_xy(d2d, saved_efx, saved_efy, 0, 0);
        if (inset_clipped) {
            node->owner->clip = clip_before_inset;
            if (d2d->set_clip != NULL)
                d2d->set_clip(d2d, clip_before_inset);
        }
        return;
    }

    render_children(node, d2d);
    ql_draw2d_set_offset(d2d, saved_ox, saved_oy);
    if (d2d->set_mask != NULL)
        ql_draw2d_set_mask_ellipse(d2d, saved_mx, saved_my, saved_mr, saved_mry);
    if (saved_efx != d2d->elem_x_fx || saved_efy != d2d->elem_y_fx)
        ql_draw2d_set_elem_scale_xy(d2d, saved_efx, saved_efy, 0, 0);
    if (inset_clipped) {
        node->owner->clip = clip_before_inset;
        if (d2d->set_clip != NULL)
            d2d->set_clip(d2d, clip_before_inset);
    }
}

void ql_ui_render(QLUIRoot *ui, QLDraw2D *d2d)
{
    if (ui == NULL || d2d == NULL || ui->node_count == 0)
        return;

    /* 记录进入时的裁剪，overflow 容器靠它保存/恢复 */
    ui->clip = ql_rect_make(0, 0, ui->width, ui->height);
    if (ui->root_clip_valid)
        ui->clip = ui->root_clip;
    render_node(&ui->nodes[0], d2d);
    draw_select_panel(ui, d2d);
    draw_dialog_layer(ui, d2d);
}

void ql_ui_render_rect(QLUIRoot *ui, QLDraw2D *d2d, QLRect dirty)
{
    if (ui == NULL || d2d == NULL || ui->node_count == 0 || d2d->set_clip == NULL)
        return;

    QLRect want = ql_rect_intersect(dirty, ql_rect_make(0, 0, ui->width, ui->height));
    if (ui->root_clip_valid)
        want = ql_rect_intersect(want, ui->root_clip);
    if (ql_rect_is_empty(want))
        return;

    d2d->set_clip(d2d, want);
    ui->clip = want;
    render_node(&ui->nodes[0], d2d);
    d2d->set_clip(d2d, want);
    draw_select_panel(ui, d2d);
    draw_dialog_layer(ui, d2d);
}

QLRect ql_ui_paint_bounds(const QLUIRoot *ui)
{
    if (ui == NULL || ui->node_count == 0)
        return ql_rect_make(0, 0, 0, 0);
    if (dialog_top(ui) != NULL)   /* 遮罩盖满视口，脏区不能只按树算 */
        return ql_rect_union(ui->nodes[0].paint_box,
                             ql_rect_make(0, 0, ui->width, ui->height));
    return ui->nodes[0].paint_box;
}

void ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas)
{
    if (ui == NULL || canvas == NULL)
        return;

    /*
     * 画布已有 clip 时以它为准，避免 overflow 恢复时把外部裁剪冲掉。
     * 画布的 clip 是**设备像素**，而整棵 ui 树的坐标是 CSS px，所以要换回 CSS px
     * （近边向下、远边向上取整，保证覆盖全部设备像素）。
     */
    ql_coord_t scale_fx = (canvas->scale_fx > 0) ? canvas->scale_fx : QL_SCALE_ONE;
    ui->root_clip = ql_unscale_rect(canvas->clip, scale_fx);
    ui->root_clip_valid = 1;

    QLDraw2D d2d;
    if (ql_draw2d_canvas_init(&d2d, canvas) != QL_OK)
        return;
    ql_ui_render(ui, &d2d);
}

/* ------------------------------ 查询 / 交互 ------------------------------ */

QLUINodeRT *ql_ui_find_by_tag(QLUIRoot *ui, QLUITag tag, uint16_t nth)
{
    if (ui == NULL)
        return NULL;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (ui->nodes[i].src->tag != (uint16_t)tag)
            continue;
        if (nth == 0)
            return &ui->nodes[i];
        nth--;
    }
    return NULL;
}

static ql_coord_t resolve_origin_len(QLUILength len, ql_coord_t basis);

/* 递归命中：子节点优先（子节点画在父之上），后出现的子节点优先（z 序已排好） */
static QLUINodeRT *hit_walk_ui(QLUINodeRT *node, ql_coord_t x, ql_coord_t y,
                               QLRect clip, ql_coord_t ox, ql_coord_t oy,
                               ql_coord_t sfx_fx, ql_coord_t sfy_fx,
                               ql_coord_t sfx_cx, ql_coord_t sfx_cy)
{
    if (node == NULL)
        return NULL;

    /* 用**生效**样式：动画/过渡中的 transform 也要跟着变，否则点不中移动后的元素 */
    const QLUIStyle *s = ql_ui_node_style(node);
    if (s == NULL || s->display == QL_UI_DISPLAY_NONE)
        return NULL;

    /*
     * 把屏幕坐标映射回节点的布局空间：先撤销祖先累积的缩放，再撤销平移。
     * 绘制时是"先缩放（绕元素中心）再平移"，所以逆变换顺序相反。
     */
    if (sfx_fx == 0 || sfy_fx == 0)
        return NULL;    /* 被压成零面积，点不中 */
    if (sfx_fx != QL_SCALE_ONE)
        x = sfx_cx + (ql_coord_t)(((int64_t)(x - sfx_cx) * QL_SCALE_ONE) / sfx_fx);
    if (sfy_fx != QL_SCALE_ONE)
        y = sfx_cy + (ql_coord_t)(((int64_t)(y - sfx_cy) * QL_SCALE_ONE) / sfy_fx);

    /*
     * (ox, oy) 是祖先累积的平移，本节点自己还要再加一层自己的 transform——
     * 绘制时正是这么画的（render_node 先设偏移再画自己）。
     */
    /*
     * 旋转/斜切：把点**逆变换**回节点的布局空间，命中就在未旋转的盒子判。
     * 绘制时旋转作用在合成结果上，所以这是最后一步逆变换；子节点接着走
     * 平移/缩放那一套（每个节点的逆变换都在它自己的局部空间里做，逐层下降
     * 正好是仿射矩阵的连乘）。
     */
    if (s->xform >= 0 && node->owner != NULL && node->owner->doc != NULL &&
        node->owner->doc->xforms != NULL &&
        s->xform < (int16_t)node->owner->doc->xform_count) {
        const QLUIXform *q = &node->owner->doc->xforms[s->xform];
        const QLRect bb = node->border_box;
        const ql_coord_t px = bb.x + ox + resolve_origin_len(s->origin[0], bb.w);
        const ql_coord_t py = bb.y + oy + resolve_origin_len(s->origin[1], bb.h);
        const int64_t a = q->a, b = q->b, c = q->c, d = q->d;
        const int64_t det = a * d - b * c;
        if (det == 0)
            return NULL;                     /* 退化矩阵：压成一条线，点不中 */
        const int64_t ux = ((int64_t)x - px) * QL_FX_ONE;
        const int64_t uy = ((int64_t)y - py) * QL_FX_ONE;
        /* lx/ly 出来的单位已经是 CSS px（ux 先乘了 FX，det 是 FX²） */
        const int64_t lx = (d * ux - c * uy) / det;
        const int64_t ly = (a * uy - b * ux) / det;
        x = px + (ql_coord_t)lx;
        y = py + (ql_coord_t)ly;
    }

    QLRect box = node->border_box;
    box.x += ox + s->tf_x;
    box.y += oy + s->tf_y;
    if (ql_rect_is_empty(box))
        return NULL;

    /*
     * 自己的 scale 是绕中心的：把点逆缩放回布局空间再和 box 比。
     * 放大时点能落在 box 外（仍然命中），压扁时 box 边缘的点会被映射出去（不命中）。
     * 压到 0（假翻转的中间帧）面积为零，任何点都点不中。
     */
    ql_coord_t hx = x, hy = y;
    if (s->sc_x != 1000 || s->sc_y != 1000) {
        /* 0 = 面积为零（假翻转的中间帧），任何点都点不中 */
        if (s->sc_x == 0 || s->sc_y == 0)
            return NULL;
        ql_coord_t cx0 = box.x + box.w / 2;
        ql_coord_t cy0 = box.y + box.h / 2;
        hx = cx0 + (ql_coord_t)(((int64_t)(x - cx0) * 1000) / s->sc_x);
        hy = cy0 + (ql_coord_t)(((int64_t)(y - cy0) * 1000) / s->sc_y);
    }

    /* 视觉范围：放大时比布局盒大，overflow 的裁剪区得跟着它走（绘制侧也是按缩放后的矩形裁） */
    QLRect vis = box;
    /* 用绝对值：负缩放（镜像）的可见范围同样可能超出布局盒 */
    const int absx = (s->sc_x < 0) ? -(int)s->sc_x : (int)s->sc_x;
    const int absy = (s->sc_y < 0) ? -(int)s->sc_y : (int)s->sc_y;
    if (absx > 1000 || absy > 1000) {
        ql_coord_t cx0 = box.x + box.w / 2;
        ql_coord_t cy0 = box.y + box.h / 2;
        ql_coord_t hw = (absx > 1000)
                            ? (ql_coord_t)((int64_t)box.w * absx / 2000)
                            : box.w / 2;
        ql_coord_t hh = (absy > 1000)
                            ? (ql_coord_t)((int64_t)box.h * absy / 2000)
                            : box.h / 2;
        vis = ql_rect_make(cx0 - hw, cy0 - hh, hw * 2, hh * 2);
    }

    /* overflow 容器把子树裁到自己的边框盒（分轴：只裁需要的那个方向） */
    QLRect child_clip = clip;
    if (s->overflow_x != QL_UI_OVERFLOW_VISIBLE ||
        s->overflow_y != QL_UI_OVERFLOW_VISIBLE) {
        QLRect c = child_clip;
        if (s->overflow_x != QL_UI_OVERFLOW_VISIBLE) {
            c.x = vis.x;
            c.w = vis.w;
        }
        if (s->overflow_y != QL_UI_OVERFLOW_VISIBLE) {
            c.y = vis.y;
            c.h = vis.h;
        }
        child_clip = ql_rect_intersect(child_clip, c);
    }
    if (ql_rect_is_empty(child_clip))
        return NULL;

    /* 正向遍历 + 覆盖：链表顺序即绘制顺序，最后一个命中的就是最上层的 */
    /* 自己的 scale 传给子树（绕自己的中心） */
    ql_coord_t nfx = sfx_fx, nfy = sfy_fx, ncx = sfx_cx, ncy = sfx_cy;
    if ((s->sc_x != 1000 || s->sc_y != 1000) && sfx_fx == QL_SCALE_ONE) {
        nfx = (ql_coord_t)((int64_t)s->sc_x * QL_SCALE_ONE / 1000);
        nfy = (ql_coord_t)((int64_t)s->sc_y * QL_SCALE_ONE / 1000);
        ncx = box.x + box.w / 2;      /* box 已经加了平移，与绘制一致 */
        ncy = box.y + box.h / 2;
    }

    /*
     * 子节点按"普通流 → sticky"两趟遍历，与绘制顺序保持一致
     * （sticky 画在最上面，所以也最后命中）。
     */
    QLUINodeRT *hit = NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            const int is_sticky = (c->style != NULL &&
                                   c->style->position == QL_UI_POSITION_STICKY);
            if (is_sticky != (pass == 1))
                continue;
            QLUINodeRT *found = hit_walk_ui(c, x, y, child_clip, ox + s->tf_x,
                                            oy + s->tf_y, nfx, nfy, ncx, ncy);
            if (found != NULL)
                hit = found;
        }
    }
    if (hit != NULL)
        return hit;

    /* 自己：必须在裁剪区内（被 overflow 裁掉的部分不可命中） */
    if (x < clip.x || x >= ql_rect_right(clip) || y < clip.y || y >= ql_rect_bottom(clip))
        return NULL;
    /*
     * `pointer-events: none`：自己不作为命中目标（子节点仍然算过 —— 它们可能
     * 自己写了 `auto`）。`visibility: hidden` 同样点不中（看不见的东西不该能点），
     * 但它的子节点已经按各自的样式走过了上面的循环。
     */
    if (s->pointer_events == QL_UI_POINTER_NONE || s->visibility != QL_UI_VISIBLE)
        return NULL;
    if (hx >= box.x && hx < ql_rect_right(box) && hy >= box.y &&
        hy < ql_rect_bottom(box))
        return node;
    return NULL;
}

QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL || ui->node_count == 0)
        return NULL;
    QLRect screen = ql_rect_make(0, 0, ui->width, ui->height);
    return hit_walk_ui(&ui->nodes[0], x, y, screen, 0, 0, QL_SCALE_ONE,
                       QL_SCALE_ONE, 0, 0);
}

/*
 * 当前位置应该用哪个指针形状：命中节点开始往上找第一个写了 `cursor` 的节点。
 *
 * **只算意图，不设光标** —— 形状由宿主/后端交给窗口系统
 * （X11 的 `XDefineCursor`、Wayland 的 `wl_cursor`）。这样 MCU 上也统一：
 * 触摸屏忽略返回值即可。默认：输入框/多行输入 → text，其余 → default。
 */
uint8_t ql_ui_cursor_at(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    QLUINodeRT *hit = ql_ui_hit_test(ui, x, y);
    for (QLUINodeRT *n = hit; n != NULL; n = n->parent) {
        const QLUIStyle *s = ql_ui_node_style(n);
        if (s != NULL && s->cursor != QL_UI_CURSOR_AUTO)
            return s->cursor;
    }
    if (hit == NULL)
        return QL_UI_CURSOR_DEFAULT;
    /* 没写 cursor 时的合理默认：文本域给 I 形，别的给箭头 */
    if (hit->src != NULL && (hit->src->tag == QL_UI_TAG_INPUT ||
                             hit->src->tag == QL_UI_TAG_TEXTAREA))
        return QL_UI_CURSOR_TEXT;
    return QL_UI_CURSOR_DEFAULT;
}

void ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data)
{
    if (ui == NULL)
        return;
    ui->click_cb = cb;
    ui->click_user_data = user_data;
}

/*
 * `opacity`：把整体透明度乘下去再画。
 *
 * 做法是"顺着子树往下乘"而不是"先渲染再整体合成" —— 前者不需要离屏图层，
 * 背景/边框/阴影/文字/图片会自动一起淡出。差别只在同一子树内部互相覆盖时：
 * CSS 是先合成一张图再降透明，这里逐图元降透明，重叠处会略微透出来。
 * 严格一致要离屏图层（见《CSS3 覆盖与裁剪》§20.1），这里选择"便宜且可预期"。
 */
/* ------------------------------ 离屏图层（M9.3） ------------------------------ */

/* `transform-origin` 的百分比是**千分比**（0..1000），与其它百分比的 /100 不同 */
static ql_coord_t resolve_origin_len(QLUILength len, ql_coord_t basis)
{
    if (len.unit == QL_UI_UNIT_PERCENT)
        return (ql_coord_t)(((int32_t)basis * (int32_t)len.value) / 1000);
    return resolve_length(len, basis, 0);
}


/* clip-path 的长度 → px（百分比按边长，px 原样） */
static ql_coord_t clip_len_to_px(QLUILength len, ql_coord_t span)
{
    if (len.unit == QL_UI_UNIT_PERCENT)
        return (ql_coord_t)(((int32_t)span * (int32_t)len.value) / 100);
    return (ql_coord_t)len.value;
}

/* 多边形顶点是否可用（点池 + 索引都在） */
static int ui_has_clip_pts(const QLUIRoot *ui, const QLUIStyle *s)
{
    return ui != NULL && ui->doc != NULL && ui->doc->clip_pts != NULL &&
           s->clip_pts != QL_UI_NO_CLIP_PTS && s->clip_pt_n >= 3;
}

/* 这一层必须走离屏合成吗？（filter / 混合模式 / 遮罩 / 真 group opacity） */
#if QL_FEAT_CSS_FILTER_LAYER
/* ---------------- 颜色矩阵：把 `filter` 的纯颜色函数折成一个 3x4 矩阵 ---------------- */

/*
 * 16.16 定点矩阵相乘：`out = a ∘ b`（**先 b 后 a**）。
 * 3x4 仿射：线性部分乘、偏移项先过 a 的线性部分再加上 a 的偏移。
 */
static void cm_compose(int32_t out[12], const int32_t a[12], const int32_t b[12])
{
    int32_t r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++)
                acc += (int64_t)a[i * 4 + k] * (int64_t)b[k * 4 + j];
            r[i * 4 + j] = (int32_t)(acc >> 16);
        }
        /*
         * 偏移项：`out_off = a_lin * b_off + a_off`。
         * 两者都是"色值 * 65536"，所以线性部分（16.16 无量纲）乘完要 >>16。
         */
        int64_t off = (int64_t)a[i * 4 + 3];
        for (int k = 0; k < 3; k++)
            off += ((int64_t)a[i * 4 + k] * (int64_t)b[k * 4 + 3]) >> 16;
        r[i * 4 + 3] = (int32_t)off;
    }
    for (int i = 0; i < 12; i++)
        out[i] = r[i];
}

/* 一个 filter 函数折成 3x4（16.16）。返回 0 表示"不是纯颜色函数" */
static int cm_of_op(uint8_t kind, int32_t arg, int32_t m[12])
{
    /*
     * 约定：线性部分是**无量纲**的 16.16（1.0 = 65536）；
     * 第 4 列是**色值单位**（0..255）的 16.16（255 = 255 * 65536）。
     * 这样加偏移时 `m[3] >> 16` 就是色值增量，组合矩阵时也不会丢精度。
     */
    memset(m, 0, 12 * sizeof(int32_t));
    for (int i = 0; i < 3; i++)
        m[i * 4 + i] = 65536;
    const double S = 65536.0;
    const double t = (double)arg / 1000.0;      /* 千分比 → 0..1 */

    switch (kind) {
    case QL_UI_FILTER_BRIGHTNESS:
        m[0] = m[5] = m[10] = (int32_t)(t * S + 0.5);
        return 1;
    case QL_UI_FILTER_CONTRAST: {
        /* (c - 0.5) * k + 0.5 ⇒ 色值尺度上是 (c - 127.5)*k + 127.5 */
        m[0] = m[5] = m[10] = (int32_t)(t * S + 0.5);
        const int32_t off = (int32_t)(127.5 * (1.0 - t) * S);
        m[3] = m[7] = m[11] = off;
        return 1;
    }
    case QL_UI_FILTER_SATURATE: {
        const double ir = 0.213, ig = 0.715, ib = 0.072;
        const double c[9] = { ir + (1.0 - ir) * t, ig - ig * t, ib - ib * t,
                              ir - ir * t, ig + (1.0 - ig) * t, ib - ib * t,
                              ir - ir * t, ig - ig * t, ib + (1.0 - ib) * t };
        for (int i = 0; i < 9; i++)
            m[(i / 3) * 4 + (i % 3)] = (int32_t)(c[i] * S + 0.5);
        return 1;
    }
    case QL_UI_FILTER_GRAYSCALE: {
        /* c' = c + (lum - c) * t ⇒ 线性部分 = 单位*(1-t) + lum 行*t */
        const double lum[3] = { 0.213, 0.715, 0.072 };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                m[i * 4 + j] = (int32_t)((((i == j) ? (1.0 - t) : 0.0) + lum[j] * t) * S +
                                         0.5);
        return 1;
    }
    case QL_UI_FILTER_SEPIA: {
        const double s9[9] = { 0.393, 0.769, 0.189, 0.349, 0.686,
                               0.168, 0.272, 0.534, 0.131 };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                m[i * 4 + j] =
                    (int32_t)((((i == j) ? (1.0 - t) : 0.0) + s9[i * 3 + j] * t) * S + 0.5);
        return 1;
    }
    case QL_UI_FILTER_INVERT: {
        /* c' = c*(1-2t) + 255t */
        m[0] = m[5] = m[10] = (int32_t)((1.0 - 2.0 * t) * S);
        m[3] = m[7] = m[11] = (int32_t)(255.0 * t * S);
        return 1;
    }
    case QL_UI_FILTER_HUE_ROTATE: {
        /* 与 qllayer.c 的 color_op_px 用同一份 Filter Effects 系数 */
        const double a = (double)arg * 3.14159265358979323846 / 180.0;
        const double cs = cos(a), sn = sin(a);
        const double mm[9] = {
            0.213 + cs * 0.787 - sn * 0.213, 0.715 - cs * 0.715 - sn * 0.715,
            0.072 - cs * 0.072 + sn * 0.928,
            0.213 - cs * 0.213 + sn * 0.143, 0.715 + cs * 0.285 + sn * 0.140,
            0.072 - cs * 0.072 - sn * 0.283,
            0.213 - cs * 0.213 - sn * 0.787, 0.715 - cs * 0.715 + sn * 0.715,
            0.072 + cs * 0.928 + sn * 0.072,
        };
        for (int i = 0; i < 9; i++)
            m[(i / 3) * 4 + (i % 3)] = (int32_t)(mm[i] * S);
        return 1;
    }
    case QL_UI_FILTER_OPACITY:
        /*
         * `opacity()` 动的是 alpha，不是颜色 —— 由调用方乘进 `d2d->alpha`。
         * 这里当"是颜色类"（矩阵为单位），以便整串 filter 能走单批次。
         */
        return 1;
    default:
        return 0;      /* blur / drop-shadow：必须走图层 */
    }
}

/*
 * 把 `filter` 的一条声明折成一个 (矩阵, alpha 千分比)。
 * 返回 1 表示**全部**是纯颜色函数（可以不走离屏图层），0 表示必须走图层。
 * `alpha_pm` 输出 opacity() 的千分比乘积（1000 = 不变）。
 */
static int filter_build_cm(const QLUIStyle *s, int32_t out[12], int32_t *alpha_pm)
{
    int32_t acc[12];
    memset(acc, 0, sizeof(acc));
    acc[0] = acc[5] = acc[10] = 65536;
    int32_t apm = 1000;
    for (uint8_t i = 0; i < s->filter_n && i < 6u; i++) {
        const uint8_t kind = QL_UI_FILTER_KIND(s->filter_op[i]);
        const int32_t arg = QL_UI_FILTER_ARG(s->filter_op[i]);
        if (kind == QL_UI_FILTER_OPACITY) {
            apm = (int32_t)(((int64_t)apm * arg) / 1000);
            continue;
        }
        int32_t m[12];
        if (!cm_of_op(kind, arg, m))
            return 0;
        cm_compose(acc, m, acc);       /* 先 acc 再 m ⇒ 左乘 m */
    }
    for (int i = 0; i < 12; i++)
        out[i] = acc[i];
    *alpha_pm = apm;
    return 1;
}
#endif /* QL_FEAT_CSS_FILTER_LAYER */

static int style_needs_layer(const QLUIStyle *s, const QLUINodeRT *node)
{
#if !QL_FEAT_CSS_FILTER_LAYER
    (void)s;
    (void)node;
    return 0;
#else
    /*
     * `filter` 只有纯颜色函数（brightness / contrast / saturate / grayscale /
     * sepia / invert / hue-rotate / opacity）时**不进离屏图层**：折成一个 3x4
     * 颜色矩阵挂到 draw2d 上（CPU 逐像素、GPU 用 uniform），省掉一整块
     * RGBA 缓冲 + 两遍绘制。含 blur / drop-shadow 才需要图层。
     *
     * 语义差别：CSS 的 filter 作用于"元素合成后的结果"，逐图元施加时
     * 子树内部互相重叠处略有不同（文档写明；实测常见用法看不出差异）。
     */
    if (s->filter_n > 0) {
        int32_t m[12], apm = 1000;
        if (!filter_build_cm(s, m, &apm))
            return 1;
    }
    if (s->mix_blend != QL_UI_BLEND_NORMAL)
        return 1;
    if (s->mask_image != QL_UI_NO_MASK)
        return 1;
    /*
     * `clip-path` 的三种形状都**不进图层**：circle/ellipse/polygon 是解析遮罩
     * （见 render_node_inner），inset 直接裁矩形。
     */
    /*
     * 旋转/斜切/矩阵：必须先把子树按原样合成，再整体重采样
     * （"transform 作用于元素的渲染结果"是 CSS 的语义，也是唯一能对得上
     * 浏览器观感的做法 —— 逐图元转会把文字和圆角转成马赛克）。
     */
    if (s->xform >= 0)
        return 1;
    /*
     * 真 group opacity：**有子节点**才需要图层。
     * 单节点时"逐图元乘 alpha"和"合成后再降透明"是同一个结果（图元之间不重叠），
     * 走老路径更快；有子节点时重叠处会透出来，必须合成。
     */
    if (s->opacity < 255 && node->first_child != NULL)
        return 1;
    return 0;
#endif
}

/* 把样式翻译成后端要的图层参数；`inherited` 是祖先留下的 alpha（0..255） */
static void layer_fx_of(const QLUIStyle *s, QLUINodeRT *node, uint8_t inherited,
                        QLDraw2DLayerFx *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->nops = (uint8_t)((s->filter_n > 6u) ? 6u : s->filter_n);
    for (uint8_t i = 0; i < fx->nops; i++)
        fx->op[i] = s->filter_op[i];
    for (uint8_t i = 0; i < fx->nops; i++) {
        const uint8_t kind = QL_UI_FILTER_KIND(fx->op[i]);
        const int32_t arg = QL_UI_FILTER_ARG(fx->op[i]);
        if (kind == QL_UI_FILTER_BLUR && arg > (int32_t)fx->blur)
            fx->blur = (uint8_t)(arg > 64 ? 64 : arg);
        if (kind == QL_UI_FILTER_DROP_SHADOW) {
            fx->has_drop = 1;
            fx->ds_dx = s->ds_dx;
            fx->ds_dy = s->ds_dy;
            fx->ds_blur = s->ds_blur;
            fx->ds_a = s->ds_a;
            /* currentColor：没写颜色就用元素的文字色（CSS 语义） */
            fx->ds_color = (s->ds_color != 0) ? s->ds_color : s->text_color;
        }
    }
    uint32_t op = (uint32_t)s->opacity * (uint32_t)inherited;
    fx->opacity = (uint8_t)((op + 127u) / 255u);
    fx->blend = s->mix_blend;
    fx->box = node->border_box;
    {
        ql_coord_t r = s->radius[0];
        const ql_coord_t mx = (node->border_box.w < node->border_box.h)
                                  ? node->border_box.w / 2
                                  : node->border_box.h / 2;
        if (r > mx)
            r = mx;
        fx->corner = (r > 0) ? r : 0;
    }
    /* 非轴对齐变换：原点是"边框盒的百分比或 px"，这里结算成绝对 CSS 坐标 */
    if (s->xform >= 0 && node->owner != NULL && node->owner->doc != NULL &&
        node->owner->doc->xforms != NULL &&
        s->xform < (int16_t)node->owner->doc->xform_count) {
        const QLUIXform *q = &node->owner->doc->xforms[s->xform];
        fx->has_xform = 1;
        fx->mat[0] = q->a;
        fx->mat[1] = q->b;
        fx->mat[2] = q->c;
        fx->mat[3] = q->d;
        fx->mat[4] = q->e;
        fx->mat[5] = q->f;
        const QLRect b = node->border_box;
        fx->origin_x = b.x + resolve_origin_len(s->origin[0], b.w);
        fx->origin_y = b.y + resolve_origin_len(s->origin[1], b.h);
    }

    if (s->mask_image != QL_UI_NO_MASK) {
        const QLUIRoot *ui = node->owner;
        if (ui != NULL && ui->doc != NULL && ui->doc->text_pool != NULL &&
            ui->doc->text_offsets != NULL)
            fx->mask = image_by_name(ui, ui->doc->text_pool +
                                             ui->doc->text_offsets[s->mask_image]);
    }
}

/* 图层的绘制范围（CSS px）：自身盒子 ∪ 子树可能画到的范围，再按滤镜半径外扩 */
static QLRect layer_bounds_of(QLUIRoot *ui, QLUINodeRT *node, const QLUIStyle *s)
{
    QLRect b = ql_rect_union(node->border_box, node->paint_box);
    ql_coord_t pad = 0;
    for (uint8_t i = 0; i < s->filter_n && i < 6u; i++) {
        const uint8_t kind = QL_UI_FILTER_KIND(s->filter_op[i]);
        const int32_t arg = QL_UI_FILTER_ARG(s->filter_op[i]);
        if (kind == QL_UI_FILTER_BLUR && arg * 3 > pad)
            pad = arg * 3;
    }
    if (s->ds_color != 0 || (s->filter_n > 0 && s->ds_blur > 0) ||
        (s->filter_n > 0 && (s->ds_dx != 0 || s->ds_dy != 0))) {
        ql_coord_t e = (ql_coord_t)((s->ds_dx < 0 ? -s->ds_dx : s->ds_dx) +
                                    (s->ds_dy < 0 ? -s->ds_dy : s->ds_dy));
        if ((ql_coord_t)s->ds_blur * 3 > e)
            e = (ql_coord_t)s->ds_blur * 3;
        if (e > pad)
            pad = e;
    }
    if (pad > 0)
        b = ql_rect_make(b.x - pad, b.y - pad, b.w + pad * 2, b.h + pad * 2);

    /*
     * 旋转/斜切：外接矩形要按**变换后**的四角重算，否则转出来的角会被裁掉。
     * 原点与矩阵都要在这里重新结算（layer_fx_of 已经算过一遍，这里只读它）。
     */
    if (s->xform >= 0 && ui != NULL && ui->doc != NULL && ui->doc->xforms != NULL &&
        s->xform < (int16_t)ui->doc->xform_count) {
        const QLUIXform *q = &ui->doc->xforms[s->xform];
        const QLRect bb = node->border_box;
        const ql_coord_t ox = bb.x + resolve_origin_len(s->origin[0], bb.w);
        const ql_coord_t oy = bb.y + resolve_origin_len(s->origin[1], bb.h);
        const double a = (double)q->a / (double)QL_FX_ONE;
        const double bq = (double)q->b / (double)QL_FX_ONE;
        const double c = (double)q->c / (double)QL_FX_ONE;
        const double d = (double)q->d / (double)QL_FX_ONE;
        const double e = (double)q->e / (double)QL_FX_ONE;
        const double f = (double)q->f / (double)QL_FX_ONE;
        const double xs[4] = { (double)bb.x, (double)bb.x + (double)bb.w,
                               (double)bb.x + (double)bb.w, (double)bb.x };
        const double ys[4] = { (double)bb.y, (double)bb.y,
                               (double)bb.y + (double)bb.h,
                               (double)bb.y + (double)bb.h };
        double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        for (int k = 0; k < 4; k++) {
            const double ux = xs[k] - (double)ox;
            const double uy = ys[k] - (double)oy;
            const double tx = (double)ox + a * ux + c * uy + e;
            const double ty = (double)oy + bq * ux + d * uy + f;
            if (tx < minx) minx = tx;
            if (ty < miny) miny = ty;
            if (tx > maxx) maxx = tx;
            if (ty > maxy) maxy = ty;
        }
        const QLRect rb = ql_rect_make((ql_coord_t)minx, (ql_coord_t)miny,
                                       (ql_coord_t)(maxx - minx + 1.0),
                                       (ql_coord_t)(maxy - miny + 1.0));
        b = ql_rect_union(b, rb);
    }
    /* 别为屏幕外的东西分配缓冲 */
    if (ui != NULL && ui->clip.w > 0 && ui->clip.h > 0)
        b = ql_rect_intersect(b, ui->clip);
    return b;
}

/* 带图层的节点：两遍哑光（CPU）/ 一遍 RGBA（GPU）——协议见 qldraw2d.h */
static void render_node_layered(QLUINodeRT *node, QLDraw2D *d2d, const QLUIStyle *s)
{
    const uint8_t inherited = d2d->alpha;
    QLDraw2DLayerFx fx;
    layer_fx_of(s, node, inherited, &fx);

    const QLRect bounds = layer_bounds_of(node->owner, node, s);
    fx.bounds_css = bounds;
    if (ql_rect_is_empty(bounds) || d2d->layer_begin == NULL) {
        render_node_inner(node, d2d);
        return;
    }

    /* 层内按"完全不透明"画，透明度统一在合成阶段乘（否则会左右各乘一次） */
    if (!ql_draw2d_layer_begin(d2d, bounds, &fx)) {
        render_node_inner(node, d2d);    /* 后端做不了：照常画，不留白 */
        return;
    }
    d2d->alpha = 255;
    render_node_inner(node, d2d);
    d2d->alpha = inherited;
    if (ql_draw2d_layer_end(d2d)) {
        /* CPU 后端要第二遍（白底），这样才反解得出 alpha */
        if (ql_draw2d_layer_begin(d2d, bounds, &fx)) {
            d2d->alpha = 255;
            render_node_inner(node, d2d);
            d2d->alpha = inherited;
            (void)ql_draw2d_layer_end(d2d);
        }
    }
}

static void render_node(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);
    if (s->display == QL_UI_DISPLAY_NONE)
        return;

    if (s->opacity == 0)
        return;                      /* 完全透明：整棵子树不用画 */

    if (style_needs_layer(s, node)) {
        render_node_layered(node, d2d, s);
        return;
    }

    const uint8_t saved = d2d->alpha;
    if (s->opacity < 255 && saved > 0) {
        uint32_t a = (uint32_t)saved * (uint32_t)s->opacity;
        a = (a + 127u) / 255u;
        d2d->alpha = (uint8_t)(a > 255u ? 255u : a);
    }

#if QL_FEAT_CSS_FILTER_LAYER
    /*
     * 纯颜色 `filter`：不进图层，把矩阵挂到 draw2d 上随子树下传。
     * 嵌套时**与外层矩阵相乘**（先子后父：父的 filter 作用在子元素的结果上），
     * 所以逐图元也保持了链式语义。
     */
    int32_t saved_cm[12];
    const uint8_t had_cm = d2d->cm_on;
    memcpy(saved_cm, d2d->cm, sizeof(saved_cm));
    int cm_applied = 0;
    if (s->filter_n > 0 && d2d->set_cm != NULL) {
        int32_t m[12], apm = 1000;
        if (filter_build_cm(s, m, &apm)) {
            if (had_cm) {
                int32_t mixed[12];
                cm_compose(mixed, saved_cm, m);
                cm_applied = ql_draw2d_set_cm(d2d, mixed, 1);
            } else {
                cm_applied = ql_draw2d_set_cm(d2d, m, 1);
            }
            if (apm != 1000 && d2d->alpha > 0) {
                uint32_t a = (uint32_t)d2d->alpha * (uint32_t)apm;
                d2d->alpha = (uint8_t)((a + 500u) / 1000u);
            }
        }
    }
#endif

    render_node_inner(node, d2d);

#if QL_FEAT_CSS_FILTER_LAYER
    if (cm_applied)
        (void)ql_draw2d_set_cm(d2d, saved_cm, had_cm);
#endif
    d2d->alpha = saved;
}

ql_err_t ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text)
{
    QL_UNUSED(ui);
    if (node == NULL)
        return QL_ERR_INVAL;
    if (node->first_child != NULL)
        return QL_ERR_INVAL;      /* 有子节点的元素不直接放文本 */
    if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG)
        return QL_ERR_INVAL;      /* <img> 的 text 是资源名，别让 set_text 覆盖它 */
    node->text = text;
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* 交互状态位：和 Web 一样沿祖先链生效（子元素 hover 时父容器也算 hover） */
/* ------------------------- 状态过渡（transition） ------------------------- */

/* 找这个节点正在跑的过渡槽（没有就返回 NULL） */
static QLUITrans *trans_find(QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || ui->trans == NULL || node == NULL)
        return NULL;
    for (int i = 0; i < QL_CFG_UI_TRANS_MAX; i++)
        if (ui->trans[i].active && ui->trans[i].node == node)
            return &ui->trans[i];
    return NULL;
}

/* float → int16，四舍五入后饱和（transform 用） */
static int32_t kf_i16(float v)
{
    if (v <= -32768.0f)
        return -32768;
    if (v >= 32767.0f)
        return 32767;
    return (int32_t)((v < 0.0f) ? (v - 0.5f) : (v + 0.5f));
}

/* float 0..255 → uint8，四舍五入（截断会让渐隐永远差一档） */
static uint8_t kf_u8(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 255.0f)
        return 255;
    return (uint8_t)(v + 0.5f);
}

/* 颜色按步骤线性混合。ql_color_t 是 0xRRGGBB，alpha 单独处理 */
static ql_color_t lerp_color(ql_color_t a, ql_color_t b, float p)
{
    float ar = (float)QL_COLOR_GET_R(a), ag = (float)QL_COLOR_GET_G(a);
    float ab = (float)QL_COLOR_GET_B(a);
    float br = (float)QL_COLOR_GET_R(b), bg = (float)QL_COLOR_GET_G(b);
    float bb = (float)QL_COLOR_GET_B(b);
    return QL_COLOR_RGB(kf_u8(ar + (br - ar) * p), kf_u8(ag + (bg - ag) * p),
                        kf_u8(ab + (bb - ab) * p));
}

static float clamp01(float v)
{
    if (v <= 0.0f)
        return 0.0f;
    if (v >= 1.0f)
        return 1.0f;
    return v;
}

/* 动画每一帧写回插值结果：进度在 v[0]，其余通道按 props 位图自己算 */
static void trans_apply(void *user, const QLAnimValue *v)
{
    QLUITrans *t = (QLUITrans *)user;
    if (t == NULL || t->node == NULL)
        return;

    const float p = clamp01(v->v[0]);

    if ((t->props & QL_UI_TRANS_BG) != 0) {
        t->style.bg_color = lerp_color(t->from.bg_color, t->to.bg_color, p);
        /* 渐变元素的终止色一起动，否则过渡期间渐变会"半新半旧" */
        t->style.bg_color2 = lerp_color(t->from.bg_color2, t->to.bg_color2, p);
    }
    if ((t->props & QL_UI_TRANS_TEXT) != 0)
        t->style.text_color = lerp_color(t->from.text_color, t->to.text_color, p);
    if ((t->props & QL_UI_TRANS_BORDER) != 0) {
        for (int i = 0; i < 4; i++)
            t->style.border_color4[i] =
                lerp_color(t->from.border_color4[i], t->to.border_color4[i], p);
    }
    if ((t->props & QL_UI_TRANS_RADIUS) != 0) {
        for (int i = 0; i < 4; i++) {
            const float r = (float)t->from.radius[i] +
                            ((float)t->to.radius[i] - (float)t->from.radius[i]) * p;
            t->style.radius[i] = (int16_t)kf_i16(r);
        }
    }
    if ((t->props & QL_UI_TRANS_ALPHA) != 0) {
        float a = (float)t->from.bg_alpha +
                  ((float)t->to.bg_alpha - (float)t->from.bg_alpha) * p;
        t->style.bg_alpha = kf_u8(a);
    }
    /* transform 四个分量一起插值：只改 translate 不改 scale 会出现"边缩边移"的错位 */
    if ((t->props & QL_UI_TRANS_XFORM) != 0) {
        t->style.tf_x = (int16_t)kf_i16((float)t->from.tf_x +
                                        ((float)t->to.tf_x - (float)t->from.tf_x) * p);
        t->style.tf_y = (int16_t)kf_i16((float)t->from.tf_y +
                                        ((float)t->to.tf_y - (float)t->from.tf_y) * p);
        t->style.sc_x = (int16_t)kf_i16((float)t->from.sc_x +
                                        ((float)t->to.sc_x - (float)t->from.sc_x) * p);
        t->style.sc_y = (int16_t)kf_i16((float)t->from.sc_y +
                                        ((float)t->to.sc_y - (float)t->from.sc_y) * p);
    }
    if ((t->props & QL_UI_TRANS_CLIP) != 0) {
        float r = (float)t->from.clip_r +
                  ((float)t->to.clip_r - (float)t->from.clip_r) * p;
        t->style.clip_r = (uint16_t)kf_i16(r);
    }

    /* 只把这一块标脏：主循环用 ql_ui_consume_dirty 拿到区域后局部重绘 */
    t->node->flags |= QL_UI_RT_DIRTY;
}

/* 记一条"动画结束"事件（有界队列，tick 末尾统一派发） */
static void anim_event_push(QLUIRoot *ui, QLUINodeRT *node, uint8_t kind)
{
    if (ui == NULL || node == NULL)
        return;
    if (ui->anim_ev_count >= (uint8_t)QL_CFG_UI_ANIM_EVENT_MAX) {
        if (!ui->anim_ev_dropped)
            QL_LOGW("ui: animation-end queue full (%d), events dropped",
                    QL_CFG_UI_ANIM_EVENT_MAX);
        ui->anim_ev_dropped = 1;
        return;
    }
    ui->anim_ev[ui->anim_ev_count].node = node;
    ui->anim_ev[ui->anim_ev_count].kind = kind;
    ui->anim_ev_count++;
}

/* 过渡结束：释放槽位，样式回落到状态变体（此时两者数值已经相同，无跳变） */
static void trans_done(void *user)
{
    QLUITrans *t = (QLUITrans *)user;
    if (t == NULL)
        return;
    if (t->node != NULL) {
        t->style = t->to;      /* 精确落到终点，避免 lerp 的舍入残差 */
        t->node->flags |= QL_UI_RT_DIRTY;
        anim_event_push(t->node->owner, t->node, QL_UI_ANIM_END_TRANSITION);
    }
    t->active = 0;
    t->node = NULL;
}

/*
 * 状态位变化后给节点起/改道过渡。
 *
 * 目标样式 = 变化**之后**该节点生效的状态变体样式。
 * 起点 = 变化**之前**屏幕上显示的样式（`before`），所以"打断时从当前插值重基"，
 * hover 上又移开也不会跳变。
 */
static void trans_retarget(QLUIRoot *ui, QLUINodeRT *node, const QLUIStyle *before)
{
    if (ui->trans == NULL || node == NULL)
        return;

    /* 该节点正在播 @keyframes：状态过渡让位（动画的 style 已经覆盖颜色通道） */
    if (anim_find(ui, node) != NULL)
        return;

    /* 目标 = 状态变体样式（不能走 ql_ui_node_style，那会拿到过渡中的中间值） */
    const QLUIStyle *target = style_for_state(node);

    /* 目标没有过渡：把还在跑的撤掉，样式立刻生效（不触发 animationend） */
    if (target == NULL || target->trans_ms == 0 || target->trans_props == 0) {
        QLUITrans *old = trans_find(ui, node);
        if (old != NULL) {
            ql_anim_cancel(&old->key);
            old->active = 0;
            old->node = NULL;
        }
        return;
    }

    QLUITrans *t = trans_find(ui, node);
    if (t == NULL) {
        for (int i = 0; i < QL_CFG_UI_TRANS_MAX; i++) {
            if (!ui->trans[i].active) {
                t = &ui->trans[i];
                break;
            }
        }
        if (t == NULL) {
            QL_LOGW("ui: transition slots full (%d), switching instantly",
                    QL_CFG_UI_TRANS_MAX);
            return;
        }
        t->node = node;
        t->active = 1;
        /* 起点 = 状态变化前实际显示的样式（不是 target，也不是 node->style） */
        t->from = (before != NULL) ? *before : *node->style;
    } else {
        /* 已经在跑：从当前插值结果接着走（自动重基，不回跳） */
        t->from = t->style;
    }

    t->to = *target;
    t->style = t->from;        /* 本帧先按起点画，下一帧才开始推进 */
    t->props = target->trans_props;

    QLAnimValue to;
    memset(&to, 0, sizeof(to));
    to.v[0] = 1.0f;            /* 唯一的语义：进度 0 → 1 */

    t->key.cur.v[0] = 0.0f;    /* 进度从 0 开始 */
    if (ql_anim_start(&t->key, &to, target->trans_ms, target->trans_ease, 0, trans_apply,
                      trans_done, t) != QL_OK) {
        /* 时长 0 / 池满：直接落到终点，不触发 animationend（没有真的播过） */
        if (t->active) {
            t->style = t->to;
            t->node->flags |= QL_UI_RT_DIRTY;
            t->node = NULL;
            t->active = 0;
        }
    }
}

void ql_ui_set_anim_end_cb(QLUIRoot *ui, ql_ui_anim_end_cb cb, void *user_data)
{
    if (ui == NULL)
        return;
    ui->anim_end_cb = cb;
    ui->anim_end_user = user_data;
}

/* 派发本帧攒下的结束事件。回调里可以安全地再起动画 / 改文本 */
static void anim_event_flush(QLUIRoot *ui)
{
    if (ui == NULL || ui->anim_ev_count == 0)
        return;

    const uint8_t n = ui->anim_ev_count;
    ui->anim_ev_count = 0;

    if (ui->anim_end_cb == NULL)
        return;

    for (uint8_t i = 0; i < n; i++) {
        QLUINodeRT *node = ui->anim_ev[i].node;
        /* 回调可能已经把树改掉（比如 ql_ui_set_text），但节点数组不搬，
           指针仍然有效；只是动画槽可能已经被重新占用，所以先取 kind */
        const QLUIAnimEndKind kind = (QLUIAnimEndKind)ui->anim_ev[i].kind;
        ui->anim_end_cb(ui, node, kind, ui->anim_end_user);
    }
}

/* ------------------------- 关键帧动画（@keyframes） ------------------------- */

/* 关键帧里用到的通道值。全部是 float，插值完再舍入一次 */
typedef struct {
    float r, g, b;      /* bg_color */
    float r2, g2, b2;   /* bg_color2（渐变终止色） */
    float a;            /* bg_alpha */
    float tr, tg, tb;   /* text_color */
    float br, bg, bb;   /* border_color */
    float tx, ty;       /* transform: translate */
    float sc_x, sc_y;   /* transform: scale（千分比） */
    float rad;          /* border_radius */
    float clip;         /* clip-path 千分比 */
} QLUIKfValue;

static const QLUIAnimRun *anim_find(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || ui->runs == NULL || node == NULL)
        return NULL;
    for (int i = 0; i < QL_CFG_UI_RUN_ANIM_MAX; i++) {
        if (ui->runs[i].active && ui->runs[i].node == node)
            return &ui->runs[i];
    }
    return NULL;
}

static QLUIAnimRun *anim_find_mut(QLUIRoot *ui, const QLUINodeRT *node)
{
    return (QLUIAnimRun *)(uintptr_t)anim_find(ui, node);
}

/*
 * 读一帧的值。没写某个属性的帧回落到底样式——这正是 CSS "隐式关键帧"的语义：
 *     @keyframes x { 50% { opacity: 0 } }
 * 等价于 0%/100% 取元素自己的基础值。生成器把这种帧标成 QL_UI_KF_BASE。
 */
static void kf_read(const QLUIAnimRun *run, const QLUIKeyframe *f, QLUIKfValue *out)
{
    const QLUIStyle *base = run->node->style;
    /*
     * QL_UI_KF_BASE = 这一帧不存值，全部取元素自己的基础样式
     * （CSS 的"隐式关键帧"：@keyframes 只写 50% 时首末帧就是这个语义）。
     */
    const int all_base = (f->props & QL_UI_KF_BASE) != 0;
    const int has_bg = !all_base && (f->props & QL_UI_KF_BG) != 0;
    const int has_a = !all_base && (f->props & QL_UI_KF_ALPHA) != 0;
    const int has_text = !all_base && (f->props & QL_UI_KF_TEXT) != 0;
    const int has_border = !all_base && (f->props & QL_UI_KF_BORDER) != 0;

    const ql_color_t bg = has_bg ? f->bg_color : base->bg_color;
    out->r = (float)QL_COLOR_GET_R(bg);
    out->g = (float)QL_COLOR_GET_G(bg);
    out->b = (float)QL_COLOR_GET_B(bg);

    const ql_color_t bg2 = has_bg ? f->bg_color2 : base->bg_color2;
    out->r2 = (float)QL_COLOR_GET_R(bg2);
    out->g2 = (float)QL_COLOR_GET_G(bg2);
    out->b2 = (float)QL_COLOR_GET_B(bg2);

    const ql_color_t tx = has_text ? f->text_color : base->text_color;
    out->tr = (float)QL_COLOR_GET_R(tx);
    out->tg = (float)QL_COLOR_GET_G(tx);
    out->tb = (float)QL_COLOR_GET_B(tx);

    /* 关键帧的边框色是单值 → 四边都用它 */
    const ql_color_t bd = has_border ? f->border_color : base->border_color4[0];
    out->br = (float)QL_COLOR_GET_R(bd);
    out->bg = (float)QL_COLOR_GET_G(bd);
    out->bb = (float)QL_COLOR_GET_B(bd);

    /*
     * alpha 跟着"背景色"走：CSS 里 background-color 本来就带 alpha，
     * 所以只动 background-color 的动画也要把 alpha 一起带过来
     * （否则透明元素永远动不出一块底色）。显式写了 opacity 时由它说了算。
     */
    out->a = (float)((has_a || has_bg) ? f->bg_alpha : base->bg_alpha);

    if (!all_base && (f->props & QL_UI_KF_RADIUS) != 0)
        out->rad = (float)f->radius;
    else
        out->rad = (float)ql_ui_radius_max(base);

    if (!all_base && (f->props & QL_UI_KF_CLIP) != 0)
        out->clip = (float)f->clip_r;
    else
        out->clip = (float)base->clip_r;

    if (!all_base && (f->props & QL_UI_KF_MOVE) != 0) {
        out->tx = (float)f->tf_x;
        out->ty = (float)f->tf_y;
        out->sc_x = (float)((f->sc_x == QL_UI_KF_UNSET) ? base->sc_x : f->sc_x);
        out->sc_y = (float)((f->sc_y == QL_UI_KF_UNSET) ? base->sc_y : f->sc_y);
    } else {
        out->tx = (float)base->tf_x;
        out->ty = (float)base->tf_y;
        out->sc_x = (float)base->sc_x;
        out->sc_y = (float)base->sc_y;
    }
}

/*
 * 找到 u（0..1）落在哪一段关键帧，返回段起点下标与段内进度。
 * 帧表按 offset 升序、首帧 0、末帧 1000（生成器保证）。
 */
static uint16_t kf_segment(const QLUIAnimation *def, const QLUIKeyframe *kf,
                           float u, float *out_local)
{
    int32_t pos = (int32_t)(u * 1000.0f + 0.5f);
    if (pos < 0)
        pos = 0;
    if (pos > 1000)
        pos = 1000;

    uint16_t k = 0;
    while ((uint16_t)(k + 1) < def->frame_count && (int32_t)kf[k + 1].offset <= pos)
        k++;

    if ((uint16_t)(k + 1) >= def->frame_count) {
        *out_local = 1.0f;
        return (uint16_t)(def->frame_count - 1);
    }
    int32_t a = (int32_t)kf[k].offset;
    int32_t b = (int32_t)kf[k + 1].offset;
    *out_local = (b > a) ? (float)(pos - a) / (float)(b - a) : 1.0f;
    return k;
}

/* elapsed → 归一化进度（已含 direction）。返回 0 表示这一轮播放已经结束 */
static int anim_progress(const QLUIAnimation *def, uint32_t elapsed, float *out_u)
{
    if (def->dur_ms == 0)
        return 0;

    uint32_t iter = elapsed / (uint32_t)def->dur_ms;
    uint32_t rem = elapsed % (uint32_t)def->dur_ms;
    if (def->iterations != 0 && iter >= (uint32_t)def->iterations)
        return 0;

    float local = (float)rem / (float)def->dur_ms;
    int flip = 0;
    switch (def->direction) {
    case QL_UI_ANIM_REVERSE:
        flip = 1;
        break;
    case QL_UI_ANIM_ALTERNATE:
        flip = (int)(iter & 1u);
        break;
    case QL_UI_ANIM_ALTERNATE_REVERSE:
        flip = (int)((iter & 1u) ^ 1u);
        break;
    default:
        break;
    }
    *out_u = flip ? (1.0f - local) : local;
    return 1;
}

/* 把 u 处的值写进 run->style。只改绘制期通道，其它字段永远是基础样式的值 */
static void anim_apply(QLUIAnimRun *run, float u)
{
    const QLUIAnimation *def = run->def;
    const QLUIKeyframe *kf = &run->node->owner->doc->keyframes[def->first_frame];

    float local = 0.0f;
    uint16_t k = kf_segment(def, kf, u, &local);
    const QLUIKeyframe *f0 = &kf[k];
    const QLUIKeyframe *f1 = ((uint16_t)(k + 1) < def->frame_count) ? &kf[k + 1] : f0;

    /* 帧上没写 timing-function 时用动画的默认缓动 */
    uint8_t ease = (f0->ease == QL_UI_KF_EASE_INHERIT) ? def->ease : f0->ease;
    float t = ql_ease_apply(ease, 0, local);

    QLUIKfValue a, b;
    kf_read(run, f0, &a);
    kf_read(run, f1, &b);
    if (f0 == f1)
        b = a;

    run->style.bg_color = QL_COLOR_RGB(kf_u8(a.r + (b.r - a.r) * t),
                                       kf_u8(a.g + (b.g - a.g) * t),
                                       kf_u8(a.b + (b.b - a.b) * t));
    run->style.bg_color2 = QL_COLOR_RGB(kf_u8(a.r2 + (b.r2 - a.r2) * t),
                                        kf_u8(a.g2 + (b.g2 - a.g2) * t),
                                        kf_u8(a.b2 + (b.b2 - a.b2) * t));
    run->style.bg_alpha = kf_u8(a.a + (b.a - a.a) * t);

    const uint8_t props = (uint8_t)(f0->props & (uint8_t)~QL_UI_KF_BASE);
    if ((props & QL_UI_KF_TEXT) != 0)
        run->style.text_color = QL_COLOR_RGB(kf_u8(a.tr + (b.tr - a.tr) * t),
                                             kf_u8(a.tg + (b.tg - a.tg) * t),
                                             kf_u8(a.tb + (b.tb - a.tb) * t));
    if ((props & QL_UI_KF_BORDER) != 0) {
        /* 关键帧只有一个边框色 → 写满四边 */
        const ql_color_t bc = QL_COLOR_RGB(kf_u8(a.br + (b.br - a.br) * t),
                                           kf_u8(a.bg + (b.bg - a.bg) * t),
                                           kf_u8(a.bb + (b.bb - a.bb) * t));
        for (int i = 0; i < 4; i++)
            run->style.border_color4[i] = bc;
    }
    if ((props & QL_UI_KF_RADIUS) != 0) {
        const int16_t rr = (int16_t)kf_i16(a.rad + (b.rad - a.rad) * t);
        for (int i = 0; i < 4; i++)
            run->style.radius[i] = rr;
    }
    if ((props & QL_UI_KF_CLIP) != 0)
        run->style.clip_r = (uint16_t)kf_i16(a.clip + (b.clip - a.clip) * t);
    if ((props & QL_UI_KF_MOVE) != 0) {
        run->style.tf_x = (int16_t)kf_i16(a.tx + (b.tx - a.tx) * t);
        run->style.tf_y = (int16_t)kf_i16(a.ty + (b.ty - a.ty) * t);
        run->style.sc_x = (int16_t)kf_i16(a.sc_x + (b.sc_x - a.sc_x) * t);
        run->style.sc_y = (int16_t)kf_i16(a.sc_y + (b.sc_y - a.sc_y) * t);
    }
}

/* 给节点开一个动画槽。slot 由调用方给；起点从 u=0 开始 */
static void anim_run_begin(QLUIRoot *ui, QLUIAnimRun *run, QLUINodeRT *node,
                           const QLUIAnimation *def)
{
    (void)ui;
    run->node = node;
    run->def = def;
    run->style = (node->style != NULL) ? *node->style : (QLUIStyle){ 0 };
    run->active = 1;
    run->started = 0;
    run->held = 0;
    run->phase = 0;
    anim_apply(run, 0.0f);   /* 先落到 0% 帧，第一次绘制就正确 */
}

/*
 * 起播：把 doc 里所有带 anim 的节点挂上动画槽。
 * 起始时刻留到第一次 ql_ui_anim_tick —— 这样测试可以用假时钟完全控制，
 * 主循环也不需要额外传时间进来。
 */
static void anim_boot(QLUIRoot *ui)
{
    if (ui == NULL || ui->runs == NULL || ui->doc == NULL)
        return;

    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *node = &ui->nodes[i];
        if (node->style == NULL || node->style->anim == 0xFFFF)
            continue;

        QLUIAnimRun *slot = NULL;
        for (int s = 0; s < QL_CFG_UI_RUN_ANIM_MAX; s++) {
            if (!ui->runs[s].active) {
                slot = &ui->runs[s];
                break;
            }
        }
        if (slot == NULL) {
            QL_LOGW("ui: animation slots full (%d), node %u runs without animation",
                    QL_CFG_UI_RUN_ANIM_MAX, (unsigned)i);
            break;
        }
        anim_run_begin(ui, slot, node, &ui->doc->anims[node->style->anim]);
    }
}

ql_err_t ql_ui_anim_restart(QLUIRoot *ui, QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->runs == NULL || ui->doc == NULL)
        return QL_ERR_INVAL;

    /* 优先复用已有槽：重播时样式从基础值重新开始，风格与首次播放一致 */
    QLUIAnimRun *run = anim_find_mut(ui, node);
    const QLUIAnimation *def = NULL;
    if (run != NULL) {
        def = run->def;
    } else {
        if (node->style == NULL || node->style->anim == 0xFFFF)
            return QL_ERR_INVAL;
        for (int s = 0; s < QL_CFG_UI_RUN_ANIM_MAX; s++) {
            if (!ui->runs[s].active) {
                run = &ui->runs[s];
                break;
            }
        }
        if (run == NULL)
            return QL_ERR_NOMEM;
        def = &ui->doc->anims[node->style->anim];
    }

    anim_run_begin(ui, run, node, def);
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

uint32_t ql_ui_anim_running(const QLUIRoot *ui)
{
    if (ui == NULL)
        return 0;

    uint32_t n = 0;
    if (ui->runs != NULL) {
        for (int i = 0; i < QL_CFG_UI_RUN_ANIM_MAX; i++) {
            if (ui->runs[i].active && !ui->runs[i].held)
                n++;
        }
    }
    if (ui->trans != NULL) {
        for (int i = 0; i < QL_CFG_UI_TRANS_MAX; i++) {
            if (ui->trans[i].active)
                n++;
        }
    }
    return n;
}

/* 推进所有关键帧动画，返回仍在动的节点数 */
static uint32_t anim_tick_runs(QLUIRoot *ui, uint32_t now_ms)
{
    if (ui->runs == NULL)
        return 0;

    uint32_t moving = 0;
    for (int i = 0; i < QL_CFG_UI_RUN_ANIM_MAX; i++) {
        QLUIAnimRun *run = &ui->runs[i];
        if (!run->active || run->node == NULL || run->def == NULL || run->held)
            continue;

        if (!run->started) {
            run->start_ms = now_ms;
            run->started = 1;
        }

        uint32_t elapsed = now_ms - run->start_ms;   /* uint32 回绕天然安全 */

        /* animation-delay：还没开始。fill-mode 带 backwards 时先亮出首帧，否则用基础样式 */
        if (elapsed < (uint32_t)run->def->delay_ms) {
            if (run->phase != 1) {
                if (run->def->fill == QL_UI_ANIM_FILL_BACKWARDS ||
                    run->def->fill == QL_UI_ANIM_FILL_BOTH)
                    anim_apply(run, 0.0f);
                else
                    run->style = *run->node->style;
                run->node->flags |= QL_UI_RT_DIRTY;
                run->phase = 1;
            }
            continue;
        }
        if (run->phase == 1) {
            run->phase = 2;
            run->node->flags |= QL_UI_RT_DIRTY;   /* 延迟刚结束：切回关键帧 */
        }
        elapsed -= (uint32_t)run->def->delay_ms;

        float u = 0.0f;
        if (anim_progress(run->def, elapsed, &u)) {
            anim_apply(run, u);
            run->node->flags |= QL_UI_RT_DIRTY;
            moving++;
            continue;
        }

        /* 播完一次（iteration 用尽）：animationend */
        anim_event_push(ui, run->node, QL_UI_ANIM_END_KEYFRAMES);

        /* fill-forwards 停在末帧，否则回落到基础样式 */
        if (run->def->fill == QL_UI_ANIM_FILL_FORWARDS ||
            run->def->fill == QL_UI_ANIM_FILL_BOTH) {
            anim_apply(run, 1.0f);
            run->held = 1;
            run->node->flags |= QL_UI_RT_DIRTY;
        } else {
            run->node->flags |= QL_UI_RT_DIRTY;   /* 让基础样式重画一遍 */
            run->active = 0;
            run->node = NULL;
        }
    }
    return moving;
}

/*
 * 平滑滚动的一帧：把所有还在滑的容器往目标推一步（ease-out 二次曲线，160ms）。
 * 有变化就重排一次 —— 滚动偏移是烘进布局的（与 ql_ui_scroll_by 同一条路径）。
 */
static uint32_t scroll_smooth_tick(QLUIRoot *ui, uint32_t now_ms)
{
    uint32_t changed = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *n = &ui->nodes[i];
        if (n->scroll_goal_ms == 0)
            continue;
        const uint32_t dur = 160u;
        const uint32_t el = now_ms - n->scroll_goal_ms;   /* uint32 回绕天然安全 */
        if (el >= dur) {
            n->scroll_x = n->scroll_goal_x;
            n->scroll_y = n->scroll_goal_y;
            n->scroll_goal_ms = 0;
        } else {
            const int64_t t = (int64_t)el * 1000 / (int64_t)dur;      /* 0..999 */
            const int64_t e = t * (2000 - t) / 1000;                  /* ease-out */
            n->scroll_x = n->scroll_from_x +
                          (ql_coord_t)(((int64_t)(n->scroll_goal_x - n->scroll_from_x) * e) /
                                       1000);
            n->scroll_y = n->scroll_from_y +
                          (ql_coord_t)(((int64_t)(n->scroll_goal_y - n->scroll_from_y) * e) /
                                       1000);
        }
        changed = 1;
    }
    if (changed)
        ql_ui_layout(ui, ui->width, ui->height);
    return changed;
}

uint32_t ql_ui_anim_tick(QLUIRoot *ui, uint32_t now_ms)
{
    if (ui != NULL)
        clipboard_retry_paste(ui);

    if (ui == NULL)
        return 0;

    uint32_t moving = scroll_smooth_tick(ui, now_ms);
    moving += anim_tick_runs(ui, now_ms);
    moving += ql_anim_tick(now_ms);            /* 状态过渡，池在 qlanim 里 */

    /* 事件统一在本帧末尾派发：回调里再起动画 / 改文本都是安全的 */
    anim_event_flush(ui);
    return moving;
}

/*
 * 把与毛玻璃元素相交的脏矩形扩成该元素的整个绘制盒。
 *
 * 为什么必须扩：毛玻璃不是"按脏区重画自己"就算完的 —— 它读的是**帧缓冲里
 * 已经画好的像素**。脏区只盖住玻璃的一角时，那一角会用新的背景重新糊，
 * 其余部分还是上一帧的糊法，交界处出现接缝；更糟的是模糊核在那一角的边缘
 * 取不到外面的邻居像素，颜色会"断"。
 *
 * 代价可控：只有带 backdrop-filter 的元素会让脏区变大，普通界面一条都不多。
 * 返回扩完之后仍在用的矩形数。
 */
static int dirty_expand_for_blur(QLUIRoot *ui, QLRect *out, int n, int max)
{
    int any_blur = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        const QLUIStyle *st = ql_ui_node_style(&ui->nodes[i]);
        if (st->backdrop_blur > 0 || st->filter_n > 0) {
            any_blur = 1;
            break;
        }
    }
    if (!any_blur)
        return n;

    /*
     * 扩张会让原本不相交的矩形变成相交，所以要做一遍"扩张 + 合并"直到稳定。
     * 每一趟至少能合并掉一对，最多 n 趟；实际上 2 趟就够（玻璃通常不嵌套）。
     */
    for (int pass = 0; pass < n; pass++) {
        int changed = 0;
        for (int k = 0; k < n; k++) {
            for (uint16_t i = 0; i < ui->node_count; i++) {
                const QLUINodeRT *node = &ui->nodes[i];
                const QLUIStyle *st = ql_ui_node_style(node);
                if (st->backdrop_blur <= 0 && st->filter_n <= 0)
                    continue;
                /*
                 * 带 `filter` 的元素要按**滤镜外扩**算脏区：模糊/投影会读到
                 * 自己盒子外面一圈，只重绘盒子内部的话边缘会缺一条。
                 * 这里多扩一圈 `blur·3 + |drop-shadow|`，和绘制时的 bounds 同口径。
                 */
                QLRect g = node->paint_box;
                if (st->filter_n > 0) {
                    ql_coord_t pad = 0;
                    for (uint8_t oi = 0; oi < st->filter_n && oi < 6u; oi++) {
                        const uint8_t kind = QL_UI_FILTER_KIND(st->filter_op[oi]);
                        const int32_t arg = QL_UI_FILTER_ARG(st->filter_op[oi]);
                        if (kind == QL_UI_FILTER_BLUR && arg * 3 > pad)
                            pad = arg * 3;
                    }
                    const ql_coord_t e =
                        (ql_coord_t)((st->ds_dx < 0 ? -st->ds_dx : st->ds_dx) +
                                     (st->ds_dy < 0 ? -st->ds_dy : st->ds_dy) +
                                     (ql_coord_t)st->ds_blur * 3);
                    if (e > pad)
                        pad = e;
                    if (pad > 0)
                        g = ql_rect_make(g.x - pad, g.y - pad, g.w + pad * 2,
                                         g.h + pad * 2);
                }
                if (ql_rect_is_empty(g) || !ql_rect_overlaps(out[k], g))
                    continue;
                const QLRect u = ql_rect_union(out[k], g);
                if (u.w != out[k].w || u.h != out[k].h || u.x != out[k].x ||
                    u.y != out[k].y) {
                    out[k] = u;
                    changed = 1;
                }
            }
        }
        /* 合并相交的矩形（扩张之后可能出现） */
        for (int a = 0; a < n; a++) {
            for (int b = a + 1; b < n; b++) {
                if (!ql_rect_overlaps(out[a], out[b]))
                    continue;
                out[a] = ql_rect_union(out[a], out[b]);
                out[b] = out[n - 1];
                n--;
                b--;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    (void)max;
    return n;
}

int ql_ui_consume_dirty(QLUIRoot *ui, QLRect *out, int max)
{
    if (ui == NULL || out == NULL || max <= 0)
        return 0;

    int n = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        QLUINodeRT *node = &ui->nodes[i];
        if ((node->flags & QL_UI_RT_DIRTY) == 0)
            continue;
        node->flags = (uint8_t)(node->flags & ~QL_UI_RT_DIRTY);

        /* 用绘制包围盒而不是边框盒：阴影/未裁剪子树也要重画 */
        QLRect r = node->paint_box;
        if (ql_rect_is_empty(r))
            continue;

        int merged = 0;
        for (int k = 0; k < n; k++) {
            if (ql_rect_overlaps(out[k], r)) {
                out[k] = ql_rect_union(out[k], r);
                merged = 1;
                break;
            }
        }
        if (merged)
            continue;
        if (n < max)
            out[n++] = r;
        else
            out[max - 1] = ql_rect_union(out[max - 1], r);
    }
    return dirty_expand_for_blur(ui, out, n, max);
}

static void set_state_chain(QLUINodeRT *node, uint8_t mask, int on)
{
    for (QLUINodeRT *n = node; n != NULL; n = n->parent) {
        /* 先记下变化前的生效样式：过渡的起点必须是"屏幕上正在显示的那个值" */
        const QLUIStyle *before = ql_ui_node_style(n);
        const uint8_t was = n->flags;
        if (on)
            n->flags = (uint8_t)(n->flags | mask);
        else
            n->flags = (uint8_t)(n->flags & ~mask);
        /*
         * **状态位真的变了就得重画**（脏区模式的动作来源）。
         *
         * 漏掉这一句的后果是"光标留在已经失焦的输入框里"：光标只在
         * `ui->focus == node` 时画，失焦时那个节点不重画 → 旧光标一直留在
         * 保留式画布上，点几次就有好几个光标同时闪。同理 `:hover`/`:active`
         * 的边框也会残留。有 transition 的节点本来由 trans 标脏，这里补的是
         * "没有 transition 的纯状态切换"。
         */
        if (n->flags != was) {
            /*
             * 标脏范围：目标节点**一定**重画（光标/选中态这类东西不在样式里，
             * 样式指针可能一点没变）；祖先只在**自己的生效样式真的变了**时重画
             * —— 子元素 hover 时父容器也算 hover，`.card:hover` 就属于这种。
             * 无条件把整条链标脏会让每次 hover 变化都变成整屏重绘。
             */
            if (n == node || ql_ui_node_style(n) != before)
                n->flags = (uint8_t)(n->flags | QL_UI_RT_DIRTY);
            /* 状态位变了：让有 transition 的节点起/改道渐变 */
            if (n->owner != NULL)
                trans_retarget(n->owner, n, before);
        }
    }
}

/* 滚轮/拖动的热区宽度：比画出来的滑块宽一点，好点中 */
#define SCROLLBAR_GRAB 10

/*
 * 指针 (x,y) 是否落在 node 的滚动条上（右侧那条竖带）。
 * 只看横向是否在带内：竖向整个容器都算，拖动时按比例映射。
 */
static int on_scrollbar(const QLUINodeRT *node, ql_coord_t x, ql_coord_t y)
{
    if (!ql_ui_scrollable(node))
        return 0;
    const QLRect box = ql_ui_node_rect(node);
    if (y < box.y || y >= ql_rect_bottom(box))
        return 0;
    return x >= ql_rect_right(box) - SCROLLBAR_GRAB;
}

int ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return 0;

    /*
     * 先看滚动条：命中测试会把点算成"容器自己"，但滚动条要的是拖动语义，
     * 不能同时把容器标成 :active（那会让整块列表变个色）。
     */
    QLUINodeRT *hit = ql_ui_hit_test(ui, x, y);
    for (QLUINodeRT *n = hit; n != NULL; n = n->parent) {
        if (!on_scrollbar(n, x, y))
            continue;
        ui->scroll_drag = n;
        ui->scroll_drag_y0 = y;
        ui->scroll_drag_scroll0 = n->scroll_y;
        return 1;
    }

    /*
     * 浮层：点外面就关（并且吃掉这次点击 —— 真实浏览器里点遮罩不会穿透）。
     * 点在对话框内部则照常走后面的流程。
     */
    if (ui->dialog_count > 0) {
        QLUINodeRT *top = dialog_top(ui);
        if (top != NULL) {
            int inside = 0;
            for (QLUINodeRT *n = hit; n != NULL; n = n->parent)
                if (n == top) {
                    inside = 1;
                    break;
                }
            if (!inside) {
                (void)ql_ui_dialog_set_open(ui, top, 0);
                return 1;
            }
        }
    }

    /* 下拉框：展开/收起/选中（面板在最上层，优先） */
    if (select_pointer_down(ui, hit, x, y))
        return 1;

    /*
     * 输入框：点进来就聚焦 + 定光标（并进入"拖动选字"状态）。
     * 命中点是输入框自己（它是 void 元素，没有子节点）。
     */
    if (hit != NULL && hit->src != NULL &&
        (hit->src->tag == QL_UI_TAG_INPUT || hit->src->tag == QL_UI_TAG_TEXTAREA)) {
        if (input_pointer_down(ui, hit, x, y))
            return 1;
    }

    /* <details>：点标题行切换展开/收起（吃掉落点击，不标 :active） */
    if (details_pointer_down(ui, hit, x, y))
        return 1;

    /*
     * 复选框（B2）：按下就切换勾选 —— 与浏览器一致（不等抬起）。
     * 切换会改 :checked 变体，所以要标脏重绘。
     */
    if (hit != NULL && hit->src != NULL &&
        (hit->src->flags & QL_UI_NODE_CHECKBOX) != 0) {
        if ((hit->flags & QL_UI_RT_DISABLED) != 0)
            return 1;                    /* 禁用：什么都不做 */
        hit->flags ^= QL_UI_RT_CHECKED;
        hit->flags |= QL_UI_RT_DIRTY;
        if (ui->click_cb != NULL)
            ui->click_cb(ui, hit, ui->click_user_data);
        return 1;
    }

    QLUINodeRT *node = hit;
    if (node == NULL)
        return 0;
    /* 点到别处：输入框失焦（光标随之消失） */
    if (ui->focus != NULL && ui->focus->src != NULL &&
        ui->focus->src->tag == QL_UI_TAG_INPUT)
        ql_ui_set_focus_node(ui, NULL);
    set_state_chain(node, QL_UI_RT_ACTIVE, 1);
    return 1;
}

int ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return 0;

    if (ui->scroll_drag != NULL) {
        ui->scroll_drag = NULL;
        return 1;              /* 拖滚动条不算点击 */
    }
    if (ui->input_drag) {
        ui->input_drag = 0;
        return 1;              /* 选字结束，不算点击 */
    }

    QLUINodeRT *node = ql_ui_hit_test(ui, x, y);
    int was_active = 0;
    /*
     * 必须走 set_state_chain 而不是直接清标志：直接清会绕过 trans_retarget，
     * 正在跑的过渡就会继续往"按下时的样式"走，松手后颜色卡在中间。
     */
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if ((ui->nodes[i].flags & QL_UI_RT_ACTIVE) != 0) {
            was_active = 1;
            set_state_chain(&ui->nodes[i], QL_UI_RT_ACTIVE, 0);
        }
    }
    if (node == NULL || !was_active)
        return 0;

    /*
     * 图像热区优先：点在 <img> 的热区里就派发 area_cb，**不再**派发 click_cb
     * —— 与浏览器一致（点击的 target 是 <area>，不是 <img>）。
     */
    if (ui->area_cb != NULL) {
        const int ai = ql_ui_area_at(ui, x, y);
        if (ai >= 0) {
            ui->area_cb(ui, ai, ui->area_user);
            return 1;
        }
    }

    if (ui->click_cb != NULL)
        ui->click_cb(ui, node, ui->click_user_data);
    return 1;
}

int ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return 0;

    /* 正在拖滚动条：位移按"可拖动行程 / 可滚动范围"换算 */
    if (ui->scroll_drag != NULL) {
        QLUINodeRT *sc = ui->scroll_drag;
        const QLRect box = ql_ui_node_rect(sc);
        ql_coord_t th = 0, ty = 0;
        scrollbar_thumb(box.h, sc->scroll_max_y, sc->scroll_y, &th, &ty);
        const ql_coord_t room = (box.h - 2 * SCROLLBAR_INSET) - th;
        if (room > 0) {
            const ql_coord_t dy = y - ui->scroll_drag_y0;
            const ql_coord_t want =
                ui->scroll_drag_scroll0 + (ql_coord_t)((int64_t)dy *
                                                       sc->scroll_max_y / room);
            if (ql_ui_scroll_to(ui, sc, sc->scroll_x, want) == QL_OK) {
                ql_ui_layout(ui, ui->width, ui->height);
                sc->flags |= QL_UI_RT_DIRTY;
            }
        }
        return 1;
    }

    if (ui->input_drag) {
        input_pointer_move(ui, x, y);
        return 1;
    }

    QLUINodeRT *node = ql_ui_hit_test(ui, x, y);
    if (ui->hover == node)
        return 0;

    /* 必须走 set_state_chain 而不是直接清标志，否则"移出"不触发过渡 */
    QLUINodeRT *old = ui->hover;
    ui->hover = node;
    if (old != NULL)
        set_state_chain(old, QL_UI_RT_HOVER, 0);
    if (node != NULL)
        set_state_chain(node, QL_UI_RT_HOVER, 1);
    return 1;
}

void ql_ui_set_focus_node(QLUIRoot *ui, QLUINodeRT *node)
{
    if (ui == NULL || ui->focus == node)
        return;
    /* 禁用元素拿不到焦点（B2：与浏览器一致） */
    if (node != NULL && (node->flags & QL_UI_RT_DISABLED) != 0)
        return;
    if (ui->focus != NULL)
        set_state_chain(ui->focus, QL_UI_RT_FOCUS, 0);
    ui->focus = node;
    /* 组字跟着焦点走：焦点一换，没上屏的预编辑就作废（否则会画到新控件里） */
    input_preedit_clear(ui);
    if (node != NULL)
        set_state_chain(node, QL_UI_RT_FOCUS, 1);
}

QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui)
{
    return (ui != NULL) ? ui->focus : NULL;
}

/* 码点下标 → 字节偏移（预编辑文本里；越界则夹到末尾） */
static uint16_t cp_index_to_bytes(const char *s, uint16_t len, int cp)
{
    if (s == NULL)
        return 0;
    if (cp < 0)
        return len;
    uint16_t i = 0;
    int k = 0;
    while (i < len && k < cp) {
        const unsigned char c = (unsigned char)s[i];
        i = (uint16_t)(i + ((c < 0x80u) ? 1u : ((c >= 0xF0u) ? 4u
                                                 : ((c >= 0xE0u) ? 3u : 2u))));
        k++;
    }
    return (i <= len) ? i : len;
}

/*
 * 把一段 UTF-8 追加/拷贝进预编辑缓冲；保证不劈开多字节字符、不越界，
 * 并且**丢掉所有不合法的字节**。
 *
 * 最后这条不是洁癖：输入法（实测 ibus）给的 `XIMText.length` 是"字符数"
 * 而不是 Xlib 手册说的"字节数"，按字节切一刀就只剩下汉字的第一字节 ——
 * 那半个 UTF-8 会原样进缓冲，字体找不到字形，屏幕上就是一个方框。
 */
static uint16_t preedit_copy_in(char *dst, size_t cap, const char *src)
{
    size_t n = (src != NULL) ? ql_utf8_valid_len(src, strlen(src)) : 0;
    if (n > cap - 1u)
        n = ql_utf8_valid_len(src, cap - 1u);   /* 截断处也别劈开字符 */
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
    return (uint16_t)n;
}

void ql_ui_input_set_preedit(QLUIRoot *ui, const char *utf8, int caret_cp)
{
    if (ui == NULL)
        return;
    ui->preedit_len = preedit_copy_in(ui->preedit, sizeof(ui->preedit), utf8);
    ui->preedit_caret = cp_index_to_bytes(ui->preedit, ui->preedit_len, caret_cp);
    if (ui->focus != NULL)
        ui->focus->flags |= QL_UI_RT_DIRTY;     /* 组字文本变了：那一块要重画 */
}

/*
 * 增量修改预编辑：把第 `chg_first` 个码点起、`chg_len` 个码点换成 `insert`。
 * 这正是 XIM `XIMPreeditDrawCallbackStruct{caret, chg_first, chg_length, text}`
 * 的语义 —— 输入法发来的是**片段改动**，不是整串。
 *
 * 为什么必须按增量做：把每个片段当成整串，输入框里就只剩最后那段、而且片段
 * 边界可能落在 UTF-8 字符中间（半个汉字 → 字体找不到字形 → 显示成方框）。
 */
void ql_ui_input_preedit_apply(QLUIRoot *ui, int chg_first, int chg_len,
                               const char *insert, int caret_cp)
{
    if (ui == NULL)
        return;
    const uint16_t len = ui->preedit_len;
    if (chg_first < 0)
        chg_first = 0;
    if (chg_len < 0)
        chg_len = 0;
    uint16_t b0 = cp_index_to_bytes(ui->preedit, len, chg_first);
    uint16_t b1 = cp_index_to_bytes(ui->preedit, len, chg_first + chg_len);
    if (b1 < b0)
        b1 = b0;

    char tail[sizeof(ui->preedit)];
    const uint16_t tail_len = (uint16_t)(len - b1);
    if (tail_len > 0)
        memcpy(tail, ui->preedit + b1, tail_len);

    uint16_t n = b0;
    /* 前段原样保留（b0 一定落在字符边界上：上面是码点换算出来的） */
    if (insert != NULL && insert[0] != '\0') {
        const size_t cap = sizeof(ui->preedit) - (size_t)b0;
        char ins[sizeof(ui->preedit)];
        const uint16_t ins_len = preedit_copy_in(ins, sizeof(ins), insert);
        size_t k = ins_len;
        if (k > cap - 1u)
            k = cap - 1u;
        memcpy(ui->preedit + b0, ins, k);
        n = (uint16_t)(b0 + k);
    }
    {   /* 后段接上（装不下就截掉尾巴，宁可少显示也不越界） */
        size_t room = sizeof(ui->preedit) - 1u - (size_t)n;
        uint16_t k = (tail_len < room) ? tail_len : (uint16_t)room;
        /*
         * 只在**真的截断**时才看"被切掉的第一个字节"：`tail[k]` 在 k == tail_len
         * 时是未初始化的（读到栈上垃圾会让循环随机少拷一个字节，测试里表现为
         * "时好时坏"）。整段都装得下就不需要任何调整。
         */
        if (k < tail_len) {
            while (k > 0 && ((unsigned char)tail[k] & 0xC0u) == 0x80u)
                k--;
        }
        if (k > 0)
            memcpy(ui->preedit + n, tail, k);
        n = (uint16_t)(n + k);
    }
    ui->preedit[n] = '\0';
    ui->preedit_len = n;
    ui->preedit_caret = cp_index_to_bytes(ui->preedit, n, caret_cp);
    if (ui->focus != NULL)
        ui->focus->flags |= QL_UI_RT_DIRTY;
}

const char *ql_ui_input_preedit(const QLUIRoot *ui, int *caret_cp)
{
    if (ui == NULL || ui->preedit_len == 0)
        return NULL;
    if (caret_cp != NULL)
        *caret_cp = (int)cp_count(ui->preedit, ui->preedit_caret);
    return ui->preedit;
}

/*
 * 焦点输入框的光标位置（CSS px，视口坐标）。
 *
 * 这里刻意**不复用** draw_input_decor 的绘制路径：那个函数需要 QLDraw2D，
 * 而输入法问位置的时候可能根本没有渲染（比如刚聚焦还没画）。两次算出来的
 * 几何必须一致，所以公式与 draw_input_decor 里画光标那两行保持同步。
 */
int ql_ui_ime_spot(const QLUIRoot *ui, ql_coord_t *out_x, ql_coord_t *out_y,
                   ql_coord_t *out_h)
{
    struct QLUIInput *in = focused_input((QLUIRoot *)ui);
    if (in == NULL || ui == NULL || in->node == NULL || ui->focus == NULL)
        return 0;
    QLUINodeRT *node = in->node;
    if (node->style == NULL)
        return 0;
    const QLFont *font = font_of_style(node, node->style);
    if (font == NULL)
        return 0;

    const QLRect cb = node->content_box;
    char dots[QL_CFG_UI_INPUT_LEN * 3 + 4];
    const int pw = (node->src != NULL) && ((node->src->flags & QL_UI_NODE_PASSWORD) != 0);
    const char *disp = input_display_text(in, pw, dots, sizeof(dots));
    const ql_coord_t base_x = cb.x - in->scroll_x;

    ql_coord_t x = base_x;
    ql_coord_t y = cb.y;
    ql_coord_t h = cb.h;

    if (in->multiline) {
        QLTextLine lines[QL_CFG_TEXT_MAX_LINES];
        int count = input_visual_lines(font, in, cb.w, lines, QL_CFG_TEXT_MAX_LINES);
        if (count > QL_CFG_TEXT_MAX_LINES)
            count = QL_CFG_TEXT_MAX_LINES;
        if (count > 0) {
            const ql_coord_t lh = text_line_h_of(node->style, font);
            ql_coord_t cx = 0, cy = 0;
            caret_px(font, in, lines, count, lh, &cx, &cy);
            x = base_x + cx;
            y = cb.y - in->scroll_y + cy;
            h = lh;
        }
    } else {
        const uint16_t off = (uint16_t)(pw ? (cp_count(in->buf, in->caret) * 3) : in->caret);
        x = base_x + ql_font_line_width(font, disp, (int32_t)off);
    }

    /* 光标滚出内容盒时把 spot 夹在盒子内：否则候选窗会跑到窗口外面去 */
    if (x < cb.x)
        x = cb.x;
    if (x > ql_rect_right(cb) - 1)
        x = ql_rect_right(cb) - 1;
    if (y < cb.y)
        y = cb.y;
    if (y + h > ql_rect_bottom(cb))
        y = ql_rect_bottom(cb) - h;
    if (h <= 0)
        h = cb.h;

    if (out_x != NULL)
        *out_x = x;
    if (out_y != NULL)
        *out_y = y;
    if (out_h != NULL)
        *out_h = h;
    return 1;
}

void ql_ui_set_input_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user)
{
    if (ui == NULL)
        return;
    ui->input_cb = cb;
    ui->input_user = user;
}

const char *ql_ui_input_value(const QLUIRoot *ui, const QLUINodeRT *node)
{
    const struct QLUIInput *in = input_of(ui, node);
    return (in != NULL) ? in->buf : NULL;
}

ql_err_t ql_ui_input_set_value(QLUIRoot *ui, QLUINodeRT *node, const char *value)
{
    struct QLUIInput *in = input_of(ui, node);
    if (in == NULL)
        return QL_ERR_INVAL;

    size_t n = (value != NULL) ? strlen(value) : 0u;
    /* 容量按控件类型取：`<textarea>` 是 512，不是 `<input>` 的 96 */
    if (n > (size_t)input_cap(in) - 1u)
        n = (size_t)input_cap(in) - 1u;
    if (n > 0)
        memcpy(in->buf, value, n);
    in->buf[n] = '\0';
    in->len = (uint16_t)n;
    in->caret = in->len;          /* 光标到末尾，符合"程序改值"的直觉 */
    in->sel = in->caret;
    in->want_x = -1;
    /* 旧内容的滚动偏移对新内容没意义：先归零，再让光标露出来 */
    in->scroll_x = 0;
    in->scroll_y = 0;
    input_scroll_to_caret(in);
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* ------------------------------ 滚动 API ------------------------------ */

int ql_ui_scrollable(const QLUINodeRT *node)
{
    if (node == NULL || node->style == NULL)
        return 0;
    const QLUIStyle *s = node->style;
    const int auto_x = (s->overflow_x == QL_UI_OVERFLOW_AUTO);
    const int auto_y = (s->overflow_y == QL_UI_OVERFLOW_AUTO);
    return (auto_x && node->scroll_max_x > 0) || (auto_y && node->scroll_max_y > 0);
}

ql_err_t ql_ui_scroll_to(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL || node == NULL || node->style == NULL)
        return QL_ERR_INVAL;

    ql_coord_t nx = x;
    ql_coord_t ny = y;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > node->scroll_max_x)
        nx = node->scroll_max_x;
    if (ny > node->scroll_max_y)
        ny = node->scroll_max_y;

    if (nx == node->scroll_x && ny == node->scroll_y) {
        node->scroll_goal_ms = 0;      /* 已经在目标上：取消可能在滑的动画 */
        return QL_OK;
    }

    /*
     * `scroll-behavior: smooth`：只记目标，偏移由 `ql_ui_anim_tick()` 逐帧推进。
     * 这样"跳"和"滑"走同一条状态（scroll_x/y），绘制/命中/脏区都不用分情况。
     */
    if (node->style->scroll_smooth) {
        node->scroll_from_x = node->scroll_x;
        node->scroll_from_y = node->scroll_y;
        node->scroll_goal_x = nx;
        node->scroll_goal_y = ny;
        node->scroll_goal_ms = ql_anim_now_ms();
        if (node->scroll_goal_ms == 0)
            node->scroll_goal_ms = 1u;    /* 0 是"没有动画"的哨兵 */
        return QL_OK;
    }

    node->scroll_x = nx;
    node->scroll_y = ny;
    node->scroll_goal_ms = 0;
    return QL_OK;
}

ql_err_t ql_ui_scroll_by(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy)
{
    if (ui == NULL || node == NULL)
        return QL_ERR_INVAL;
    if (node->style == NULL || !ql_ui_scrollable(node))
        return QL_ERR_INVAL;

    const ql_coord_t before_x = node->scroll_x;
    const ql_coord_t before_y = node->scroll_y;
    if (ql_ui_scroll_to(ui, node, before_x + dx, before_y + dy) != QL_OK)
        return QL_ERR_INVAL;
    if (node->scroll_x == before_x && node->scroll_y == before_y)
        return QL_OK;              /* 顶到头了：不需要重排 */

    /*
     * 偏移是烘进布局的（见 apply_scroll），所以这里必须重排。
     * 脏区 = 这个容器本身（内容只可能变在它里面）。
     */
    ql_ui_layout(ui, ui->width, ui->height);
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* 从节点往上找最近的可滚动容器 */
static QLUINodeRT *nearest_scrollable(QLUINodeRT *node)
{
    for (QLUINodeRT *n = node; n != NULL; n = n->parent)
        if (ql_ui_scrollable(n))
            return n;
    return NULL;
}

/* ------------------------------ 接入输入系统 ------------------------------ */

void ql_ui_input_event(QLUIRoot *ui, const QLInputEvent *ev)
{
    if (ui == NULL || ev == NULL)
        return;

    switch (ev->type) {
    case QL_INPUT_POINTER_MOVE:
        ql_ui_pointer_move(ui, ev->x, ev->y);
        break;
    case QL_INPUT_POINTER_DOWN:
        ql_ui_pointer_down(ui, ev->x, ev->y);
        break;
    case QL_INPUT_POINTER_UP:
        ql_ui_pointer_up(ui, ev->x, ev->y);
        break;
    case QL_INPUT_POINTER_LEAVE:
        ql_ui_pointer_move(ui, -1, -1);
        break;
    case QL_INPUT_WHEEL: {
        /*
         * 滚轮：`ev->dy` 是**格数**（X11 的 Button4/5 一次一格），不是像素。
         * 乘上 QL_CFG_UI_WHEEL_STEP_PX 才是要滚的距离 —— 一格 ≈ 3 行，
         * 与浏览器一个量级。之前直接当像素用，一格只动 1px（"转飞了才下来一行"）。
         *
         * 滚到底之后继续往上找父级容器 —— 与浏览器一致（列表到底了整页接着滚）。
         */
        const ql_coord_t step = (ql_coord_t)QL_CFG_UI_WHEEL_STEP_PX;
        ql_coord_t dx = (ql_coord_t)ev->dx * step;
        ql_coord_t dy = (ql_coord_t)ev->dy * step;

        QLUINodeRT *hit = ql_ui_hit_test(ui, ev->x, ev->y);

        /*
         * 多行输入框先吃滚轮：它的内容是**自己画的**，不在 overflow 那套里，
         * `nearest_scrollable()` 找不到它。滚到尽头再把剩下的交给父容器。
         */
        if (dy != 0) {
            for (QLUINodeRT *n = hit; n != NULL; n = n->parent) {
                struct QLUIInput *tin = input_of(ui, n);
                if (tin != NULL && tin->multiline) {
                    dy -= input_scroll_by(tin, dy);
                    break;
                }
            }
        }

        QLUINodeRT *target = nearest_scrollable(hit);
        for (int guard = 0; target != NULL && (dy != 0 || dx != 0) && guard < 8;
             guard++) {
            const ql_coord_t old_y = target->scroll_y;
            const ql_coord_t old_x = target->scroll_x;
            (void)ql_ui_scroll_by(ui, target, dx, dy);
            /* 真的动了多少？没动说明这个容器在这个方向已经到边界 */
            const ql_coord_t used_y = target->scroll_y - old_y;
            const ql_coord_t used_x = target->scroll_x - old_x;
            dy -= used_y;
            dx -= used_x;
            if (used_y == 0 && used_x == 0)
                target = nearest_scrollable(target->parent);
            else
                break;
        }
        break;
    }
    case QL_INPUT_TEXT: {
        /* 输入框拿到字符就插入（平台层已经把它翻译成 UTF-8 了） */
        struct QLUIInput *in = (ui->focus != NULL) ? input_of(ui, ui->focus) : NULL;
        if (in != NULL)
            input_insert(ui, in, ev->text);
        break;
    }
    case QL_INPUT_KEY_DOWN: {
        /* Esc 先关浮层（别让输入框先吃掉它） */
        if (ev->key == QL_KEY_ESCAPE && ql_ui_dialog_close_top(ui))
            break;

        /*
         * 剪贴板快捷键。X11 那边带 Ctrl 的组合**不产生文本事件**，
         * 所以这里不会和"插入字符"打架。
         */
        if ((ev->mods & QL_MOD_CTRL) != 0 && ql_ui_focus_node(ui) != NULL) {
            int done = 1;
            if (ev->key == 'a') {
                /* 全选：光标放到末尾、选区锚点放到开头（与浏览器一致） */
                struct QLUIInput *in = focused_input(ui);
                if (in != NULL) {
                    in->sel = 0;
                    in->caret = in->len;
                    in->node->flags |= QL_UI_RT_DIRTY;
                }
            } else if (ev->key == 'c')
                (void)ql_ui_clipboard_copy(ui);
            else if (ev->key == 'x')
                (void)ql_ui_clipboard_cut(ui);
            else if (ev->key == 'v')
                (void)ql_ui_clipboard_paste(ui);
            else
                done = 0;
            if (done)
                break;
        }

        /* Tab / Shift+Tab 先走：它要在可聚焦控件之间跳，和控件自身无关 */
        if (ev->key == QL_KEY_TAB) {
            focus_step(ui, (ev->mods & QL_MOD_SHIFT) ? -1 : 1);
            break;
        }

        /* 焦点在下拉框上：上下键改选中，空格/回车展开 */
        struct QLUISelectState *ss = select_of(ui, ui->focus);
        if (ss != NULL && select_key(ui, ss, ev))
            break;

        /* 焦点在输入框上：方向键/删除/回车都归它 */
        struct QLUIInput *in = (ui->focus != NULL) ? input_of(ui, ui->focus) : NULL;
        if (in != NULL && input_key(ui, in, ev))
            break;

        /* PageUp/PageDown/Home/End 作用于焦点节点所在的可滚动容器 */
        QLUINodeRT *target = nearest_scrollable(ui->focus);
        if (target == NULL)
            break;
        const ql_coord_t page = (target->content_box.h > 0) ? target->content_box.h : 0;
        switch (ev->key) {
        case QL_KEY_PAGE_UP:
            (void)ql_ui_scroll_by(ui, target, 0, -page);
            break;
        case QL_KEY_PAGE_DOWN:
            (void)ql_ui_scroll_by(ui, target, 0, page);
            break;
        case QL_KEY_HOME:
            (void)ql_ui_scroll_to(ui, target, target->scroll_x, 0);
            (void)ql_ui_scroll_by(ui, target, 0, 0);
            break;
        case QL_KEY_END:
            (void)ql_ui_scroll_by(ui, target, 0, target->scroll_max_y);
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void ui_input_sink(const QLInputEvent *ev, void *user)
{
    QLUIRoot *ui = (QLUIRoot *)user;
    if (ui == NULL || ui != s_input_target)
        return;
    ql_ui_input_event(ui, ev);
}

void ql_ui_attach_input(QLUIRoot *ui)
{
    s_input_target = ui;
    ql_input_set_sink(ui_input_sink, ui);
}

void ql_ui_detach_input(void)
{
    s_input_target = NULL;
    ql_input_set_sink(NULL, NULL);
}
