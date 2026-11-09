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
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

/* 定义在下拉框那一段；构建时初始化要用 */
static void select_sync_text(struct QLUISelectState *st);

/* 起播关键帧动画（定义在过渡那一段，构建时要先用） */
static void anim_boot(QLUIRoot *ui);

/* 定义在布局那一段；构建时判断"父节点是不是 flex 容器"要用 */
static int is_flex_container(const QLUIStyle *s);

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
static const QLUIStyle s_style_hidden = { .display = QL_UI_DISPLAY_NONE };

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
    char        buf[QL_CFG_UI_INPUT_LEN];
    uint16_t    len;      /* 字节数（不含结尾 0） */
    uint16_t    caret;    /* 光标：字节偏移，0..len */
    uint16_t    sel;      /* 选区锚点；== caret 表示没有选区 */
    ql_coord_t  scroll_x; /* 文本比盒子宽时横向滚动的像素 */
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
    QLUINodeRT *node;
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

/* ------------------------------ 构建 ------------------------------ */

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
    for (uint16_t i = 0; i < doc->node_count; i++) {
        if (doc->nodes[i].tag == QL_UI_TAG_INPUT) {
            ui->input_count = (uint16_t)(ui->input_count + 1u);
        }
    }
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
        for (uint16_t i = 0; i < doc->node_count && k < ui->input_count; i++) {
            QLUINodeRT *rt = &ui->nodes[i];
            if (rt->src->tag != QL_UI_TAG_INPUT)
                continue;
            struct QLUIInput *in = &ui->inputs[k++];
            in->node = rt;
            /* 初始值 = 解析期从 value 属性带进来的文本（可能没有） */
            const char *init = rt->text;
            size_t n = (init != NULL) ? strlen(init) : 0u;
            if (n > (size_t)QL_CFG_UI_INPUT_LEN - 1u)
                n = (size_t)QL_CFG_UI_INPUT_LEN - 1u;
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
    /* 输入汇里还握着这个指针，必须先摘掉，否则下一次事件就是 use-after-free */
    if (s_input_target == ui)
        ql_ui_detach_input();
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
    ql_mem_free(ui->inputs);
    ql_mem_free(ui->selects);
    ql_mem_free(ui->nodes);
    if (ui->owns_doc)
        ql_ui_doc_free((QLUIDoc *)(uintptr_t)ui->doc);
    ql_mem_free(ui);
}

QLUINodeRT *ql_ui_root_node(QLUIRoot *ui)
{
    return (ui != NULL && ui->node_count > 0) ? &ui->nodes[0] : NULL;
}

/* ------------------------------ 布局 ------------------------------ */

/* 布局期间的视口尺寸（fixed 定位用；单线程，布局期间有效） */
static ql_coord_t s_viewport_w;
static ql_coord_t s_viewport_h;

static ql_coord_t resolve_length(QLUILength len, ql_coord_t basis, ql_coord_t fallback)
{
    switch (len.unit) {
    case QL_UI_UNIT_PX:
        return (ql_coord_t)len.value;
    case QL_UI_UNIT_PERCENT:
        return (ql_coord_t)(((int32_t)basis * (int32_t)len.value) / 100);
    default:
        return fallback;
    }
}

/* 样式 → 字体：字号来自生成期的 font_sizes 表，具体字体由字体系统按字号给 */
static const QLFont *font_of_style(const QLUINodeRT *node, const QLUIStyle *style)
{
    int size = 0;
    if (node != NULL && node->owner != NULL && node->owner->doc != NULL) {
        const QLUIDoc *doc = node->owner->doc;
        if (doc->font_sizes != NULL && style->font_id < doc->font_size_count)
            size = (int)doc->font_sizes[style->font_id];
    }
    return ql_font_default(size > 0 ? size : 16);
}

static ql_coord_t inner_width_of(const QLUIStyle *style, ql_coord_t content_w)
{
    return content_w - style->padding.left - style->padding.right -
           2 * (ql_coord_t)style->border_width;
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
static ql_coord_t text_line_h_of(const QLUIStyle *s, const QLFont *font)
{
    return (s->line_h > 0) ? s->line_h : line_height_of(font);
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
        return iw + s->padding.left + s->padding.right +
               2 * (ql_coord_t)s->border_width;
    }

    const QLFont *font = font_of_style(node, s);
    int text_sized = s->display != QL_UI_DISPLAY_BLOCK &&
                     s->width.unit == QL_UI_UNIT_AUTO &&
                     has_shown_text(node) && node->first_child == NULL;

    if (text_sized)
        return ql_font_line_width(font, node->text, -1) + s->padding.left +
               s->padding.right + 2 * (ql_coord_t)s->border_width;

    if (s->width.unit == QL_UI_UNIT_AUTO) {
        ql_coord_t w = avail_w - s->margin.left - s->margin.right;
        return (w > 0) ? w : 0;
    }

    ql_coord_t v = resolve_length(s->width, avail_w, avail_w);
    if (s->box_sizing == 1)
        return v;
    return v + s->padding.left + s->padding.right + 2 * (ql_coord_t)s->border_width;
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

/* 给节点写一个"强制主/交叉尺寸"，走 style_copy（border-box 语义） */
static void force_size(QLUINodeRT *node, ql_coord_t main_sz, int row, int apply_main,
                       ql_coord_t cross_sz, int apply_cross)
{
    if (!apply_main && !apply_cross)
        return;

    if (!node->used_copy) {
        node->style_copy = *node->style;
        node->style = &node->style_copy;
        node->used_copy = 1;
    }

    QLUIStyle *s = &node->style_copy;
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
                              ql_coord_t avail_w);
static ql_coord_t layout_flex_children(QLUINodeRT *node, ql_coord_t content_x,
                                       ql_coord_t content_y, ql_coord_t content_w,
                                       ql_coord_t avail_h, int definite_h);

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
    if (len.unit == QL_UI_UNIT_PX)
        return (ql_coord_t)len.value;
    if (len.unit == QL_UI_UNIT_PERCENT)
        return (ql_coord_t)(((int32_t)basis * (int32_t)len.value) / 100);
    return fallback;
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
            ql_coord_t tw = ql_font_line_width(font, child->text, -1) +
                            2 * (ql_coord_t)s->border_width + s->padding.left +
                            s->padding.right;
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
        if (!child->used_copy) {
            child->style_copy = *child->style;
            child->style = &child->style_copy;
            child->used_copy = 1;
        }
        child->style_copy.box_sizing = 1;
        child->style_copy.width.value = (int16_t)((own_w < 0) ? 0 : own_w);
        child->style_copy.width.unit = QL_UI_UNIT_PX;
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
        if (!child->used_copy) {
            child->style_copy = *child->style;
            child->style = &child->style_copy;
            child->used_copy = 1;
        }
        child->style_copy.box_sizing = 1;
        child->style_copy.height.value = (int16_t)stretch;
        child->style_copy.height.unit = QL_UI_UNIT_PX;
    }

    ql_coord_t y;
    if (has_t) {
        y = cb.y + t;
    } else if (has_b) {
        (void)layout_node(child, x, 0, avail_w);
        y = cb.y + cb.h - b - child->border_box.h - (s->margin.top + s->margin.bottom);
    } else {
        y = cb.y;        /* 都没给：贴在包含块左上（CSS 的 static position 未实现） */
    }

    (void)layout_node(child, x, y, avail_w);
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

static struct QLUIInput *input_of(const QLUIRoot *ui, const QLUINodeRT *node)
{
    if (ui == NULL || node == NULL || ui->inputs == NULL)
        return NULL;
    if (node->src == NULL || node->src->tag != QL_UI_TAG_INPUT)
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
static void input_insert(QLUIRoot *ui, struct QLUIInput *in, const char *s)
{
    size_t add = strlen(s);
    if (add == 0)
        return;
    (void)input_drop_selection(in);

    const size_t room = (size_t)(QL_CFG_UI_INPUT_LEN - 1u) - in->len;
    if (add > room)
        add = room;
    if (add == 0)
        return;

    memmove(in->buf + in->caret + add, in->buf + in->caret,
            (size_t)(in->len - in->caret) + 1u);
    memcpy(in->buf + in->caret, s, add);
    in->len = (uint16_t)(in->len + add);
    in->caret = (uint16_t)(in->caret + add);
    in->sel = in->caret;
    input_emit(ui, in);
}

static void input_set_caret(QLUIRoot *ui, struct QLUIInput *in, uint16_t pos, int extend)
{
    if (pos > in->len)
        pos = in->len;
    in->caret = pos;
    if (!extend)
        in->sel = pos;
    if (in->node != NULL)
        in->node->flags |= QL_UI_RT_DIRTY;
    (void)ui;
}

/* 点进来：聚焦 + 定光标（并开始拖动选字） */
static int input_pointer_down(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x)
{
    struct QLUIInput *in = input_of(ui, node);
    if (in == NULL)
        return 0;
    ql_ui_set_focus_node(ui, node);
    const QLFont *font = font_of_style(node, node->style);
    const QLRect cb = node->content_box;
    const ql_coord_t rel = x - cb.x + in->scroll_x;
    const uint16_t pos = caret_at_px(font, in->buf, in->len, rel > 0 ? rel : 0);
    input_set_caret(ui, in, pos, 0);
    ui->input_drag = 1;
    return 1;
}

static void input_pointer_move(QLUIRoot *ui, ql_coord_t x)
{
    struct QLUIInput *in = (ui->focus != NULL) ? input_of(ui, ui->focus) : NULL;
    if (in == NULL || !ui->input_drag)
        return;
    const QLFont *font = font_of_style(in->node, in->node->style);
    const QLRect cb = in->node->content_box;
    const ql_coord_t rel = x - cb.x + in->scroll_x;
    const uint16_t pos = caret_at_px(font, in->buf, in->len, rel > 0 ? rel : 0);
    input_set_caret(ui, in, pos, 1);
}

/* 键盘：只处理"编辑"这一层；没有剪贴板（平台层不提供） */
static int input_key(QLUIRoot *ui, struct QLUIInput *in, const QLInputEvent *ev)
{
    const int shift = (ev->mods & QL_MOD_SHIFT) != 0;
    switch (ev->key) {
    case QL_KEY_BACKSPACE:
        if (in->sel != in->caret) {
            (void)input_drop_selection(in);
            input_emit(ui, in);
        } else if (in->caret > 0) {
            const uint16_t from = prev_boundary(in->buf, in->caret);
            memmove(in->buf + from, in->buf + in->caret,
                    (size_t)(in->len - in->caret) + 1u);
            in->len = (uint16_t)(in->len - (in->caret - from));
            in->caret = from;
            in->sel = from;
            input_emit(ui, in);
        }
        return 1;
    case QL_KEY_DELETE:
        if (in->sel != in->caret) {
            (void)input_drop_selection(in);
            input_emit(ui, in);
        } else if (in->caret < in->len) {
            const uint16_t to = next_boundary(in->buf, in->len, in->caret);
            memmove(in->buf + in->caret, in->buf + to,
                    (size_t)(in->len - to) + 1u);
            in->len = (uint16_t)(in->len - (to - in->caret));
            in->sel = in->caret;
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
        input_set_caret(ui, in, 0, shift);
        return 1;
    case QL_KEY_END:
        input_set_caret(ui, in, in->len, shift);
        return 1;
    case QL_KEY_ENTER:
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
    return tag == QL_UI_TAG_INPUT || tag == QL_UI_TAG_SELECT ||
           tag == QL_UI_TAG_BUTTON;
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

    if (s->overflow != QL_UI_OVERFLOW_AUTO) {
        /* visible / hidden：不滚（hidden 只裁不滚，与 CSS 一致） */
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

    node->scroll_max_x = (ext_w > cw) ? (ext_w - cw) : 0;
    node->scroll_max_y = (ext_h > ch) ? (ext_h - ch) : 0;

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

/* ------------------------------ 主布局 ------------------------------ */

static ql_coord_t layout_node(QLUINodeRT *node, ql_coord_t x, ql_coord_t y,
                              ql_coord_t avail_w)
{
    const QLUIStyle *s = node->style;

    if (s->display == QL_UI_DISPLAY_NONE) {
        node->border_box = ql_rect_make(x, y, 0, 0);
        node->content_box = node->border_box;
        return 0;
    }

    const QLFont *font = font_of_style(node, s);
    ql_coord_t border_w = border_width_of(node, avail_w);

    ql_coord_t box_x = x + s->margin.left;
    ql_coord_t box_y = y + s->margin.top;

    ql_coord_t content_x = box_x + s->border_width + s->padding.left;
    ql_coord_t content_y = box_y + s->border_width + s->padding.top;
    ql_coord_t content_w = inner_width_of(s, border_w);
    if (content_w < 0)
        content_w = 0;

    ql_coord_t content_h;

    if (node->first_child != NULL && is_flex_container(s)) {
        /*
         * flex 容器：显式高度时才有剩余空间可分配（grow/shrink 才有意义）。
         * box-sizing：border-box 时 height 指边框盒，content-box 时指内容盒。
         */
        int definite_h = (s->height.unit != QL_UI_UNIT_AUTO);
        ql_coord_t avail_h = 0;
        if (definite_h) {
            ql_coord_t hv = resolve_length(s->height, 0, 0);
            if (s->box_sizing == 1) {
                avail_h = hv - 2 * (ql_coord_t)s->border_width - s->padding.top -
                          s->padding.bottom;
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
                    cursor_y += row_h;
                    cursor_x = content_x;
                    row_h = 0;
                    in_row = 0;
                }
                cursor_y += layout_node(child, content_x, cursor_y, content_w);
            } else {
                ql_coord_t child_avail = content_w;
                ql_coord_t child_w = border_width_of(child, content_w) +
                                     child->style->margin.left + child->style->margin.right;
                if (in_row && cursor_x + child_w > content_x + content_w) {
                    cursor_y += row_h;  /* 换行 */
                    cursor_x = content_x;
                    row_h = 0;
                }
                ql_coord_t h = layout_node(child, cursor_x, cursor_y, child_avail);
                cursor_x += outer_width_of(child);
                if (h > row_h)
                    row_h = h;
                in_row = 1;
            }
        }

        if (in_row)
            cursor_y += row_h;
        content_h = cursor_y - content_y;
    } else if (has_shown_text(node)) {
        const ql_coord_t lh = text_line_h_of(s, font);
        if (s->height.unit == QL_UI_UNIT_AUTO) {
            /*
             * 折行时高度由行数决定：这里必须用**和绘制同一份**行数统计
             * （ql_text_wrap 传 out = NULL 就只数行），否则会出现
             * "布局按一行算、绘制画了三行"的错位。
             */
            int lines = 1;
            if (s->white_space == 1)
                lines = ql_text_wrap(font, node->text, content_w, NULL, 0);
            if (lines < 1)
                lines = 1;
            content_h = lh * (ql_coord_t)lines;
        } else {
            content_h = resolve_length(s->height, 0, lh);
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
    if (s->height.unit != QL_UI_UNIT_AUTO && !(node->first_child != NULL &&
                                               is_flex_container(s))) {
        ql_coord_t hv = resolve_length(s->height, 0, content_h);
        if (s->box_sizing == 1) {
            hv -= 2 * (ql_coord_t)s->border_width + s->padding.top + s->padding.bottom;
            if (hv < 0)
                hv = 0;
        }
        content_h = hv;
    }

    ql_coord_t border_h = content_h + s->padding.top + s->padding.bottom +
                          2 * (ql_coord_t)s->border_width;

    node->border_box = ql_rect_make(box_x, box_y, border_w, border_h);
    node->content_box = ql_rect_make(content_x, content_y, content_w, content_h);

    /* ---- 定位收尾 ---- */
    if (s->position == QL_UI_POSITION_STATIC) {
        node->abs_cb = (node->parent != NULL)
                           ? node->parent->abs_cb
                           : ql_rect_make(0, 0, s_viewport_w, s_viewport_h);
    } else {
        /* 绝对定位的包含块是**padding box**（CSS 规范），即边框盒内缩一个边框宽 */
        ql_coord_t bw = (ql_coord_t)s->border_width;
        node->abs_cb = ql_rect_make(box_x + bw, box_y + bw,
                                    border_w - 2 * bw, border_h - 2 * bw);
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
        row ? (ql_coord_t)(s->padding.left + s->padding.right +
                           2 * (ql_coord_t)s->border_width)
            : (ql_coord_t)(s->padding.top + s->padding.bottom +
                           2 * (ql_coord_t)s->border_width);
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
                own = (font != NULL) ? ql_font_line_width(font, node->text, -1) : 0;
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
                if (s->white_space == 1 && font != NULL) {
                    ql_coord_t inner = avail_w - pad_border - margin;
                    if (inner > 0) {
                        lines = ql_text_wrap(font, node->text, inner, NULL, 0);
                        if (lines < 1)
                            lines = 1;
                    }
                }
                own = lh * (ql_coord_t)lines;
            }
            own += pad_border;
        } else if (node->first_child == NULL) {
            own = pad_border;              /* 空盒子：内容 0 */
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
        } else {
            /* 纵向堆叠（block / column flex）：主轴上是"子项的最大值" */
            ql_coord_t mx = 0;
            for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
                if (c->style->display == QL_UI_DISPLAY_NONE)
                    continue;
                ql_coord_t v = max_content_main(c, row,
                                                avail_w - pad_border - s->margin.left -
                                                    s->margin.right,
                                                depth + 1);
                if (v > mx)
                    mx = v;
            }
            own = mx + pad_border;
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
                                        2 * (ql_coord_t)c->style->border_width)
                         : (ql_coord_t)(c->style->padding.top + c->style->padding.bottom +
                                        2 * (ql_coord_t)c->style->border_width);
            const ql_coord_t len_value =
                (c->style->box_sizing == 1) ? box : (box - pad_border);

            if (!c->used_copy) {
                c->style_copy = *c->style;
                c->style = &c->style_copy;
                c->used_copy = 1;
            }
            if (row_axis) {
                c->style_copy.width.value = (int16_t)((len_value < 0) ? 0 : len_value);
                c->style_copy.width.unit = QL_UI_UNIT_PX;
            } else {
                c->style_copy.height.value = (int16_t)((len_value < 0) ? 0 : len_value);
                c->style_copy.height.unit = QL_UI_UNIT_PX;
            }
        }

        (void)layout_node(c, 0, 0, content_w);   /* 位置无关紧要，只取尺寸 */
    }
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

    if (count_flex_items(node) < 0) {
        QL_LOGW("ui: flex has more than %d children, falling back to block flow",
                QL_CFG_UI_FLEX_MAX_ITEMS);
        ql_coord_t cy = content_y;
        for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;
            cy += layout_node(c, content_x, cy, content_w);
        }
        return cy - content_y;
    }

    /* (1) 测量：在哑位置跑一遍，尺寸写进各子节点的 border_box */
    measure_flex_items(node, content_w);


    ql_coord_t cross_cursor = 0;
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

            const int stretch = (cs->align_items == QL_UI_ALIGN_STRETCH) &&
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
                if (cs->align_items == QL_UI_ALIGN_CENTER)
                    cross_pos = room / 2;
                else if (cs->align_items == QL_UI_ALIGN_END)
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
                (void)layout_node(c, content_x + main_pos, content_y + cross_at, inner_sz);
            else
                (void)layout_node(c, content_x + cross_at, content_y + main_pos, content_w);

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
        if (line_begin != NULL)
            cross_cursor += row ? (ql_coord_t)cs->gap_row : (ql_coord_t)cs->gap_col;

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
static int collect_node_styles(const QLUINodeRT *node, const QLUIStyle *out[4])
{
    out[0] = node->style;
    int n = (node->style != NULL) ? 1 : 0;

    const QLUIDoc *doc = (node->owner != NULL) ? node->owner->doc : NULL;
    const QLUINode *src = node->src;
    if (doc == NULL || src == NULL)
        return n;

    const uint16_t idx[3] = { src->style_hover, src->style_active, src->style_focus };
    for (int i = 0; i < 3; i++) {
        if (idx[i] != 0xFFFF && idx[i] < doc->style_count)
            out[n++] = &doc->styles[idx[i]];
    }
    return n;
}

/* 阴影可能画到的矩形（不算阴影时返回空矩形） */
static QLRect shadow_extent(QLRect box, const QLUIStyle *s)
{
    if (s == NULL || s->shadow_alpha == 0 || ql_rect_is_empty(box))
        return ql_rect_make(0, 0, 0, 0);
    ql_coord_t pad = s->shadow_blur + 1;
    return ql_rect_make(box.x + s->shadow_dx - pad, box.y + s->shadow_dy - pad,
                        box.w + 2 * pad, box.h + 2 * pad);
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

    const QLUIStyle *styles[4];
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

    const QLUIStyle *styles[4];
    const int n = collect_node_styles(node, styles);
    for (int i = 0; i < n; i++) {
        if (styles[i] == NULL)
            continue;
        if (styles[i]->sc_x > *mx)
            *mx = styles[i]->sc_x;
        if (styles[i]->sc_y > *my)
            *my = styles[i]->sc_y;
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
        if (f->sc_x > *mx)
            *mx = f->sc_x;
        if (f->sc_y > *my)
            *my = f->sc_y;
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
    const QLUIStyle *styles[4];
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

    ui->width = width;
    ui->height = height;
    s_viewport_w = width;
    s_viewport_h = height;

    /*
     * 清掉上一轮遗留的 style_copy：flex 分配和绝对定位都会往 style_copy 里写尺寸，
     * 不清就会把上一帧的强制尺寸带到这一帧（视口变窄时表现为"尺寸不更新"）。
     */
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
        n->used_copy = 0;
    }

    QLUINodeRT *root = &ui->nodes[0];
    root->abs_cb = ql_rect_make(0, 0, width, height);
    layout_node(root, 0, 0, width);

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
        uint16_t idx = 0xFFFF;
        if ((node->flags & QL_UI_RT_ACTIVE) != 0)
            idx = src->style_active;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_FOCUS) != 0)
            idx = src->style_focus;
        if (idx == 0xFFFF && (node->flags & QL_UI_RT_HOVER) != 0)
            idx = src->style_hover;
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

    const QLRect box = ql_ui_node_rect(node);
    ql_coord_t th = 0, ty = 0;
    scrollbar_thumb(box.h, node->scroll_max_y, node->scroll_y, &th, &ty);
    if (th <= 0)
        return;

    QLRect bar = ql_rect_make(ql_rect_right(box) - SCROLLBAR_INSET - SCROLLBAR_W,
                              box.y + ty, SCROLLBAR_W, th);
    d2d->fill_round_rect(d2d, bar, SCROLLBAR_W / 2, 0x808080, 0x96);
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
        if (ir->node->text == NULL || strcmp(ir->node->text, in->name) != 0)
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
        ir->node->flags |= QL_UI_RT_DIRTY;
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
    const ql_coord_t radius = (ql_coord_t)s->border_radius;
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

    /* 密码框显示成圆点，所以宽度必须按**显示文本**量，否则光标会飘 */
    char dots[QL_CFG_UI_INPUT_LEN * 3 + 4];
    const int pw = (node->src->flags & QL_UI_NODE_PASSWORD) != 0;
    const char *disp = input_display_text(in, pw, dots, sizeof(dots));

    /* 原字节偏移 → 显示文本的字节偏移 */
    #define DISP_OFF(o)                                                        \
        ((uint16_t)((pw) ? (uint16_t)(cp_count(in->buf, (o)) * 3) : (o)))

    /* 选区 */
    if (in->sel != in->caret && d2d->fill_rect != NULL) {
        uint16_t a = in->sel;
        uint16_t b = in->caret;
        if (a > b) {
            const uint16_t t = a;
            a = b;
            b = t;
        }
        const ql_coord_t x0 = base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(a));
        const ql_coord_t x1 = base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(b));
        if (x1 > x0) {
            QLRect r = ql_rect_make(x0, cb.y, x1 - x0, cb.h);
            d2d->fill_rect(d2d, r, node->style->text_color, 0x50);
        }
    }

    /* 光标：只在聚焦时画（不闪 —— 闪烁要主循环一直醒着，省电优先） */
    if (ui->focus == node && d2d->fill_rect != NULL) {
        const ql_coord_t cx =
            base_x + ql_font_line_width(font, disp, (int32_t)DISP_OFF(in->caret));
        QLRect r = ql_rect_make(cx, cb.y + 2, 1, (cb.h > 4) ? cb.h - 4 : cb.h);
        d2d->fill_rect(d2d, r, node->style->text_color, 255);
    }
    #undef DISP_OFF
}

static void render_node(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);

    if (s->display == QL_UI_DISPLAY_NONE)
        return;

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
    if (s->tf_x != 0 || s->tf_y != 0) {
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
    if (s->sc_x != 1000 || s->sc_y != 1000)
        ql_draw2d_set_elem_scale_xy(
            d2d, (ql_coord_t)((int64_t)s->sc_x * QL_SCALE_ONE / 1000),
            (ql_coord_t)((int64_t)s->sc_y * QL_SCALE_ONE / 1000), elem_cx, elem_cy);

    const ql_coord_t saved_mx = d2d->mask_cx, saved_my = d2d->mask_cy;
    const ql_coord_t saved_mr = d2d->mask_r;
    if (s->clip_kind == 1 && d2d->set_mask != NULL) {
        ql_coord_t dmax = ql_len_far_corner(box.w, box.h);
        ql_coord_t r = (ql_coord_t)(((int32_t)dmax * (int32_t)s->clip_r) / 1000);
        if (saved_mr >= 0)
            QL_LOGW("ui: nested clip-path is not supported (node #%u), keeping the "
                    "outer circle", (unsigned)(node - node->owner->nodes));
        else
            ql_draw2d_set_mask(d2d, box.x + box.w / 2, box.y + box.h / 2, r);
    }

    /*
     * 毛玻璃：必须在**画自己的背景之前**做，否则会连自己一起糊掉。
     * 模糊的是"这一层背后已经画好的内容"，所以它天然跟随 z 序。
     */
    if (s->backdrop_blur > 0 && !ql_rect_is_empty(box) && d2d->backdrop_blur != NULL)
        (void)ql_draw2d_backdrop_blur(d2d, box, (int)s->backdrop_blur);

    /* 阴影先画（在背景之下）；阴影轮廓取边框盒，与 CSS box-shadow 一致 */
    if (s->shadow_alpha > 0 && d2d->fill_shadow != NULL && !ql_rect_is_empty(box))
        d2d->fill_shadow(d2d, box, s->border_radius, s->shadow_blur,
                         s->shadow_dx, s->shadow_dy, s->shadow_color, s->shadow_alpha);

    if (s->bg_alpha > 0 && !ql_rect_is_empty(box)) {
        if (s->bg_grad_dir != 0 && d2d->fill_round_rect_gradient != NULL)
            d2d->fill_round_rect_gradient(d2d, box, s->border_radius, s->bg_color,
                                          s->bg_color2,
                                          (uint8_t)(s->bg_grad_dir - 1), s->bg_alpha);
        else
            d2d->fill_round_rect(d2d, box, s->border_radius, s->bg_color, s->bg_alpha);
    }

    if (s->border_width > 0 && !ql_rect_is_empty(box))
        d2d->stroke_round_rect(d2d, box, s->border_radius, s->border_width,
                               s->border_color);

    /* <img>：背景/边框画完之后贴图（P3a：object-fit: fill） */
    if (node->src != NULL && node->src->tag == QL_UI_TAG_IMG)
        draw_img(node->owner, node, d2d);

    /* EMBED 包：画标签名当占位提示（这些节点自身没有文本） */
    if (node->src != NULL && tag_is_embed_box(node->src->tag))
        draw_embed_placeholder(node, d2d);

    /* <progress>/<meter>：轨道就是背景（已经画完），这里补填充 */
    if (node->src != NULL && (node->src->tag == QL_UI_TAG_PROGRESS ||
                              node->src->tag == QL_UI_TAG_METER))
        draw_progress(node, d2d);

    /*
     * <img> 的 `node->text` 是**资源名**（"logo.png"），不是要显示的文字 ——
     * 这一条必须挡住，否则资源名会被当成正文画在图片上。
     */
    if (has_shown_text(node) && node->text[0] != '\0' &&
        !ql_rect_is_empty(node->content_box)) {
        /*
         * 输入框：显示文本可能被换成圆点（密码），而且比盒子宽时要横向滚。
         * 这两件事都只在输入框上发生，普通文本节点走的是同一条 draw_text。
         */
        struct QLUIInput *in = input_of(node->owner, node);
        char dots[QL_CFG_UI_INPUT_LEN * 3 + 4];
        const char *shown = node->text;
        QLRect tbox = node->content_box;
        if (in != NULL) {
            shown = input_display_text(in, (node->src->flags & QL_UI_NODE_PASSWORD) != 0,
                                       dots, sizeof(dots));
            tbox.x -= in->scroll_x;
        }

        QLTextStyle ts;
        memset(&ts, 0, sizeof(ts));
        ts.color = s->text_color;
        ts.line_h = s->line_h;
        ts.align = s->text_align;
        ts.wrap = (uint8_t)(s->white_space == 1);
        ts.bold = (uint8_t)((s->font_flags & QL_UI_FONT_BOLD) != 0);
        ts.italic = (uint8_t)((s->font_flags & QL_UI_FONT_ITALIC) != 0);
        ts.deco = (uint8_t)(((s->font_flags & QL_UI_FONT_UNDERLINE) ? QL_TEXT_DECO_UNDERLINE
                                                                   : 0u) |
                            ((s->font_flags & QL_UI_FONT_LINE_THROUGH)
                                 ? QL_TEXT_DECO_LINE_THROUGH
                                 : 0u));
        d2d->draw_text(d2d, font_of(node), shown, -1, tbox, &ts);
        draw_input_decor(node, d2d);
    }

    /* overflow != visible 时，子树被裁到本节点的边框盒（含圆角） */
    if (s->overflow != QL_UI_OVERFLOW_VISIBLE && d2d->set_clip != NULL) {
        QLRect saved = clip;
        QLRect clipped = ql_rect_intersect(saved, box);
        /* 裁剪同时记进 ui->clip：子节点的剪枝判断必须用真实裁剪，
           否则会按外层更大的裁剪放行，白跑一遍 */
        node->owner->clip = clipped;
        d2d->set_clip(d2d, clipped);
        for (QLUINodeRT *child = node->first_child; child != NULL;
             child = child->next_sibling) {
            render_node(child, d2d);
        }
        draw_scrollbar(node, d2d);
        d2d->set_clip(d2d, saved);
        node->owner->clip = saved;
        ql_draw2d_set_offset(d2d, saved_ox, saved_oy);
        if (d2d->set_mask != NULL)
            ql_draw2d_set_mask(d2d, saved_mx, saved_my, saved_mr);
        if (saved_efx != d2d->elem_x_fx || saved_efy != d2d->elem_y_fx)
            ql_draw2d_set_elem_scale_xy(d2d, saved_efx, saved_efy, 0, 0);
        return;
    }

    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        render_node(child, d2d);
    }
    ql_draw2d_set_offset(d2d, saved_ox, saved_oy);
    if (d2d->set_mask != NULL)
        ql_draw2d_set_mask(d2d, saved_mx, saved_my, saved_mr);
    if (saved_efx != d2d->elem_x_fx || saved_efy != d2d->elem_y_fx)
        ql_draw2d_set_elem_scale_xy(d2d, saved_efx, saved_efy, 0, 0);
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
        if (s->sc_x <= 0 || s->sc_y <= 0)
            return NULL;
        ql_coord_t cx0 = box.x + box.w / 2;
        ql_coord_t cy0 = box.y + box.h / 2;
        hx = cx0 + (ql_coord_t)(((int64_t)(x - cx0) * 1000) / s->sc_x);
        hy = cy0 + (ql_coord_t)(((int64_t)(y - cy0) * 1000) / s->sc_y);
    }

    /* 视觉范围：放大时比布局盒大，overflow 的裁剪区得跟着它走（绘制侧也是按缩放后的矩形裁） */
    QLRect vis = box;
    if (s->sc_x > 1000 || s->sc_y > 1000) {
        ql_coord_t cx0 = box.x + box.w / 2;
        ql_coord_t cy0 = box.y + box.h / 2;
        ql_coord_t hw = (s->sc_x > 1000)
                            ? (ql_coord_t)((int64_t)box.w * s->sc_x / 2000)
                            : box.w / 2;
        ql_coord_t hh = (s->sc_y > 1000)
                            ? (ql_coord_t)((int64_t)box.h * s->sc_y / 2000)
                            : box.h / 2;
        vis = ql_rect_make(cx0 - hw, cy0 - hh, hw * 2, hh * 2);
    }

    /* overflow 容器把子树裁到自己的边框盒 */
    QLRect child_clip = clip;
    if (s->overflow != QL_UI_OVERFLOW_VISIBLE)
        child_clip = ql_rect_intersect(clip, vis);
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

    QLUINodeRT *hit = NULL;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        QLUINodeRT *found = hit_walk_ui(c, x, y, child_clip, ox + s->tf_x,
                                        oy + s->tf_y, nfx, nfy, ncx, ncy);
        if (found != NULL)
            hit = found;
    }
    if (hit != NULL)
        return hit;

    /* 自己：必须在裁剪区内（被 overflow 裁掉的部分不可命中） */
    if (x < clip.x || x >= ql_rect_right(clip) || y < clip.y || y >= ql_rect_bottom(clip))
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

void ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data)
{
    if (ui == NULL)
        return;
    ui->click_cb = cb;
    ui->click_user_data = user_data;
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
    if ((t->props & QL_UI_TRANS_BORDER) != 0)
        t->style.border_color = lerp_color(t->from.border_color, t->to.border_color, p);
    if ((t->props & QL_UI_TRANS_RADIUS) != 0) {
        float r = (float)t->from.border_radius +
                  ((float)t->to.border_radius - (float)t->from.border_radius) * p;
        t->style.border_radius = (int16_t)kf_i16(r);
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

    const ql_color_t bd = has_border ? f->border_color : base->border_color;
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
        out->rad = (float)base->border_radius;

    if (!all_base && (f->props & QL_UI_KF_CLIP) != 0)
        out->clip = (float)f->clip_r;
    else
        out->clip = (float)base->clip_r;

    if (!all_base && (f->props & QL_UI_KF_MOVE) != 0) {
        out->tx = (float)f->tf_x;
        out->ty = (float)f->tf_y;
        out->sc_x = (float)((f->sc_x < 0) ? base->sc_x : f->sc_x);
        out->sc_y = (float)((f->sc_y < 0) ? base->sc_y : f->sc_y);
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
    if ((props & QL_UI_KF_BORDER) != 0)
        run->style.border_color = QL_COLOR_RGB(kf_u8(a.br + (b.br - a.br) * t),
                                               kf_u8(a.bg + (b.bg - a.bg) * t),
                                               kf_u8(a.bb + (b.bb - a.bb) * t));
    if ((props & QL_UI_KF_RADIUS) != 0)
        run->style.border_radius = (int16_t)kf_i16(a.rad + (b.rad - a.rad) * t);
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

uint32_t ql_ui_anim_tick(QLUIRoot *ui, uint32_t now_ms)
{
    if (ui == NULL)
        return 0;

    uint32_t moving = anim_tick_runs(ui, now_ms);
    moving += ql_anim_tick(now_ms);            /* 状态过渡，池在 qlanim 里 */

    /* 事件统一在本帧末尾派发：回调里再起动画 / 改文本都是安全的 */
    anim_event_flush(ui);
    return moving;
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
    return n;
}

static void set_state_chain(QLUINodeRT *node, uint8_t mask, int on)
{
    for (QLUINodeRT *n = node; n != NULL; n = n->parent) {
        /* 先记下变化前的生效样式：过渡的起点必须是"屏幕上正在显示的那个值" */
        const QLUIStyle *before = ql_ui_node_style(n);
        if (on)
            n->flags = (uint8_t)(n->flags | mask);
        else
            n->flags = (uint8_t)(n->flags & ~mask);
        /* 状态位变了：让有 transition 的节点起/改道渐变 */
        if (n->owner != NULL)
            trans_retarget(n->owner, n, before);
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
    if (hit != NULL && hit->src != NULL && hit->src->tag == QL_UI_TAG_INPUT) {
        if (input_pointer_down(ui, hit, x))
            return 1;
    }

    /* <details>：点标题行切换展开/收起（吃掉落点击，不标 :active） */
    if (details_pointer_down(ui, hit, x, y))
        return 1;

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
        input_pointer_move(ui, x);
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
    if (ui->focus != NULL)
        set_state_chain(ui->focus, QL_UI_RT_FOCUS, 0);
    ui->focus = node;
    if (node != NULL)
        set_state_chain(node, QL_UI_RT_FOCUS, 1);
}

QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui)
{
    return (ui != NULL) ? ui->focus : NULL;
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
    if (n > (size_t)QL_CFG_UI_INPUT_LEN - 1u)
        n = (size_t)QL_CFG_UI_INPUT_LEN - 1u;
    if (n > 0)
        memcpy(in->buf, value, n);
    in->buf[n] = '\0';
    in->len = (uint16_t)n;
    in->caret = in->len;          /* 光标到末尾，符合"程序改值"的直觉 */
    in->sel = in->caret;
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* ------------------------------ 滚动 API ------------------------------ */

int ql_ui_scrollable(const QLUINodeRT *node)
{
    if (node == NULL || node->style == NULL)
        return 0;
    return node->style->overflow == QL_UI_OVERFLOW_AUTO &&
           (node->scroll_max_x > 0 || node->scroll_max_y > 0);
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

    if (nx == node->scroll_x && ny == node->scroll_y)
        return QL_OK;

    node->scroll_x = nx;
    node->scroll_y = ny;
    return QL_OK;
}

ql_err_t ql_ui_scroll_by(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy)
{
    if (ui == NULL || node == NULL)
        return QL_ERR_INVAL;
    if (node->style == NULL || node->style->overflow != QL_UI_OVERFLOW_AUTO)
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
         * 滚轮：命中测试找到指针下的节点，往上找最近的可滚动容器。
         * 滚不到就交给父级 —— 与浏览器一致（列表滚到底以后整页继续滚）。
         */
        QLUINodeRT *hit = ql_ui_hit_test(ui, ev->x, ev->y);
        QLUINodeRT *target = nearest_scrollable(hit);
        if (target != NULL)
            (void)ql_ui_scroll_by(ui, target, ev->dx, ev->dy);
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
