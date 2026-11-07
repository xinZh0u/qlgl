/**
 * @file    qlui.h
 * @brief   HTML + CSS 构建期编译结果（QLUIDoc）与运行期物化/布局/绘制。
 *
 * 工作流（与 Qt 的 .ui/uic 相同）：
 *     index.html + index.css  ──tools/qlui/qlui.py──▶  ui_index.c/.h  ──编译器──▶ .rodata
 *
 * 运行期只做三件事，不做任何 CSS 解析：
 *     QLUIRoot *ui = ql_ui_build(&ui_index_doc);
 *     ql_ui_layout(ui, 320, 240);
 *     ql_ui_render(ui, canvas);
 */
#ifndef QLGL_UI_H
#define QLGL_UI_H

#include "qlgl.h"
#include "qlcanvas.h"
#include "qldraw2d.h"

/* ----- 生成数据的格式（构建期产出，运行期只读） ----- */

typedef enum {
    QL_UI_TAG_DIV = 0,
    QL_UI_TAG_SECTION,
    QL_UI_TAG_P,
    QL_UI_TAG_SPAN,
    QL_UI_TAG_B,
    QL_UI_TAG_I,
    QL_UI_TAG_BUTTON,
    QL_UI_TAG_H1,
    QL_UI_TAG_H2,
    QL_UI_TAG_H3,
    QL_UI_TAG_COUNT
} QLUITag;

typedef enum {
    QL_UI_DISPLAY_BLOCK = 0,
    QL_UI_DISPLAY_INLINE,
    QL_UI_DISPLAY_INLINE_BLOCK,
    QL_UI_DISPLAY_NONE,
} QLUIDisplay;

typedef enum {
    QL_UI_UNIT_AUTO = 0,   /* 由布局决定 */
    QL_UI_UNIT_PX,
    QL_UI_UNIT_PERCENT,    /* 相对父元素内容宽度 */
} QLUIUnit;

/* 一条边的四个值，顺序固定为 上右下左 */
typedef struct {
    int16_t top;
    int16_t right;
    int16_t bottom;
    int16_t left;
} QLUIEdges;

typedef struct {
    uint16_t value;        /* px 值，或百分比整数（如 50 表示 50%） */
    uint8_t  unit;         /* QLUIUnit */
    uint8_t  reserved;
} QLUILength;

typedef struct {
    uint32_t    bg_color;      /* 0xRRGGBB；alpha 见 bg_alpha */
    uint32_t    text_color;
    uint32_t    border_color;

    QLUILength  width;
    QLUILength  height;

    QLUIEdges   margin;
    QLUIEdges   padding;

    uint8_t     display;       /* QLUIDisplay */
    uint8_t     bg_alpha;      /* 0..255 */
    uint8_t     border_width;
    uint8_t     border_radius;
    uint8_t     text_align;    /* QLTextAlign */
    uint8_t     font_id;       /* doc->font_sizes 的下标（字号，像素） */
    uint8_t     box_sizing;    /* 0 = content-box, 1 = border-box */
    uint8_t     reserved;
} QLUIStyle;

typedef struct {
    uint16_t tag;          /* QLUITag */
    uint16_t parent;       /* 0xFFFF = 根 */
    uint16_t first_child;  /* 0xFFFF = 无 */
    uint16_t next_sibling; /* 0xFFFF = 无 */
    uint16_t style;        /* 基础样式下标 */
    uint16_t text;         /* 文本池下标，0xFFFF = 无文本 */
    /* 状态变体样式下标；0xFFFF = 该状态与基础样式完全一致（不覆盖） */
    uint16_t style_hover;
    uint16_t style_active;
    uint16_t style_focus;
} QLUINode;

typedef struct {
    const char       *name;
    uint16_t          node_count;
    uint16_t          style_count;
    uint16_t          text_count;
    uint16_t          font_size_count;
    const QLUINode   *nodes;
    const QLUIStyle  *styles;
    const char       *text_pool;      /* 以 '\0' 分隔的字符串池 */
    const uint16_t   *text_offsets;   /* 文本池内偏移 */
    const uint16_t   *font_sizes;     /* 文档用到的字号（像素），font_id 索引它 */
} QLUIDoc;

/* ----- 运行期节点 ----- */

typedef struct QLUIRoot QLUIRoot;

typedef struct QLUINodeRT {
    struct QLUIRoot    *owner;      /* 用于解析状态变体样式 */
    struct QLUINodeRT *parent;
    struct QLUINodeRT *first_child;
    struct QLUINodeRT *next_sibling;

    const QLUINode  *src;
    const QLUIStyle *style;      /* 指向只读样式（或运行期改写的副本） */
    QLUIStyle        style_copy; /* 只有运行期改过样式时才使用 */

    const char      *text;

    QLRect           border_box; /* 边框盒的绝对矩形（布局结果） */
    QLRect           content_box;/* 内容盒的绝对矩形 */

    uint16_t         depth;
    uint8_t          flags;      /* QL_UI_RT_* */
    uint8_t          used_copy;  /* style_copy 是否启用 */
} QLUINodeRT;

enum {
    QL_UI_RT_HOVER   = 1u << 0,
    QL_UI_RT_ACTIVE  = 1u << 1,
    QL_UI_RT_DIRTY   = 1u << 2,
    QL_UI_RT_FOCUS   = 1u << 3,
};

/* 状态位 → 样式下标的映射（与生成器里的 PSEUDO_STATES 一致） */
#define QL_UI_STATE_BITS    3

typedef void (*ql_ui_click_cb)(QLUIRoot *ui, struct QLUINodeRT *node, void *user_data);

struct QLUIRoot {
    const QLUIDoc *doc;
    QLUINodeRT    *nodes;        /* 深度优先顺序，与 doc->nodes 一一对应 */
    uint16_t       node_count;
    ql_coord_t     width;
    ql_coord_t     height;
    ql_ui_click_cb click_cb;     /* 点击回调（可为 NULL） */
    void          *click_user_data;
    QLUINodeRT    *hover;        /* 悬停节点（驱动 :hover 样式） */
    QLUINodeRT    *focus;        /* 键盘焦点节点（驱动 :focus 样式） */
};

/* ----- 生命周期 ----- */
QLUIRoot   *ql_ui_build(const QLUIDoc *doc);
void        ql_ui_free(QLUIRoot *ui);

/* 按给定视口尺寸做一次完整布局（自上而下 + 自下而上算高度） */
void        ql_ui_layout(QLUIRoot *ui, ql_coord_t width, ql_coord_t height);

/* 绘制：背景 → 边框 → 文本，父先子后。d2d 可以是 CPU 或 GPU 后端 */
void        ql_ui_render(QLUIRoot *ui, QLDraw2D *d2d);
/* 便捷入口：内部包一层 CPU 接口 */
void        ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas);

/* ----- 查询 / 交互 ----- */
QLUINodeRT *ql_ui_root_node(QLUIRoot *ui);
QLUINodeRT *ql_ui_find_by_tag(QLUIRoot *ui, QLUITag tag, uint16_t nth);
QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
const char *ql_ui_node_text(const QLUINodeRT *node);
const QLFont *ql_ui_node_font(const QLUINodeRT *node);
/* 生效样式：ACTIVE > FOCUS > HOVER > 基础（只影响绘制，不影响布局） */
const QLUIStyle *ql_ui_node_style(const QLUINodeRT *node);
QLRect      ql_ui_node_rect(const QLUINodeRT *node);
const char *ql_ui_tag_name(QLUITag tag);

/* 运行期只允许改文本（结构在构建期固定，见 Web兼容设计 §12.5） */
ql_err_t    ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text);

/* 交互状态：命中测试 + 状态位 + 点击回调 */
void        ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data);
int         ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);

/*
 * 把 QLUIRoot 接到输入系统上：之后主循环只需要
 *     ql_input_dispatch();
 * 指针事件就会转成 hover / active / :focus 状态并触发 click_cb。
 * 同一时刻只支持一个 QLUIRoot（与"单一窗口"一致）。
 */
void        ql_ui_attach_input(QLUIRoot *ui);
void        ql_ui_detach_input(void);

/*
 * 单条事件映射。需要自己再处理按键（Esc 等）时，装自己的 sink：
 *     static void sink(const QLInputEvent *ev, void *user) {
 *         if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_ESCAPE) { ...; return; }
 *         ql_ui_input_event((QLUIRoot *)user, ev);
 *     }
 *     ql_input_set_sink(sink, ui);
 */
void        ql_ui_input_event(QLUIRoot *ui, const QLInputEvent *ev);
/* 键盘焦点：驱动 :focus 样式；传 NULL 清除 */
void        ql_ui_set_focus_node(QLUIRoot *ui, QLUINodeRT *node);
QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui);

#endif /* QLGL_UI_H */
