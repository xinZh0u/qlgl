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
#include <qlgl/qlgl.h>

static const char *const s_tag_names[QL_UI_TAG_COUNT] = {
    "div", "section", "p", "span", "b", "i", "button", "h1", "h2", "h3",
};

const char *ql_ui_tag_name(QLUITag tag)
{
    return (tag < QL_UI_TAG_COUNT) ? s_tag_names[tag] : "?";
}

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

/* 计算节点的边框盒宽度（与 layout_node 内部逻辑保持一致） */
static ql_coord_t border_width_of(const QLUINodeRT *node, ql_coord_t avail_w)
{
    const QLUIStyle *s = node->style;

    if (s->display == QL_UI_DISPLAY_NONE)
        return 0;

    const QLFont *font = font_of_style(node, s);
    int text_sized = s->display != QL_UI_DISPLAY_BLOCK &&
                     s->width.unit == QL_UI_UNIT_AUTO &&
                     node->text != NULL && node->first_child == NULL;

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
        if (child->text != NULL && child->first_child == NULL && font != NULL) {
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
    } else if (node->text != NULL) {
        content_h = s->height.unit == QL_UI_UNIT_AUTO ? line_height_of(font)
                                                      : resolve_length(s->height, 0,
                                                                       line_height_of(font));
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
static void measure_flex_items(QLUINodeRT *node, ql_coord_t content_w)
{
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        if (c->style->display == QL_UI_DISPLAY_NONE)
            continue;
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

/* justify-content → 行首偏移 + 额外项间距 */
static void justify_offsets(ql_coord_t free_space, int n, uint8_t justify,
                            ql_coord_t *out_start, ql_coord_t *out_extra)
{
    *out_start = 0;
    *out_extra = 0;
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
        if (n > 1)
            *out_extra = free_space / (n - 1);
        break;
    case QL_UI_JUSTIFY_SPACE_AROUND:
        *out_start = free_space / (2 * n);
        *out_extra = free_space / n;
        break;
    case QL_UI_JUSTIFY_SPACE_EVENLY:
        *out_start = free_space / (n + 1);
        *out_extra = free_space / (n + 1);
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
        ql_coord_t j_start = 0, j_extra = 0;
        if (sum_grow <= 0)
            justify_offsets(free_space, cnt, cs->justify_content, &j_start, &j_extra);

        /* (6) 分配 + 定位 */
        ql_coord_t cursor = j_start;
        for (QLUINodeRT *c = first; c != NULL; c = c->next_sibling) {
            if (c->style->display == QL_UI_DISPLAY_NONE)
                continue;

            const QLUIStyle *is = c->style;
            ql_coord_t base = flex_base_of(c, row, inner_main);
            ql_coord_t final_main = base;

            if (free_space > 0 && sum_grow > 0) {
                final_main = base + (ql_coord_t)((int64_t)free_space *
                                                 (int64_t)is->flex_grow /
                                                 (int64_t)sum_grow);
            } else if (free_space < 0 && sum_shrink_w > 0 && is->flex_shrink > 0) {
                ql_coord_t w = (ql_coord_t)((uint32_t)is->flex_shrink *
                                            (uint32_t)((base > 0) ? base : 1) / 100u);
                ql_coord_t cut = (ql_coord_t)((int64_t)(-free_space) * (int64_t)w /
                                              (int64_t)sum_shrink_w);
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

            cursor += final_main + gap + j_extra;
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
}

/* ------------------------------ 绘制 ------------------------------ */

static const QLFont *font_of(const QLUINodeRT *node)
{
    return font_of_style(node, node->style);
}

const QLFont *ql_ui_node_font(const QLUINodeRT *node)
{
    return (node != NULL) ? font_of(node) : NULL;
}

const QLUIStyle *ql_ui_node_style(const QLUINodeRT *node)
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

QLRect ql_ui_node_rect(const QLUINodeRT *node)
{
    return (node != NULL) ? node->border_box : ql_rect_make(0, 0, 0, 0);
}

const char *ql_ui_node_text(const QLUINodeRT *node)
{
    return (node != NULL && node->text != NULL) ? node->text : "";
}

static void render_node(QLUINodeRT *node, QLDraw2D *d2d)
{
    const QLUIStyle *s = ql_ui_node_style(node);

    if (s->display == QL_UI_DISPLAY_NONE)
        return;

    QLRect box = node->border_box;

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

    if (node->text != NULL && node->text[0] != '\0' && !ql_rect_is_empty(node->content_box))
        d2d->draw_text(d2d, font_of(node), node->text, -1, node->content_box,
                       s->text_color, (QLTextAlign)s->text_align);

    /* overflow != visible 时，子树被裁到本节点的边框盒（含圆角） */
    if (s->overflow != QL_UI_OVERFLOW_VISIBLE && d2d->set_clip != NULL) {
        QLRect saved = node->owner != NULL ? node->owner->clip : ql_rect_make(0, 0, 0, 0);
        QLRect clipped = ql_rect_intersect(saved, box);
        d2d->set_clip(d2d, clipped);
        for (QLUINodeRT *child = node->first_child; child != NULL;
             child = child->next_sibling) {
            render_node(child, d2d);
        }
        d2d->set_clip(d2d, saved);
        return;
    }

    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        render_node(child, d2d);
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
}

void ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas)
{
    if (ui == NULL || canvas == NULL)
        return;

    /* 画布已有 clip 时以它为准，避免 overflow 恢复时把外部裁剪冲掉 */
    ui->root_clip = canvas->clip;
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
static QLUINodeRT *hit_walk_ui(QLUINodeRT *node, ql_coord_t x, ql_coord_t y, QLRect clip)
{
    if (node == NULL || node->style->display == QL_UI_DISPLAY_NONE)
        return NULL;

    QLRect box = node->border_box;
    if (ql_rect_is_empty(box))
        return NULL;

    /* overflow 容器把子树裁到自己的边框盒 */
    QLRect child_clip = clip;
    if (node->style->overflow != QL_UI_OVERFLOW_VISIBLE)
        child_clip = ql_rect_intersect(clip, box);
    if (ql_rect_is_empty(child_clip))
        return NULL;

    /* 正向遍历 + 覆盖：链表顺序即绘制顺序，最后一个命中的就是最上层的 */
    QLUINodeRT *hit = NULL;
    for (QLUINodeRT *c = node->first_child; c != NULL; c = c->next_sibling) {
        QLUINodeRT *found = hit_walk_ui(c, x, y, child_clip);
        if (found != NULL)
            hit = found;
    }
    if (hit != NULL)
        return hit;

    /* 自己：必须在裁剪区内（被 overflow 裁掉的部分不可命中） */
    if (x < clip.x || x >= ql_rect_right(clip) || y < clip.y || y >= ql_rect_bottom(clip))
        return NULL;
    if (x >= box.x && x < ql_rect_right(box) && y >= box.y && y < ql_rect_bottom(box))
        return node;
    return NULL;
}

QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL || ui->node_count == 0)
        return NULL;
    QLRect screen = ql_rect_make(0, 0, ui->width, ui->height);
    return hit_walk_ui(&ui->nodes[0], x, y, screen);
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
    node->text = text;
    node->flags |= QL_UI_RT_DIRTY;
    return QL_OK;
}

/* 交互状态位：和 Web 一样沿祖先链生效（子元素 hover 时父容器也算 hover） */
static void set_state_chain(QLUINodeRT *node, uint8_t mask, int on)
{
    for (QLUINodeRT *n = node; n != NULL; n = n->parent) {
        if (on)
            n->flags = (uint8_t)(n->flags | mask);
        else
            n->flags = (uint8_t)(n->flags & ~mask);
    }
}

int ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    QLUINodeRT *node = ql_ui_hit_test(ui, x, y);
    if (node == NULL)
        return 0;
    set_state_chain(node, QL_UI_RT_ACTIVE, 1);
    return 1;
}

int ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return 0;

    QLUINodeRT *node = ql_ui_hit_test(ui, x, y);
    int was_active = 0;
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if ((ui->nodes[i].flags & QL_UI_RT_ACTIVE) != 0)
            was_active = 1;
        ui->nodes[i].flags &= (uint8_t)~QL_UI_RT_ACTIVE;
    }
    if (node == NULL || !was_active)
        return 0;

    if (ui->click_cb != NULL)
        ui->click_cb(ui, node, ui->click_user_data);
    return 1;
}

int ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return 0;

    QLUINodeRT *node = ql_ui_hit_test(ui, x, y);
    if (ui->hover == node)
        return 0;

    for (uint16_t i = 0; i < ui->node_count; i++)
        ui->nodes[i].flags &= (uint8_t)~QL_UI_RT_HOVER;
    ui->hover = node;
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
