/**
 * @file    qlui.c
 * @brief   把构建期生成的 QLUIDoc 物化成节点树，并做布局与绘制。
 *
 * 布局模型（v0）：
 *   - 只做 block 与 inline/inline-block 流式布局，不含 flex/grid/position；
 *   - 宽度支持 px 与百分比（相对父内容宽度），高度支持 px 与 auto；
 *   - 文本节点由生成器折叠成"元素的一段文本"，不支持混合内容。
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

QLUIRoot *ql_ui_build(const QLUIDoc *doc)
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

static QLUIRoot *s_input_target;   /* 挂在输入系统上的根（同一时刻只有一个） */

void ql_ui_free(QLUIRoot *ui)
{
    if (ui == NULL)
        return;
    /* 输入汇里还握着这个指针，必须先摘掉，否则下一次事件就是 use-after-free */
    if (s_input_target == ui)
        ql_ui_detach_input();
    ql_mem_free(ui->nodes);
    ql_mem_free(ui);
}

QLUINodeRT *ql_ui_root_node(QLUIRoot *ui)
{
    return (ui != NULL && ui->node_count > 0) ? &ui->nodes[0] : NULL;
}

/* ------------------------------ 布局 ------------------------------ */

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

/*
 * 递归布局：给定"父内容盒"的位置与宽度，确定本节点的边框盒。
 * 返回本节点的外部高度（含上下 margin），供父节点推进游标。
 */
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

    if (node->first_child != NULL) {
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

    /* 显式高度优先 */
    if (s->height.unit != QL_UI_UNIT_AUTO) {
        ql_coord_t h = resolve_length(s->height, 0, content_h);
        if (h > content_h)
            content_h = h;      /* 显式更高时撑开；更矮时允许内容溢出 */
    }

    ql_coord_t border_h = content_h + s->padding.top + s->padding.bottom +
                          2 * (ql_coord_t)s->border_width;

    node->border_box = ql_rect_make(box_x, box_y, border_w, border_h);
    node->content_box = ql_rect_make(content_x, content_y, content_w, content_h);

    return border_h + s->margin.top + s->margin.bottom;
}

void ql_ui_layout(QLUIRoot *ui, ql_coord_t width, ql_coord_t height)
{
    if (ui == NULL || ui->node_count == 0)
        return;

    ui->width = width;
    ui->height = height;

    QLUINodeRT *root = &ui->nodes[0];
    root->style = root->used_copy ? &root->style_copy : root->style;
    layout_node(root, 0, 0, width);
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

    if (s->bg_alpha > 0 && !ql_rect_is_empty(box))
        d2d->fill_round_rect(d2d, box, s->border_radius, s->bg_color, s->bg_alpha);

    if (s->border_width > 0 && !ql_rect_is_empty(box))
        d2d->stroke_round_rect(d2d, box, s->border_radius, s->border_width,
                               s->border_color);

    if (node->text != NULL && node->text[0] != '\0' && !ql_rect_is_empty(node->content_box))
        d2d->draw_text(d2d, font_of(node), node->text, -1, node->content_box,
                       s->text_color, (QLTextAlign)s->text_align);

    for (QLUINodeRT *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        render_node(child, d2d);
    }
}

void ql_ui_render(QLUIRoot *ui, QLDraw2D *d2d)
{
    if (ui == NULL || d2d == NULL || ui->node_count == 0)
        return;
    render_node(&ui->nodes[0], d2d);
}

void ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas)
{
    if (ui == NULL || canvas == NULL)
        return;
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

QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    if (ui == NULL)
        return NULL;

    /* 反向遍历：后面的节点在视觉上更靠上，命中优先 */
    for (int32_t i = (int32_t)ui->node_count - 1; i >= 0; i--) {
        QLUINodeRT *node = &ui->nodes[i];
        if (node->style->display == QL_UI_DISPLAY_NONE)
            continue;
        QLRect r = node->border_box;
        if (ql_rect_is_empty(r))
            continue;
        if (x >= r.x && y >= r.y && x < ql_rect_right(r) && y < ql_rect_bottom(r))
            return node;
    }
    return NULL;
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
