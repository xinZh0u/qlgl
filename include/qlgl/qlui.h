/**
 * @file    qlui.h
 * @brief   HTML + CSS 构建期编译结果（QLUIDoc）与运行期物化/布局/绘制。
 *
 * 三形态，产出的都是同一个 QLUIDoc，区别只在"字节放在哪、什么时候解析"：
 *
 *   ① 静态 C 数据（零加载成本，进 .rodata）：
 *        index.html + index.css ──qltext (tools/qltext)──▶ ui_index.c/.h ──编译器──▶ .rodata
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

/*
 * 标签。
 *
 * 标签在本架构里只影响三件事：① UA 默认样式（相当于浏览器内置样式表）；
 * ② `tag` 选择器；③ 调试/查询。布局与绘制只看编译出来的 QLUIStructStyle，
 * 不看标签——所以加标签几乎零成本。
 *
 * 顺序必须与 qlui_parse.c 的 `k_tags[]` 一致（下标就是枚举值）。
 * 只能在末尾追加：node.tag 是存进 blob 的数字。
 */
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
    /* --- 标题的其余三级 --- */
    QL_UI_TAG_H4,
    QL_UI_TAG_H5,
    QL_UI_TAG_H6,
    /* --- 文档结构：UA 默认样式就是 display:block，纯语义 --- */
    QL_UI_TAG_HEADER,
    QL_UI_TAG_FOOTER,
    QL_UI_TAG_NAV,
    QL_UI_TAG_MAIN,
    QL_UI_TAG_ARTICLE,
    QL_UI_TAG_ASIDE,
    QL_UI_TAG_FIGURE,
    QL_UI_TAG_FIGCAPTION,
    QL_UI_TAG_BLOCKQUOTE,
    /* --- 列表（注意：不画项目符号，本库没有生成内容，见文档） --- */
    QL_UI_TAG_UL,
    QL_UI_TAG_OL,
    QL_UI_TAG_LI,
    /* --- 两个空元素：<hr> 是块级细线，<br> 是"换一行" --- */
    QL_UI_TAG_HR,
    QL_UI_TAG_BR,
    /* --- 行内语义：UA 默认 display:inline；加粗/斜体/下划线要等文本样式支持 --- */
    QL_UI_TAG_MARK,
    QL_UI_TAG_EM,
    QL_UI_TAG_STRONG,
    QL_UI_TAG_U,
    QL_UI_TAG_S,
    /* --- 输入框 / 下拉框（运行期值会变，值是 ql_ui_build 时建的池） --- */
    QL_UI_TAG_INPUT,
    QL_UI_TAG_SELECT,
    QL_UI_TAG_OPTION,

    /*
     * --- 纯语义标签：UA 默认只有 block / inline，不引入任何新能力 ---
     *
     * 加它们是为了"真实 HTML 片段贴进来不报错"。它们各自的**语义**（链接、
     * 缩写、行内代码、编辑标记…）大多还没有实现，见 QLUI阅读指南 §5.4.7 的诚实清单。
     */
    /* 文档外壳：本库要求整个文档只有一个根元素，有这两个就能整段贴 */
    QL_UI_TAG_HTML,
    QL_UI_TAG_BODY,
    /* 分组：描述列表 / 菜单 / 联系信息 / 标题组 / 搜索区 */
    QL_UI_TAG_DL,
    QL_UI_TAG_DT,
    QL_UI_TAG_DD,
    QL_UI_TAG_MENU,
    QL_UI_TAG_ADDRESS,
    QL_UI_TAG_HGROUP,
    QL_UI_TAG_SEARCH,
    /* 行内语义：纯文字，没有字体变体（`code`/`kbd`/`samp` 的等宽字体还没有） */
    QL_UI_TAG_A,
    QL_UI_TAG_ABBR,
    QL_UI_TAG_CODE,
    QL_UI_TAG_KBD,
    QL_UI_TAG_SAMP,
    QL_UI_TAG_VAR,
    QL_UI_TAG_Q,
    QL_UI_TAG_DFN,
    QL_UI_TAG_CITE,
    QL_UI_TAG_DATA,
    QL_UI_TAG_TIME,
    QL_UI_TAG_SMALL,
    QL_UI_TAG_BDI,
    QL_UI_TAG_BDO,
    QL_UI_TAG_WBR,
    /* 编辑标记：装饰线已经有了，所以这两个能真的看出区别 */
    QL_UI_TAG_DEL,
    QL_UI_TAG_INS,
    /* 表单外壳：没有 <form> 语义（不提交、不校验），只是排版用的盒子 */
    QL_UI_TAG_FORM,
    QL_UI_TAG_LABEL,
    QL_UI_TAG_FIELDSET,
    QL_UI_TAG_LEGEND,
    QL_UI_TAG_OPTGROUP,

    /*
     * --- META 包：文档元数据。**不进树、不产生盒子** ---
     *
     * 加它们是为了"整页 HTML 直接贴进来能编译"：`<head>` / `<title>` / `<meta>` /
     * `<link>` / `<base>` / `<style>` 在解析期整段跳过（`<style>` 的内容连字都不进文本池）。
     * 所以这些枚举值**永远不会出现在 node.tag 里**，列在这里只为名字表与报错文案。
     */
    QL_UI_TAG_BASE,
    QL_UI_TAG_HEAD,
    QL_UI_TAG_LINK,
    QL_UI_TAG_META,
    QL_UI_TAG_STYLE,
    QL_UI_TAG_TITLE,

    /*
     * --- WIDGET 包：需要一点运行期状态或子节点行为的控件 ---
     *
     * 放在最后是为了守住"只能末尾追加"：前面每个枚举值都已上线（哪怕元数据
     * 那六个永远不进树），挪一下就可能让旧 blob 解释成别的标签。
     */
    QL_UI_TAG_OUTPUT,     /* 行内文本（表单结果），与 span 等价 */
    QL_UI_TAG_DATALIST,   /* 候选列表：display:none；`list=` 补全框还没做 */
    QL_UI_TAG_PROGRESS,   /* 进度条：value/max → 0..1000 的完成度 */
    QL_UI_TAG_METER,      /* 度量条：同上（low/high 分色还没做） */
    QL_UI_TAG_DETAILS,    /* 折叠面板：open 状态控制子节点可见性 */
    QL_UI_TAG_SUMMARY,    /* 折叠面板的标题行（点击它切换） */
    QL_UI_TAG_DIALOG,     /* 浮层：居中的模态框 + 遮罩 */
    QL_UI_TAG_IMG,        /* 位图：src 是资源名，见 doc/图像管线设计.md */
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
    int16_t     border_radius;  /* px；>= 短边一半时就是内切圆/胶囊 */
    uint8_t     text_align;    /* QLTextAlign */
    uint8_t     font_id;       /* doc->font_sizes 的下标（字号，像素） */
    uint8_t     box_sizing;    /* 0 = content-box, 1 = border-box */
    uint8_t     bg_grad_dir;   /* 0 = 纯色；1 = 垂直；2 = 水平；3 = 径向（= QLGradientDir+1） */
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

    /* ----- 状态过渡（transition） -----
     * 状态位（:hover/:active/:focus）切换时，这些属性在 trans_ms 内插值过去，
     * 而不是瞬间跳变。0 = 无过渡。位图见 QL_UI_TRANS_*。
     */
    uint16_t    trans_ms;
    uint8_t     trans_ease;    /* QLEasing */
    uint8_t     trans_props;   /* QL_UI_TRANS_* 位图 */

    /*
     * clip-path: circle()（子树圆形揭示）。
     *   clip_kind: 0 = 无，1 = 圆（圆心固定在元素中心）
     *   clip_r   : 半径，**千分比 of 最远角距离**（0..1000，1000 = 完全覆盖）
     * 用千分比而不是 px：动画 0 → 1000 视觉上一直有效（px 会"提前跑完"）。
     * 目前只支持一层（嵌套遮罩取交集，而两个圆盘的交集不是圆盘）。
     */
    uint8_t     clip_kind;
    uint16_t    clip_r;

    /* ----- transform（只有 translate，不进布局） -----
     * 绘制期把它加到绘制偏移上，所以移动元素**零重排**；
     * 代价是命中测试要做逆变换，paint_box 要按位移范围膨胀。
     */
    int16_t     tf_x;
    int16_t     tf_y;
    /*
     * transform: scale() / scaleX() / scaleY()，千分比（1000 = 1.0），绕元素中心。
     * 非等比缩放时圆角与文字按 min(sx, sy) 近似——见 QLUI阅读指南 §5.4.3。
     */
    int16_t     sc_x;
    int16_t     sc_y;

    /* ----- 关键帧动画（@keyframes） -----
     * doc->anims 的下标，0xFFFF = 没有动画。动画在 ql_ui_build 时起播，
     * 之后与状态位无关（:hover 不会打断它）。
     */
    uint16_t    anim;

    /*
     * backdrop-filter: blur(Npx)：把这层元素**背后**已经画好的内容模糊掉。
     * 0 = 关闭。想看见效果，元素自己的 background-color 必须带 alpha（否则糊了也盖住）。
     * PC/GL 走半分辨率两趟可分离模糊；CPU 后端就地模糊画布（同一条像素路径）。
     */
    uint8_t     backdrop_blur;

    /*
     * line-height（px）。0 = `normal`，用字体自己的 line_height（= ascent + descent）。
     *
     * 这是**单行文字垂直居中**的唯一正解（水平方向 counterpart 是 text-align）：
     * 半行距 = (line_h - (ascent + descent)) / 2，第一行基线再往下挪这么点。
     * 所以 `height: 26px; line-height: 26px` 就是完美居中。
     * 不给值时半行距恒为 0，与旧版本逐像素一致。
     */
    int16_t     line_h;

    /*
     * white-space：0 = `nowrap`（默认，本库与浏览器的**故意差异**，见下），
     *              1 = `normal`（按内容盒宽度折行）。
     *
     * 为什么默认是 nowrap：本库的 v0 文本模型是"一段文本就是一行"，
     * 大量已有界面（含 golden 基准）都建立在这个前提上。改成浏览器默认的
     * `normal` 会让那些页面在你没改任何东西的情况下重新排版。
     * 想要折行就显式写 `white-space: normal` —— 一行 CSS 换一个明确的语义。
     */
    uint8_t     white_space;

    /*
     * 字体变体 + 装饰线（`font-weight` / `font-style` / `text-decoration`）的位图，
     * 见 QL_UI_FONT_*。都是**合成**的：不加载第二个字体文件，加粗=画两遍偏移 1px、
     * 斜体=按行剪切、装饰线=画一条 1px 的横线。MCU 上也一样。
     */
    uint8_t     font_flags;   /* 低 4 位 = QL_UI_FONT_*，高 4 位 = object-fit */
} QLUIStyle;

/* 字体变体与装饰线：QLUIStyle.font_flags 的位 */
#define QL_UI_FONT_BOLD           0x01u   /* font-weight >= 600：合成加粗 */
#define QL_UI_FONT_ITALIC         0x02u   /* font-style: italic：合成斜体 */
#define QL_UI_FONT_UNDERLINE      0x04u   /* text-decoration: underline */
#define QL_UI_FONT_LINE_THROUGH   0x08u   /* text-decoration: line-through */

/*
 * `object-fit`（只有 `<img>` 有意义）。为了**不撑大 QLUIStyle**（120 字节的预算），
 * 它和字体位挤在同一个字节里：低 4 位是 QL_UI_FONT_*，高 4 位是这里。
 * 读的时候一律用下面的访问器，别手写移位。
 */
#define QL_UI_FIT_SHIFT   4
#define QL_UI_FIT_MASK    0x70u
#define QL_UI_FIT_FILL    0u   /* 拉伸（浏览器默认） */
#define QL_UI_FIT_CONTAIN 1u   /* 等比装进去 */
#define QL_UI_FIT_COVER   2u   /* 等比盖满，多的裁掉 */
#define QL_UI_FIT_NONE    3u   /* 原始像素尺寸，居中，超出部分裁掉 */
#define QL_UI_FIT_SCALE_DOWN 4u /* contain 与 none 取小的那个 */

static inline uint8_t ql_ui_style_object_fit(const QLUIStyle *s)
{
    return (uint8_t)((s->font_flags & QL_UI_FIT_MASK) >> QL_UI_FIT_SHIFT);
}

/* 能过渡的属性（"transition: all" = 四个都开） */
#define QL_UI_TRANS_BG      0x01u   /* background-color（渐变时连同终止色） */
#define QL_UI_TRANS_ALPHA   0x02u   /* opacity（落在 bg_alpha 上） */
#define QL_UI_TRANS_TEXT    0x04u   /* color */
#define QL_UI_TRANS_BORDER  0x08u   /* border-color */
#define QL_UI_TRANS_RADIUS  0x10u   /* border-radius（圆角/圆形揭示） */
#define QL_UI_TRANS_XFORM   0x20u   /* transform：translate + scale（一起动，拆开没有意义） */
#define QL_UI_TRANS_CLIP    0x40u   /* clip-path 半径（圆形揭示） */
#define QL_UI_TRANS_ALL     0x7Fu

/* ----- 关键帧动画（@keyframes）的静态表 ----- */

/* 关键帧里存了哪些通道 */
#define QL_UI_KF_BG         0x01u   /* bg_color + bg_color2 */
#define QL_UI_KF_ALPHA      0x02u   /* bg_alpha */
#define QL_UI_KF_MOVE       0x04u   /* transform 通道：tf_x/tf_y（translate）+ sc（scale） */
#define QL_UI_KF_TEXT       0x08u   /* text_color */
#define QL_UI_KF_BORDER     0x10u   /* border_color */
#define QL_UI_KF_RADIUS     0x20u   /* border_radius */
#define QL_UI_KF_CLIP       0x40u   /* clip_r（圆形揭示） */
/*
 * 本帧不存值，全部取"节点的基础样式"。
 * 生成器在 CSS 没写 0%/100% 时合成这样一帧（`@keyframes x { 50% { opacity: 0 } }`
 * 等于 0%/100% 用元素自己的基础值），所以它只能出现在首帧或末帧。
 */
#define QL_UI_KF_BASE       0x80u

/* 关键帧没写 animation-timing-function：这一段用动画自己的默认缓动 */
#define QL_UI_KF_EASE_INHERIT  0xFFu

typedef enum {
    QL_UI_ANIM_NORMAL = 0,
    QL_UI_ANIM_REVERSE,
    QL_UI_ANIM_ALTERNATE,
    QL_UI_ANIM_ALTERNATE_REVERSE,
} QLUIAnimDirection;

typedef enum {
    QL_UI_ANIM_FILL_NONE = 0,
    QL_UI_ANIM_FILL_FORWARDS,   /* 播完停在末帧 */
    QL_UI_ANIM_FILL_BACKWARDS,  /* 无 delay → 等价于 NONE（保留值以对齐 CSS） */
    QL_UI_ANIM_FILL_BOTH,
} QLUIAnimFill;

/* 一个时刻的属性快照。offset 是千分比（0..1000），对应 CSS 的 0%~100% */
typedef struct {
    uint16_t offset;
    uint8_t  ease;        /* 从本帧到下一帧的缓动（末帧忽略） */
    uint8_t  props;       /* QL_UI_KF_* 位图 */
    uint32_t bg_color;
    uint32_t bg_color2;
    uint32_t text_color;
    uint32_t border_color;
    int16_t  tf_x;        /* transform: translate 的像素偏移 */
    int16_t  tf_y;
    int16_t  sc_x;        /* transform: scale，千分比（负 = 用基础值） */
    int16_t  sc_y;
    int16_t  radius;      /* border-radius（负 = 用基础值） */
    int16_t  clip_r;      /* clip-path 千分比（负 = 用基础值） */
    uint8_t  bg_alpha;
    uint8_t  reserved;
} QLUIKeyframe;           /* 36 字节，blob 里顺序存放 */

/* 一段 @keyframes：帧表 + 播放参数。运行期只读 */
typedef struct {
    uint16_t first_frame; /* doc->keyframes 下标 */
    uint16_t frame_count; /* >= 2 */
    uint16_t dur_ms;      /* 一轮的时长；0 = 立即结束（等价于一帧） */
    uint8_t  ease;        /* 默认缓动；帧上有 animation-timing-function 时以帧为准 */
    uint8_t  iterations;  /* 0 = infinite */
    uint8_t  direction;   /* QLUIAnimDirection */
    uint8_t  fill;        /* QLUIAnimFill */
    uint16_t delay_ms;    /* animation-delay：起播前干等多久 */
} QLUIAnimation;

/* QLUINode.flags 的位 */
#define QL_UI_NODE_PASSWORD  0x01u   /* <input type="password">：显示成圆点 */
#define QL_UI_NODE_OPEN      0x02u   /* <details open>：展开状态 */

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
    uint8_t  flags;        /* QL_UI_NODE_* */
    uint8_t  reserved;
} QLUINode;

/* ----- 图像资源（doc 里的一条） ----- */
typedef struct {
    uint16_t name;          /* text 池下标：资源名（"logo.png"），与 <img src> 对照 */
    uint32_t off;           /* 字节在 image_data 段内的偏移 */
    uint32_t len;           /* 字节数 */
    uint16_t w, h;          /* 原始像素尺寸（RAW 时必填；其余格式可由解码得出） */
    uint8_t  format;        /* QLImageSrcFormat */
    uint8_t  pixel_format;  /* QLImageFormat，仅 format == RAW 时有效 */
} QLUIImageSrc;

typedef struct {
    const char       *name;
    uint16_t          node_count;
    uint16_t          style_count;
    uint16_t          text_count;
    uint16_t          font_size_count;
    uint16_t          anim_count;      /* @keyframes 段数；没有动画时为 0 */
    uint16_t          keyframe_count;  /* 所有段的关键帧总数 */
    /*
     * 被降级/裁掉的标签个数（`QL_CFG_UI_LENIENT_TAGS=1` 或某个能力包关掉时会计数）。
     * **不写进 blob**：加载出来的 doc 这个字段恒为 0，它只描述"这份 doc 是被解析出来的"。
     */
    uint16_t          degraded_tags;
    uint16_t          image_count;      /* 图像资源条数 */
    uint32_t          image_data_size;  /* 资源字节段的总长度 */
    const QLUIImageSrc *images;         /* image_count 条 */
    const uint8_t     *image_data;      /* 所有资源的字节（RAW 的像素也在这里） */
    const QLUINode   *nodes;
    const QLUIStyle  *styles;
    const char       *text_pool;      /* 以 '\0' 分隔的字符串池 */
    const uint16_t   *text_offsets;   /* 文本池内偏移 */
    const uint16_t   *font_sizes;     /* 文档用到的字号（像素），font_id 索引它 */
    const QLUIAnimation *anims;       /* anim_count 条 */
    const QLUIKeyframe  *keyframes;   /* keyframe_count 帧 */
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

    /*
     * transform 的绝对偏移（含祖先的），以及**动画可能带来的额外位移范围**。
     * 只在布局末尾算一次，供 paint_box 取上界用：绘制时直接按生效样式加偏移。
     */
    int16_t          tf_abs_x;
    int16_t          tf_abs_y;
    int16_t          tf_dev_lo_x;
    int16_t          tf_dev_lo_y;
    int16_t          tf_dev_hi_x;
    int16_t          tf_dev_hi_y;

    /*
     * 滚动（只有 `overflow: auto` 的容器会用）：
     *   scroll_x/y    当前偏移（>= 0）；布局末尾已经把子树整体平移过，
     *                 所以绘制/命中测试都不用再管它；
     *   scroll_max_x/y 可滚动范围（内容尺寸 - 可视尺寸，>= 0）。
     * 改滚动 = 改这两个数 + 重跑布局（0.04ms 量级，比维护一套绘制偏移便宜）。
     */
    ql_coord_t       scroll_x;
    ql_coord_t       scroll_y;
    ql_coord_t       scroll_max_x;
    ql_coord_t       scroll_max_y;

    uint16_t         depth;
    uint8_t          flags;      /* QL_UI_RT_* */
    uint8_t          used_copy;  /* style_copy 是否启用 */
} QLUINodeRT;

enum {
    QL_UI_RT_HOVER   = 1u << 0,
    QL_UI_RT_ACTIVE  = 1u << 1,
    QL_UI_RT_DIRTY   = 1u << 2,
    QL_UI_RT_FOCUS   = 1u << 3,
    QL_UI_RT_OPEN    = 1u << 4,   /* <details> 当前的展开状态（从 QL_UI_NODE_OPEN 初始化） */
};

/*
 * 推进状态过渡（每帧调用一次）：
 *     ql_ui_layout(...) 之后，渲染之前
 *     if (ql_ui_anim_tick(ui, ql_anim_now_ms()) > 0) { 重绘 }
 * 返回仍在过渡中的节点数；0 表示这一帧没有东西因为动画而改变
 * （静态界面下应当恒为 0，主循环可据此整帧跳过）。
 */
uint32_t    ql_ui_anim_tick(QLUIRoot *ui, uint32_t now_ms);

/*
 * 从头重播某个节点的 @keyframes 动画（点击反馈、出错抖动之类）。
 * 该节点没有动画时返回 QL_ERR_INVAL。起始时刻取下一次 ql_ui_anim_tick 的 now。
 */
ql_err_t    ql_ui_anim_restart(QLUIRoot *ui, QLUINodeRT *node);

/* 正在跑的动画数：过渡 + 还在走的关键帧动画（播完停在末帧的不算） */
uint32_t    ql_ui_anim_running(const QLUIRoot *ui);

/*
 * 动画结束回调（等价于 CSS 的 transitionend / animationend）。
 *
 * 什么时候触发：
 *   - `@keyframes` 播完（含 infinite 之外的最后一帧，无论 fill 是 forwards 还是 none）
 *   - `transition` 走到终点
 *   - 被 `ql_ui_anim_restart()` 打断的**不会**触发（那是新的开始，不是结束）
 *   - 被状态变化撤掉（目标样式没有过渡）的也**不会**触发
 *
 * 回调在 `ql_ui_anim_tick()` 内部被派发，但**已经排到队列尾部**：
 * 在回调里 `ql_ui_anim_restart()` / 改文本 / 甚至 `ql_ui_free()` 都是安全的
 * （后者请先 dtach，见 ql_ui_free 的说明）。队列是有界的
 * （QL_CFG_UI_ANIM_EVENT_MAX），满了会丢事件并告警一次——不做无界排队。
 */
typedef enum {
    QL_UI_ANIM_END_KEYFRAMES = 0,   /* @keyframes 播完 */
    QL_UI_ANIM_END_TRANSITION,      /* transition 走到终点 */
} QLUIAnimEndKind;

typedef void (*ql_ui_anim_end_cb)(QLUIRoot *ui, QLUINodeRT *node,
                                  QLUIAnimEndKind kind, void *user_data);

void        ql_ui_set_anim_end_cb(QLUIRoot *ui, ql_ui_anim_end_cb cb, void *user_data);

/*
 * 取走"因过渡而脏"的区域，并把脏标记清掉：
 *     QLRect areas[8]; int n = ql_ui_consume_dirty(ui, areas, 8);
 *     for (i < n) ql_ui_render_rect(ui, d2d, areas[i]);
 * 返回写入的区域数（最多 max 个，超出的并进最后一块）。
 */
int         ql_ui_consume_dirty(QLUIRoot *ui, QLRect *out, int max);

/* 状态位 → 样式下标的映射（与生成器里的 PSEUDO_STATES 一致） */
#define QL_UI_STATE_BITS    3

/*
 * 同时在跑的"状态过渡"上限。
 * 一个节点一个槽（同节点的多次打断会重基，不占新槽）。
 */
#ifndef QL_CFG_UI_TRANS_MAX
#define QL_CFG_UI_TRANS_MAX 16
#endif

/*
 * 过渡槽。
 *
 * 插值的是**整份样式**：`from` → `to` 按进度线性混合选中的通道，
 * 结果写进 `style`（绘制时用它）。`key.cur.v[0]` 只当 0..1 的进度用，
 * 所以通道数不受 QLAnimValue 的 4 个 float 限制。
 */
typedef struct {
    struct QLUINodeRT *node;
    QLUIStyle          from;    /* 起点：状态变化前屏幕上显示的那份 */
    QLUIStyle          to;      /* 终点：状态变体样式 */
    QLUIStyle          style;   /* 当前插值结果 */
    QLAnimKey          key;     /* 只用 v[0] 当进度 */
    uint8_t            active;
    uint8_t            props;   /* QL_UI_TRANS_* */
} QLUITrans;

/*
 * 同时播放的关键帧动画上限（每节点一个槽）。
 * 只在 doc 里真的有 @keyframes 时才分配这块内存。
 */
#ifndef QL_CFG_UI_RUN_ANIM_MAX
#define QL_CFG_UI_RUN_ANIM_MAX 16
#endif

/* 一次 tick 里最多派发多少个"动画结束"事件（超出丢事件并告警） */
#ifndef QL_CFG_UI_ANIM_EVENT_MAX
#define QL_CFG_UI_ANIM_EVENT_MAX 16
#endif

/* 一条正在播的关键帧动画 */
typedef struct {
    struct QLUINodeRT   *node;
    const QLUIAnimation *def;    /* doc->anims 里的一条 */
    QLUIStyle            style;  /* 当前帧的生效样式（基础样式 + 动画覆盖） */
    uint32_t             start_ms;
    uint8_t              active;
    uint8_t              started; /* 0 = 还没拿到起始时刻（第一次 tick 时定） */
    uint8_t              held;    /* 1 = 已播完并停在末帧（fill: forwards） */
    uint8_t              phase;   /* 0 = 未起播，1 = 正在 animation-delay 里，2 = 播放中 */
} QLUIAnimRun;

typedef void (*ql_ui_click_cb)(QLUIRoot *ui, struct QLUINodeRT *node, void *user_data);

/* 输入框的值变化 / 回车回调（定义在这里是因为 QLUIRoot 要存函数指针） */
typedef void (*ql_ui_input_cb)(QLUIRoot *ui, QLUINodeRT *node, const char *value,
                               void *user);

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
    /* 状态过渡池：只在本 doc 有 transition 时才分配（NULL = 完全没有过渡） */
    QLUITrans     *trans;
    /* 关键帧动画池：只在本 doc 有 @keyframes 时才分配 */
    QLUIAnimRun   *runs;
    ql_ui_anim_end_cb anim_end_cb;
    void             *anim_end_user;
    /* 待派发的结束事件（有界；满了丢新的并告警） */
    struct QLUIAnimEvent { struct QLUINodeRT *node; uint8_t kind; } anim_ev[QL_CFG_UI_ANIM_EVENT_MAX];
    uint8_t        anim_ev_count;
    uint8_t        anim_ev_dropped;
    /* 正在拖滚动条：容器 + 按下时的指针位置与当时的偏移 */
    QLUINodeRT    *scroll_drag;
    ql_coord_t     scroll_drag_y0;
    ql_coord_t     scroll_drag_scroll0;

    /* 输入框池：doc 里没有 <input> 时为 NULL（零字节） */
    struct QLUIInput *inputs;      /* 输入框池（没有 <input> 时为 NULL） */
    uint16_t       input_count;
    uint16_t       dialog_count;
    struct QLUIImageRT *images;        /* 图像池（没有 <img> 时为 NULL） */
    uint16_t       image_count;       /* doc 里 <dialog> 的个数（0 时可以跳过整段浮层逻辑） */
    struct QLUIProgress *progresses;   /* 进度条池（没有 progress/meter 时为 NULL） */
    uint16_t       progress_count;
    struct QLUISelectState *selects;   /* 下拉框池（没有 <select> 时为 NULL） */
    uint16_t       select_count;
    struct QLUISelectState *open_select;   /* 当前展开的那个下拉框 */
    int            input_drag;       /* 正在拖动选字 */
    ql_ui_input_cb input_cb;
    void          *input_user;
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
const char *ql_ui_tag_name(uint16_t tag);

/* 运行期只允许改文本（结构在构建期固定，见 Web兼容设计 §12.5） */
ql_err_t    ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text);

/* 交互状态：命中测试 + 状态位 + 点击回调 */
void        ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data);
/* ------------------------------ 输入框 ------------------------------ */

/*
 * `<input>` 的值存在**运行期池**里（`ql_ui_build` 时按需分配），长度上限
 * `QL_CFG_UI_INPUT_LEN - 1` 字节（UTF-8）。这是本库唯一一处"运行期会变的文本"。
 *
 * 编辑：点击定位光标、拖动选字、Backspace/Delete/方向键/Home/End、
 * Shift+方向扩展选区、`QL_INPUT_TEXT` 插入字符。**没有剪贴板**（平台层不提供），
 * 光标也**不闪**（闪烁要主循环一直醒着，省电优先）。
 */
/* 值变化 / 按回车时回调；cb 为 NULL 取消 */
void        ql_ui_set_input_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
/* 当前值；node 不是输入框返回 NULL。指针在下次编辑前有效。 */
const char *ql_ui_input_value(const QLUIRoot *ui, const QLUINodeRT *node);
/* 改值（会被夹到上限；光标移到末尾） */
ql_err_t    ql_ui_input_set_value(QLUIRoot *ui, QLUINodeRT *node, const char *value);

/* ---------------------------- 进度条 / 度量条 ---------------------------- */

/*
 * `<progress value="70" max="100">` 与 `<meter>`：内部只存**完成度**（0..1000），
 * 不存 value/max —— 布局与绘制都只需要"多满"。
 *
 * 没有 `value` 属性 = 不确定进度：画一个 1/3 宽的固定条（**不做动画**：
 * 会跑的进度条要求主循环一直醒着，省电优先）。
 *
 * 轨道 = `background-color`，填充 = `color`。所以换颜色只写 CSS，不用碰 API。
 */
/* 0..1000 的完成度；-1 = 不是 progress/meter，-2 = 没有 value（不确定） */
int         ql_ui_progress_permille(const QLUIRoot *ui, const QLUINodeRT *node);
/* 按 value/max 设置（夹到 [0, max]；max <= 0 视为 100） */
ql_err_t    ql_ui_progress_set(QLUIRoot *ui, QLUINodeRT *node, int value, int max);

/* -------------------------------- 浮层 -------------------------------- */

/*
 * `<dialog>`：**真正的浮层** —— 居中、压暗背景、点外面或按 `Esc` 关闭。
 *
 * 画法：先把整棵树按正常顺序画完，再在最后（与下拉面板同一层）画一遍遮罩 +
 * 对话框子树。所以浮层永远在最上面，代价是那个子树每帧多画一次（只有开着才有）。
 *
 * 布局：`ql_ui_layout` 末尾把已打开的对话框平移到视口中央（平移子树，与滚动同一招）。
 * 已知限制：不做焦点圈定（Tab 不会被困在框内）、不支持嵌套模态、宽度按 UA 的 `70%`。
 */
/* 1 = 打开，0 = 关闭；不是 <dialog> 返回 -1 */
int         ql_ui_dialog_open(const QLUIRoot *ui, const QLUINodeRT *node);
/* 打开/关闭（内部会重排，返回脏区由调用方按 paint_bounds 处理） */
ql_err_t    ql_ui_dialog_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);
/* 关掉最上层那个（没有打开的返回 0） */
int         ql_ui_dialog_close_top(QLUIRoot *ui);

/* -------------------------------- 图像 -------------------------------- */

/*
 * `<img src="logo.png">`：`src` 只是**资源名**，字节从两处来：
 *   1. doc 里（构建期 `qltext assets` 转好 / `.qlui` blob 带进来）；
 *   2. 运行期 `ql_ui_register_image()`（PC 读文件、网络图）。
 *
 * 布局只需要 `w/h`，所以 `w/h` 在 doc 里就有 —— 图没解出来也占对位置。
 * 解码只在 build / register 时发生一次，**每帧只搬像素**。
 */
typedef struct {
    const char    *name;         /* 对应 <img src> 的名字 */
    const uint8_t *data;
    size_t         len;
    uint16_t       w, h;         /* RAW 必填；其他格式填 0 = 以解码结果为准 */
    uint8_t        format;       /* QLImageSrcFormat；AUTO = 按魔数探测 */
    uint8_t        pixel_format; /* RAW 时必填（QLImageFormat） */
} QLUIImageInput;

/* 注册/替换一张图（同名覆盖）。会重排（尺寸可能变了）。 */
ql_err_t    ql_ui_register_image(QLUIRoot *ui, const QLUIImageInput *in);
/* 节点当前的解码结果；没有/失败返回 NULL，不是 <img> 返回 NULL */
const QLImage *ql_ui_node_image(const QLUIRoot *ui, const QLUINodeRT *node);

/* ------------------------------ 折叠面板 ------------------------------ */

/*
 * `<details>` / `<summary>`：点标题行展开或收起。
 *
 * 收起时把非 `<summary>` 的子节点指向一份**共享的隐藏样式**（与"只含空白的
 * flex 项"用的是同一份 `s_style_hidden`）—— 零额外内存，也不用改任何布局循环。
 * 切换**会重排**（高度变了），所以 `ql_ui_details_set_open()` 内部直接调 `ql_ui_layout`。
 */
/* 1 = 展开，0 = 收起；不是 <details> 返回 -1 */
int         ql_ui_details_open(const QLUIRoot *ui, const QLUINodeRT *node);
/* 设置展开状态（会重排；状态没变时是空操作） */
ql_err_t    ql_ui_details_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);

/* ------------------------------ 下拉框 ------------------------------ */

/*
 * `<select>` + `<option>`：点击展开一个**覆盖式**面板（不进布局、不占空间），
 * 选中项的文字就是这个 select 显示的文字。
 *
 * 面板是最后一层直接画在视口上的，**不受父级 overflow 裁剪** —— 与浏览器的
 * popup 行为一致。简单实现：同一时刻只有一个下拉框能展开。
 */
void        ql_ui_set_select_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
/* 当前选中项下标；不是 <select> 返回 -1 */
int         ql_ui_select_index(const QLUIRoot *ui, const QLUINodeRT *node);
/* 设置选中项（越界夹紧） */
ql_err_t    ql_ui_select_set_index(QLUIRoot *ui, QLUINodeRT *node, int index);

/*
 * 滚动（`overflow: auto`）。
 *
 * 偏移会被夹到 [0, scroll_max]，返回实际生效的位置。节点不可滚动时是空操作。
 * `ql_ui_scroll_to` 之后需要重跑一次 `ql_ui_layout`（见 QLUI阅读指南 §5.6），
 * 或者干脆用 `ql_ui_scroll_by` —— 它内部会重排并返回脏区。
 */
ql_err_t    ql_ui_scroll_to(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x, ql_coord_t y);
ql_err_t    ql_ui_scroll_by(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy);
/* 这个节点能不能滚（overflow: auto 且有超出内容） */
int         ql_ui_scrollable(const QLUINodeRT *node);

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
 *   [ anims      ] anim_count      * sizeof(QLUIAnimation)
 *   [ keyframes  ] keyframe_count  * sizeof(QLUIKeyframe)
 *   [ name       ] name_len 字节（不含 '\0'）
 *
 * 之所以能"直接绑指针"，是因为 doc 内部全是 uint16 下标。
 * 但结构体内存布局受编译器/ABI 影响，所以 blob 不是跨架构可移植的：
 * 头里的 struct_size / abi 字段会在加载时逐项校验，不匹配就明确拒绝。
 */
#define QL_UI_BLOB_MAGIC      0x49554C51u   /* 'Q','L','U','I'（小端） */
#define QL_UI_BLOB_VERSION    13u  /* 13: 图像资源段（images / image_data） */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;         /* 固定 72 */
    uint16_t node_count;
    uint16_t style_count;
    uint16_t text_count;
    uint16_t font_size_count;
    uint16_t anim_count;          /* @keyframes 段数 */
    uint16_t keyframe_count;      /* 关键帧总数 */
    uint16_t image_count;         /* 图像资源条数 */
    uint16_t name_len;            /* 不含结尾 '\0' */
    uint16_t node_struct_size;    /* sizeof(QLUINode)      —— ABI 校验 */
    uint16_t style_struct_size;   /* sizeof(QLUIStyle)     —— ABI 校验 */
    uint16_t anim_struct_size;    /* sizeof(QLUIAnimation) —— ABI 校验 */
    uint16_t keyframe_struct_size;/* sizeof(QLUIKeyframe)  —— ABI 校验 */
    uint32_t total_size;          /* 整个 blob 的字节数 */
    uint32_t checksum;            /* FNV-1a 32，覆盖 [header_size, total_size) */
    uint32_t off_nodes;
    uint32_t off_styles;
    uint32_t off_text_pool;
    uint32_t off_text_offsets;
    uint32_t off_font_sizes;
    uint32_t off_anims;
    uint32_t off_keyframes;
    uint32_t off_images;          /* QLUIImageSrc[] */
    uint32_t off_image_data;      /* 资源字节 */
    uint32_t image_data_size;
    uint32_t off_name;
} QLUIDocBlobHeader;

_Static_assert(sizeof(QLUIDocBlobHeader) == 84, "blob 头必须是 84 字节（v13）");
_Static_assert(sizeof(QLUIKeyframe) == 36, "关键帧必须是 36 字节（blob 顺序存放）");

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
