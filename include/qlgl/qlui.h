/**
 * @file    qlui.h
 * @brief   HTML + CSS 构建期编译结果（QLUIDoc）与运行期物化/布局/绘制。
 *
 * 三形态，产出的都是同一个 QLUIDoc，区别只在"字节放在哪、什么时候解析"：
 *
 *   ① 静态 C 数据（零加载成本，进 .rodata）：
 *        index.html + index.css ──tools/qlui/qlui.py──▶ ui_index.c/.h ──编译器──▶ .rodata
 *        QLUIRoot *ui = ql_ui_build(&ui_index_doc);
 *
 *   ② 二进制 blob（改 UI 不用重编译；加载只是校验 + 设基址）：
 *        QLUIRoot *ui = ql_ui_load_file("ui/index.qlui");
 *
 *   ③ 运行时文本（目标机上解析，见 M3.5；这里只是预留入口）
 *
 * 三者渲染开销完全相同（都走 QLUINodeRT → 布局 → 绘制），运行期不做 CSS 匹配。
 * 关键：所有内部引用都是数组下标（uint16），没有指针，所以 blob 的"加载"不需要
 * 递归重定位——只要校验版本/长度并设好 QLUIDoc 的几个基址。
 *
 * ⚠ doc 之间不能互相引用合成一棵树：parent/first_child/next_sibling 是同一份
 *   数组内的下标。混用的单位是"面板"（一个 QLUIRoot ↔ 一份 doc）。
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
    QL_UI_DISPLAY_FLEX,
    QL_UI_DISPLAY_INLINE_FLEX,
} QLUIDisplay;

typedef enum {
    QL_UI_FLEX_ROW = 0,
    QL_UI_FLEX_ROW_REVERSE,
    QL_UI_FLEX_COLUMN,
    QL_UI_FLEX_COLUMN_REVERSE,
} QLUIDirection;

typedef enum {
    QL_UI_JUSTIFY_START = 0,
    QL_UI_JUSTIFY_CENTER,
    QL_UI_JUSTIFY_END,
    QL_UI_JUSTIFY_SPACE_BETWEEN,
    QL_UI_JUSTIFY_SPACE_AROUND,
    QL_UI_JUSTIFY_SPACE_EVENLY,
} QLUIJustify;

typedef enum {
    QL_UI_ALIGN_STRETCH = 0,
    QL_UI_ALIGN_START,
    QL_UI_ALIGN_CENTER,
    QL_UI_ALIGN_END,
} QLUIAlign;

typedef enum {
    QL_UI_POSITION_STATIC = 0,
    QL_UI_POSITION_RELATIVE,
    QL_UI_POSITION_ABSOLUTE,
    QL_UI_POSITION_FIXED,
} QLUIPosition;

typedef enum {
    QL_UI_OVERFLOW_VISIBLE = 0,
    QL_UI_OVERFLOW_HIDDEN,
    QL_UI_OVERFLOW_AUTO,
} QLUIOverflow;

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
    int16_t  value;        /* px 值（可为负，定位用），或百分比整数 */
    uint8_t  unit;         /* QLUIUnit */
    uint8_t  reserved;
} QLUILength;

typedef struct {
    uint32_t    bg_color;      /* 0xRRGGBB；alpha 见 bg_alpha */
    uint32_t    text_color;
    uint32_t    border_color;
    uint32_t    bg_color2;     /* 渐变终止色：只有 bg_grad_dir != 0 时有效 */
    uint32_t    shadow_color;

    QLUILength  width;
    QLUILength  height;

    QLUIEdges   margin;
    QLUIEdges   padding;

    int16_t     shadow_dx;     /* 阴影偏移 */
    int16_t     shadow_dy;

    uint8_t     display;       /* QLUIDisplay */
    uint8_t     bg_alpha;      /* 0..255 */
    uint8_t     border_width;
    uint8_t     border_radius;
    uint8_t     text_align;    /* QLTextAlign */
    uint8_t     font_id;       /* doc->font_sizes 的下标（字号，像素） */
    uint8_t     box_sizing;    /* 0 = content-box, 1 = border-box */
    uint8_t     bg_grad_dir;   /* 0 = 纯色；1 = 垂直；2 = 水平（见 QLGradientDir+1） */
    uint8_t     shadow_blur;   /* 0 = 硬阴影 */
    uint8_t     shadow_alpha;  /* 0 = 无阴影 */

    /* ----- flex 容器 ----- */
    uint8_t     flex_direction;  /* QLUIDirection */
    uint8_t     justify_content; /* QLUIJustify */
    uint8_t     align_items;     /* QLUIAlign */
    uint8_t     flex_wrap;       /* 0 = nowrap，1 = wrap */
    int16_t     gap_row;
    int16_t     gap_col;

    /* ----- flex 项 ----- */
    uint16_t    flex_grow;     /* 定点 /100：100 表示 flex-grow:1，默认 0 */
    uint16_t    flex_shrink;   /* 定点 /100：默认 100（即 flex-shrink:1） */
    int16_t     flex_basis;    /* -1 = auto（用 width）；>=0 = px */
    uint16_t    flex_basis_pct;/* 当 basis 是百分比时有效；0 = 不是百分比 */

    /* ----- 定位与裁剪 ----- */
    uint8_t     position;      /* QLUIPosition */
    uint8_t     overflow;      /* QLUIOverflow */
    int16_t     z_index;
    QLUILength  inset[4];      /* top / right / bottom / left（只有非 static 时用） */
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
    /*
     * 所有权标记：
     *   NULL  → 本 doc 是静态数据（.rodata 或调用方自己管），ql_ui_doc_free 不做任何事；
     *   非 NULL → 本 doc 由 ql_ui_doc_load* 分配，这块内存归 doc 所有。
     * 判据只有这一条，所以"静态 doc 误进 free 路径"不可能发生。
     */
    void             *arena;
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
    /*
     * 绝对定位子节点的"包含块"：最近的非 static 祖先的内容盒，
     * 没有的话就是视口。布局时自上而下传递。
     */
    QLRect           abs_cb;
    /*
     * 本节点 + 整个子树可能画到的范围（绝对坐标），布局末尾自下而上算出。
     *
     * 它是"上界"：自己的边框盒 ∪ 阴影外扩 ∪ 未裁剪子树的包围盒。
     * 绘制时只要 `paint_box` 与当前裁剪不相交，整棵子树就可以直接跳过——
     * 一个节点一次矩形相交判断，替代整棵子树的遍历。
     *
     * 状态变体样式（:hover/:active/:focus）也会参与取上界：
     * 伪类可以改 box-shadow / overflow，不做并集就会漏画。
     */
    QLRect           paint_box;

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
    uint8_t        owns_doc;     /* 1 = ql_ui_free 时一并释放 doc（ql_ui_load_file 用） */
    QLUINodeRT    *nodes;        /* 深度优先顺序，与 doc->nodes 一一对应 */
    uint16_t       node_count;
    ql_coord_t     width;
    ql_coord_t     height;
    ql_ui_click_cb click_cb;     /* 点击回调（可为 NULL） */
    void          *click_user_data;
    QLRect         root_clip;    /* 调用方给定的初始裁剪（渲染器与画布 clip 对齐用） */
    uint8_t        root_clip_valid;
    QLRect         clip;         /* 当前裁剪（render_node 进出 overflow 容器时保存/恢复） */
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

/*
 * 只重绘 dirty 区域（动画 / 局部刷新的入口）。
 *
 * 内部把 d2d 的裁剪设成 `dirty ∩ 视口`（再与之前 ql_ui_render_canvas 记下的
 * root_clip 求交），然后靠每个节点的 `paint_box` 做子树剪枝：
 * 包围盒不相交的整棵子树一次判断就跳过。
 *
 * 保证：**在 dirty 区域内的结果与全量 ql_ui_render 逐像素相同**（test_render 守着）。
 * 注意 dirty 之外的内容不会被重画，调用方负责那部分保持上一帧。
 */
void        ql_ui_render_rect(QLUIRoot *ui, QLDraw2D *d2d, QLRect dirty);
/* 跑完一次布局后，整棵树的绘制范围（= 根节点的 paint_box） */
QLRect      ql_ui_paint_bounds(const QLUIRoot *ui);

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

/* ===================== 二进制 blob（.qlui） ===================== */

/*
 * 格式（little-endian，各段按 8 字节对齐）：
 *
 *   [ QLUIDocBlobHeader 64 字节 ]
 *   [ nodes      ] node_count      * sizeof(QLUINode)
 *   [ styles     ] style_count     * sizeof(QLUIStyle)
 *   [ text_pool  ] 以 '\0' 分隔的 UTF-8 字节
 *   [ text_ofs   ] text_count      * uint16
 *   [ font_sizes ] font_size_count * uint16
 *   [ name       ] name_len 字节（不含 '\0'）
 *
 * 之所以能"直接绑指针"，是因为 doc 内部全是 uint16 下标。
 * 但结构体内存布局受编译器/ABI 影响，所以 blob 不是跨架构可移植的：
 * 头里的 struct_size / abi 字段会在加载时逐项校验，不匹配就明确拒绝。
 */
#define QL_UI_BLOB_MAGIC      0x49554C51u   /* 'Q','L','U','I'（小端） */
#define QL_UI_BLOB_VERSION    2u   /* 2: QLUIStyle 增加渐变与阴影字段 */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;      /* 固定 64 */
    uint16_t node_count;
    uint16_t style_count;
    uint16_t text_count;
    uint16_t font_size_count;
    uint16_t name_len;         /* 不含结尾 '\0' */
    uint16_t node_struct_size; /* sizeof(QLUINode)      —— ABI 校验 */
    uint16_t style_struct_size;/* sizeof(QLUIStyle)     —— ABI 校验 */
    uint32_t total_size;       /* 整个 blob 的字节数 */
    uint32_t checksum;         /* FNV-1a 32，覆盖 [header_size, total_size) */
    uint32_t off_nodes;
    uint32_t off_styles;
    uint32_t off_text_pool;
    uint32_t off_text_offsets;
    uint32_t off_font_sizes;
    uint32_t off_name;
    uint8_t  reserved[8];      /* 补齐到 64 */
} QLUIDocBlobHeader;

/*
 * 把 doc 编码成 blob。buf 为 NULL 或容量不足时只返回需要的字节数（不写）。
 * 返回值：需要的字节数（>0），或 0 表示 doc 非法。
 */
size_t ql_ui_doc_encode(const QLUIDoc *doc, void *buf, size_t cap);

/* 写文件；返回 QL_OK / QL_ERR_* */
ql_err_t ql_ui_doc_save(const QLUIDoc *doc, const char *path);

/*
 * 从内存加载。data 会被拷贝一份（调用方可以立刻释放自己的缓冲）。
 * 返回的 QLUIDoc 由 arena 持有，必须用 ql_ui_doc_free 释放。
 * 失败返回 NULL，原因可用 ql_ui_doc_last_error() 取（含字段名，便于排查 ABI 不匹配）。
 */
QLUIDoc *ql_ui_doc_load_mem(const void *data, size_t size);
QLUIDoc *ql_ui_doc_load(const char *path);
void     ql_ui_doc_free(QLUIDoc *doc);

/* 是否由 ql_ui_doc_load* 分配（等价于 doc != NULL && doc->arena != NULL） */
int      ql_ui_doc_is_owned(const QLUIDoc *doc);

/* 文本池的字节数（由最后一段字符串推出；没有文本时为 0） */
size_t   ql_ui_doc_text_pool_size(const QLUIDoc *doc);

/* 最近一次加载失败的原因（静态缓冲，线程不安全；成功时清空） */
const char *ql_ui_doc_last_error(void);

/*
 * 从 .qlui 文件直接构建并布局。返回的 root **持有**那份 doc，
 * ql_ui_free 会一并释放，调用方不需要再管 doc。
 */
QLUIRoot *ql_ui_load_file(const char *path);

#endif /* QLGL_UI_H */
