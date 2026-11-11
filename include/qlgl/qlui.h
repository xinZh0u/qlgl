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
#include "qlanim.h"   /* QLAnimKey：布局/绘制都直接用 */

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
    QL_UI_TAG_PICTURE,    /* 响应式图片容器：自身不画，子节点里的 <img> 照常排 */
    QL_UI_TAG_SOURCE,     /* <picture>/<video> 的候选源：空元素，完全不参与渲染 */
    QL_UI_TAG_MAP,        /* 图像热区表：display:none，只为 <area> 提供名字 */
    QL_UI_TAG_AREA,       /* 热区：空元素，几何存进 doc->areas（见热区 API） */

    /*
     * --- EMBED 包：**永远只做占位**，不做真渲染 ---
     *
     * 它们不是"还没做"，是按红线不做：
     *   - 没有 DOM / 没有 JS → script / noscript / template / slot
     *   - 外来内容无法有界（网络、第三方渲染器）→ iframe / embed / object
     *   - 要解码器 + 音频输出 + 同步 → audio / video / track
     *   - 要 JS 或"往 HTML 上画"的新 API 层 → canvas
     *   - 各自需要独立渲染器（等于写第二个 QLGL）→ svg / math
     *
     * 认下来是为了"整页 HTML 贴进来能编译"：这些标签不报错，
     * 替换型元素画一个带标签名的占位盒，元数据型 `display:none`。
     * 详见 doc/手册/ui/HTML5标签覆盖与裁剪.md §5。
     */
    QL_UI_TAG_IFRAME,     /* 占位盒 + 标签名 */
    QL_UI_TAG_EMBED,      /* 空元素，占位盒 */
    QL_UI_TAG_OBJECT,     /* 占位盒；子内容是兜底，不渲染 */
    QL_UI_TAG_AUDIO,      /* 占位盒（矮），子内容不渲染 */
    QL_UI_TAG_VIDEO,      /* 占位盒，子内容不渲染 */
    QL_UI_TAG_TRACK,      /* 空元素，display:none（字幕轨元数据） */
    QL_UI_TAG_CANVAS,     /* 占位盒；子内容是兜底，不渲染 */
    QL_UI_TAG_SCRIPT,     /* display:none；**内容整段跳过**（raw text） */
    QL_UI_TAG_NOSCRIPT,   /* display:none */
    QL_UI_TAG_SLOT,       /* display:none（没有 shadow DOM） */
    QL_UI_TAG_TEMPLATE,   /* display:none；**内容整段跳过**（惰性内容） */
    QL_UI_TAG_SVG,        /* 占位盒；**内容整段跳过**（非 HTML 子树） */
    QL_UI_TAG_MATH,       /* 占位盒；**内容整段跳过**（非 HTML 子树） */

    /* --- TEXT 包（P4）：空白保留与上下标 --- */
    QL_UI_TAG_PRE,        /* block + white-space: pre + 等宽字体：空白与换行全保留 */
    QL_UI_TAG_SUB,        /* 行内 + 下标（基线略降 + 字号缩小） */
    QL_UI_TAG_SUP,        /* 行内 + 上标（基线略升 + 字号缩小） */
    QL_UI_TAG_TEXTAREA,   /* 多行输入框（P5）：复用输入池的另一半 */

    /* --- B7 表格（`QL_FEAT_CSS_TABLE`）：结构标签自己不画，只定 display 角色 --- */
    QL_UI_TAG_TABLE,      /* display: table */
    QL_UI_TAG_CAPTION,    /* display: table-caption（表格上方那一行说明） */
    QL_UI_TAG_THEAD,      /* display: table-row-group */
    QL_UI_TAG_TBODY,      /* display: table-row-group */
    QL_UI_TAG_TFOOT,      /* display: table-row-group（与 thead/tbody 同义，见文档） */
    QL_UI_TAG_TR,         /* display: table-row */
    QL_UI_TAG_TD,         /* display: table-cell */
    QL_UI_TAG_TH,         /* display: table-cell（UA：加粗 + 居中） */
    QL_UI_TAG_COLGROUP,   /* display: none（列宽声明还没做，见文档） */
    QL_UI_TAG_COL,        /* display: none */
    QL_UI_TAG_COUNT
} QLUITag;


typedef enum {
    QL_UI_DISPLAY_BLOCK = 0,
    QL_UI_DISPLAY_INLINE,
    QL_UI_DISPLAY_INLINE_BLOCK,
    QL_UI_DISPLAY_NONE,
    QL_UI_DISPLAY_FLEX,
    QL_UI_DISPLAY_INLINE_FLEX,
    QL_UI_DISPLAY_GRID,          /* B6 */
    QL_UI_DISPLAY_INLINE_GRID,   /* B6（布局上与 grid 同义） */
    /*
     * B7 表格的五个角色。浏览器把它们当成"内部 display"，外面会自动补
     * anonymous 包装盒；本库不补（要求结构写规范），布局时按角色收集。
     */
    QL_UI_DISPLAY_TABLE,
    QL_UI_DISPLAY_TABLE_ROW,
    QL_UI_DISPLAY_TABLE_CELL,
    QL_UI_DISPLAY_TABLE_CAPTION,
    QL_UI_DISPLAY_TABLE_ROW_GROUP,   /* thead / tbody / tfoot 同义 */
    QL_UI_DISPLAY_TABLE_COLUMN,       /* <col>：只提供列宽，不产生盒子 */
    QL_UI_DISPLAY_TABLE_COLUMN_GROUP, /* <colgroup>：同上，可嵌套 <col> */
} QLUIDisplay;

/* `vertical-align` 在**表格单元里**的取值（其它场合只认 baseline/sub/super） */
typedef enum {
    QL_UI_VALIGN_BASELINE = 0,
    QL_UI_VALIGN_MIDDLE,
    QL_UI_VALIGN_TOP,
    QL_UI_VALIGN_BOTTOM,
} QLUIVerticalAlign;

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
    /*
     * `sticky`：**在流里占位**（像 relative），但贴住最近的滚动容器的可视边缘。
     * 实现方式是布局末尾的一次补偿平移（见 qlui.c 的 apply_sticky），
     * 所以它对绘制/命中/脏区都是"位置就是对的"，没有第二套坐标。
     */
    QL_UI_POSITION_STICKY,
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
    /*
     * 相对单位。`EM` / `REM` 的 `value` 是**百分之一 em**（`1.5em` → 150），
     * 因为要表达小数；`VW` / `VH` 的 `value` 是整数百分比（`10vw` → 10）。
     *
     * `EM` / `REM` 在 `compile_style` 里就折成 px（那时字号已确定），
     * 只有 `VW` / `VH` 活到布局期 —— 视口尺寸只有布局时才知道。
     */
    QL_UI_UNIT_EM,
    QL_UI_UNIT_REM,
    QL_UI_UNIT_VW,
    QL_UI_UNIT_VH,
    QL_UI_UNIT_VMIN,       /* 视口长边/短边里**较小**的那个 */
    QL_UI_UNIT_VMAX,
    /*
     * `calc()` / `min()` / `max()` / `clamp()` 编译成的**后缀字节码**。
     * `value` 是 `QLUIDoc.calc_ops[]` 里的起始下标；表达式自带 `QL_UI_CALC_END`
     * 结尾，所以多个字段可以指向同一段。求值在布局期做 —— 只有那时才知道
     * 百分比基准和视口尺寸。
     */
    QL_UI_UNIT_CALC,
} QLUIUnit;

/* `calc()` 字节码的一条指令（4 字节，blob 里顺序存放） */
typedef enum {
    QL_UI_CALC_TERM = 0,   /* 压入操作数：`value` + `unit`（QLUIUnit） */
    QL_UI_CALC_ADD,
    QL_UI_CALC_SUB,
    QL_UI_CALC_MIN,        /* `unit` = 参数个数（2..8） */
    QL_UI_CALC_MAX,
    QL_UI_CALC_CLAMP,      /* 3 个参数：lo / val / hi */
    QL_UI_CALC_END,        /* 表达式结束 */
} QLUICalcOpKind;

typedef struct {
    int16_t value;         /* TERM：按 `unit` 解释的操作数；其余指令不用 */
    uint8_t op;            /* QLUICalcOpKind */
    uint8_t unit;          /* TERM：QLUIUnit；MIN/MAX/CLAMP：参数个数 */
} QLUICalcOp;

_Static_assert(sizeof(QLUICalcOp) == 4, "calc 指令必须是 4 字节（blob 里顺序存放）");

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

/*
 * 一层阴影（`box-shadow` 的一项，或 `text-shadow`）。
 *
 * `inset` 只对 box-shadow 有意义：1 = 画在盒子**里面**（内阴影），
 * 常用于"凹陷的输入框 / 卡片内凹边"。
 */
typedef struct {
    int16_t  dx;       /* 偏移（px） */
    int16_t  dy;
    uint8_t  blur;     /* 0 = 硬边；> 0 的软边由后端的覆盖率衰减实现 */
    uint8_t  alpha;    /* 0 = 这一层不存在 */
    uint8_t  inset;    /* 1 = 内阴影 */
    int8_t   spread;   /* 扩散半径 px（CSS `box-shadow: x y blur spread color`，可为负） */
    uint32_t color;    /* 0xRRGGBB */
} QLUIShadow;          /* 12 字节 */

typedef struct {
    uint32_t    bg_color;      /* 0xRRGGBB；alpha 见 bg_alpha */
    uint32_t    text_color;
    uint32_t    bg_color2;     /* 渐变终止色：只有 bg_grad_dir != 0 时有效 */
    /*
     * 四边的边框色，顺序 上 右 下 左（blob v26）。
     * `border-color: #333` 会写满四个；`border-color: a b c d` 分别是四边。
     */
    uint32_t    border_color4[4];
    /* `box-shadow` 列表：最多两层（顺序 = 书写顺序，先写的画在下面） */
    QLUIShadow  shadow[2];
    /* `text-shadow`：一层（dx/dy/color/alpha，blur 只做近似） */
    QLUIShadow  text_shadow;

    QLUILength  width;
    QLUILength  height;
    /*
     * `min-width` / `max-width` / `min-height` / `max-height`。
     * `unit == QL_UI_UNIT_AUTO` = 没设（不是 0）。百分比相对父元素内容盒。
     */
    QLUILength  min_width;
    QLUILength  max_width;
    QLUILength  min_height;
    QLUILength  max_height;

    QLUIEdges   margin;
    QLUIEdges   padding;

    uint8_t     display;       /* QLUIDisplay */
    uint8_t     bg_alpha;      /* 0..255：背景（或渐变起始色）的 alpha */
    /*
     * 渐变**终止色**的 alpha（v37）。
     *
     * 以前一个背景只有一个 alpha，`linear-gradient(#FFF, rgba(255,255,255,0))` 的
     * 透明端会被静默丢掉 —— 玻璃/柔光这类效果全靠"一端不透明另一端透明"，
     * 所以单独存一个。没有渐变（或渐变色标都没写 alpha）时它等于 bg_alpha，
     * 与旧行为完全一致。
     */
    uint8_t     bg_alpha2;
    /*
     * 四边独立的边框宽度，顺序 上 右 下 左（blob v26）。
     * `border-width: 2px` 写满四个；`border-width: 1px 2px` 是"上下 左右"。
     */
    uint8_t     border_w[4];
    /* 每边的样式（QLUIBorderStyle）。`border-style` 简写同样写满四边 */
    uint8_t     border_style4[4];
    /*
     * 四角圆角半径（px），顺序 左上 右上 右下 左下。
     * `border-radius: 8px` 写满四个；两个值 = "左上右下 右上左下"。
     */
    int16_t     radius[4];
    uint8_t     text_align;    /* QLTextAlign */
    uint8_t     font_id;       /* doc->font_sizes 的下标（字号，像素） */
    uint8_t     box_sizing;    /* 0 = content-box, 1 = border-box */
    uint8_t     bg_grad_dir;   /* 0 = 纯色；1 = 垂直；2 = 水平；3 = 径向（= QLGradientDir+1） */

    /* ----- flex 容器 ----- */
    uint8_t     flex_direction;  /* QLUIDirection */
    uint8_t     justify_content; /* QLUIJustify */
    uint8_t     align_items;     /* QLUIAlign */
    uint8_t     flex_wrap;       /* 0 = nowrap，1 = wrap */
    int16_t     gap_row;
    int16_t     gap_col;
    /*
     * gap 的单位（低 4 位 = row，高 4 位 = column）：0 = px，1 = em，2 = rem。
     * em/rem 时 `gap_row`/`gap_col` 存的是**百分之一 em**，解析末尾统一折成 px
     * 并把这里清零 —— 布局期只可能看到 px。
     */
    uint8_t     gap_units;

    /* ----- flex 项 ----- */
    uint16_t    flex_grow;     /* 定点 /100：100 表示 flex-grow:1，默认 0 */
    uint16_t    flex_shrink;   /* 定点 /100：默认 100（即 flex-shrink:1） */
    int16_t     flex_basis;    /* -1 = auto（用 width）；>=0 = px */
    uint16_t    flex_basis_pct;/* 当 basis 是百分比时有效；0 = 不是百分比 */

    /* ----- 定位与裁剪 ----- */
    uint8_t     position;      /* QLUIPosition */
    /*
     * `overflow`：**最后一次写的简写值**，只为兼容读法保留。
     * 真正生效的是 `overflow_x` / `overflow_y`（`overflow: hidden` 两个轴都写，
     * `overflow-x: hidden` 只写一个轴）。运行期一律用下面的访问器。
     */
    uint8_t     overflow;      /* QLUIOverflow（简写值） */
    int16_t     z_index;
    QLUILength  inset[4];      /* top / right / bottom / left（只有非 static 时用） */

    /* ----- 状态过渡（transition） -----
     * 状态位（:hover/:active/:focus）切换时，这些属性在 trans_ms 内插值过去，
     * 而不是瞬间跳变。0 = 无过渡。位图见 QL_UI_TRANS_*。
     */
    uint16_t    trans_ms;
    uint8_t     trans_ease;    /* QLEasing */
    /*
     * `transition-delay`（毫秒）：起播前先干等多久。
     * 过渡期里样式保持起点值（CSS 语义），不会"提前一点点开始动"。
     */
    uint16_t    trans_delay_ms;
    /* `steps(n)`：>0 时覆盖 `trans_ease`（阶跃缓动） */
    uint8_t     trans_steps;
    /* 自定义 `cubic-bezier(x1,y1,x2,y2)`：单位为 1/1000，`trans_bez_on` 为真时生效 */
    uint8_t     trans_bez_on;
    int16_t     trans_bez[4];
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
    /*
     * ----- clip-path 其他形状（blob v34） -----
     *
     *   clip_kind: 0 无、1 circle、2 ellipse、3 inset、4 polygon
     *
     * `clip_box` 是**按形状复用**的 4 个长度：
     *   ellipse: [0] = rx、[1] = ry（百分比相对于边框盒宽/高）
     *   inset  : [0..3] = top / right / bottom / left（px 或百分比）
     *   circle / polygon 不用它。
     *
     * `clip_pts` / `clip_pt_n`：polygon 的顶点在 `doc->clip_pts` 池里的起始下标
     * 与个数。每个顶点占 2 个 QLUILength（x, y），x 的百分比相对于边框盒宽、
     * y 相对于高。同一份顶点表在池里只存一份（同值去重）。
     */
    QLUILength  clip_box[4];
    uint16_t    clip_pts;
    uint8_t     clip_pt_n;
    /*
 * `transform: translate()` 用了百分比时的标记（bit0 = tf_x、bit1 = tf_y）。
 * 百分比相对**元素自身**的边框盒（x → 宽、y → 高），构建期不知道尺寸，
 * 所以 `tf_x/tf_y` 先存百分数，等布局期 `resolve_tf_pct()` 折成 px 写回样式副本。
 * 这个字节原来是 `clip_reserved` 填充位，占用不变 → blob 布局不变、不用升版本。
 */
    uint8_t     tf_pct;

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
    /*
     * ----- 非轴对齐变换（M9.4，blob v30） -----
     *
     * `rotate()` / `skew()` / `matrix()`（以及它们与 translate/scale 的组合）
     * 没法用"矩形 + 轴对齐缩放"表达，所以有它们时元素走**离屏图层 + 仿射重采样**：
     * 整棵子树先按原样合成，再整体按矩阵画回去（与 CSS "transform 作用于元素的
     * 渲染结果"语义一致）。
     *
     * `xform` 是 doc->xforms[] 的下标（同一份变换在池里只存一份）。
     * 有 xform 时 `tf_*` / `sc_*` 不再单独生效 —— 它们已被折进矩阵。
     */
    int16_t     xform;
    /* `transform-origin`：x / y（px，或 percent = 边框盒的千分比） */
    QLUILength  origin[2];

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
     * `backdrop-filter: saturate(<N>%)`（v37），0 = 没写（按 100% 处理）。
     *
     * Apple 的"液态玻璃"关键一步：模糊之后把颜色推艳。只有模糊的话，
     * 玻璃后面是一片灰蒙蒙的均值色，看不出"玻璃"。
     */
    uint8_t     backdrop_sat;
    /*
     * `backdrop-filter: refract(<N>px)`（v38），0 = 没写。
     *
     * 在轮廓附近把采样点往外推一圈，玻璃边缘于是有"透镜"的厚度感 ——
     * 只有 blur 的玻璃是一块均匀的毛玻璃，折射才让它像**有形状的**玻璃。
     * 半径越大轮廓那一圈弯折越明显；做成圆角/圆形时就是 Apple 那套液态玻璃。
     */
    uint8_t     backdrop_refract;

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
     * white-space：见 QL_UI_WS_*。
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

    /*
     * 基线额外偏移（px，负数 = 上移）。`<sup>` 用负值、`<sub>` 用正值。
     *
     * 只影响**这一行文字的基线**，不影响行盒高度 —— 与浏览器一致
     * （`vertical-align: super` 不会把行撑高；浏览器是靠字体自身的
     * 上升/下降空间容下它）。所以上下标可能顶到上一行的空隙里，
     * 要干净就显式给 `line-height`。
     */
    int8_t      baseline_dy;

    /*
     * 字体族下标：0 = 正文（`ql_font_default`），1 = 等宽。
     *
     * 本库只有两级：内建位图字体本身就是等宽的，所以**不接矢量字体时
     * 两级其实一样**。接了 FreeType（`ql_font_set_mono_factory`）才分得开：
     * `<pre>` / `<code>` / `<kbd>` / `<samp>` 走等宽，其余走正文。
     */
    uint8_t     font_family;

    /*
     * `opacity`：元素整体的不透明度（0..255，255 = 不透明）。
     *
     * 施加方式是**绘制期把所有 alpha 乘一遍**（背景/边框/阴影/文字/图片），
     * 并且顺着子树往下乘 —— 所以子元素也会一起淡出，观感接近 CSS 的
     * group opacity。差别只在"同一子树内部互相覆盖"的场合：CSS 是先合成
     * 一张图再整体降透明，这里是逐个图元降透明，重叠处会略微透出来。
     * 要做到严格一致需要离屏图层（见《CSS3 覆盖与裁剪》§20.1）。
     */
    uint8_t     opacity;

    /*
     * `margin` / `padding` 每一边的"单位"（每边 4 位，顺序 上右下左）：
     *   0 = px、1 = em、2 = rem、3 = auto（只对 margin 有效）、
     *   4 = %（相对**包含块宽度**）、5 = vw、6 = vh、7 = vmin、8 = vmax。
     *
     * em/rem 在**构建期**就折成 px 了（`resolve_length_units()`），所以这里只剩
     * px / auto / 相对单位。相对单位（% / vw / vh / vmin / vmax）必须在**布局期**
     * 求值：`QLUIEdges` 里存的是**原始数值**（`5%` → 5），
     * 由 `ql_ui_layout()` 把结果写进 `QLUINodeRT.margin_px` / `padding_px`，
     * 布局代码只读那两个（见 `resolve_node_edges()`）。
     */
    uint16_t    margin_unit;
    uint16_t    padding_unit;

    /*
     * `aspect-ratio: 宽 / 高`（存成两个 uint16；任一为 0 = 没设）。
     * 只在"一个方向是确定长度、另一个是 auto"时生效 —— 两个方向都写了具体值时
     * 以具体值为准（与 CSS 一致）。
     */
    uint16_t    aspect_w;
    uint16_t    aspect_h;

    /* ----- 文本细节（B3） -----
     * `letter-spacing` / `text-indent` 存 px（em/rem 在构建期已折成 px）；
     * 后三个是枚举，见下面 QL_UI_TEXT_* / QL_UI_OVERFLOW_* / QL_UI_WORDBREAK_*。
     */
    int16_t     letter_spacing;  /* 字间距（px，可为负） */
    int16_t     word_spacing;    /* 词间距（px，可为负）：加在**空格之后**（B3） */
    int16_t     text_indent;     /* 首行缩进（px） */
    /*
     * `tab-size`（B3）：制表符折算成几个空格。0 = 按 8 个空格（CSS 初值），
     * 支持 1..32。只认"空格个数"写法；`tab-size: 20px` 报错（要量字体，不值得）。
     */
    uint8_t     tab_size;
    uint8_t     text_transform;  /* QL_UI_TEXT_* */
    uint8_t     text_overflow;   /* QL_UI_OVERFLOW_* */
    uint8_t     word_break;      /* QL_UI_WORDBREAK_* */
    uint8_t     reserved_text;   /* 对齐填充 */

    /* ----- B5 小件（blob v23） ----- */
    /*
     * `visibility`：**占位但不可见**（与 `display: none` 的区别就在这里 ——
     * 后者不参与布局）。它是**继承**属性：父级 hidden 时子级也 hidden，
     * 子级可以自己写 `visibility: visible` 显示出来（CSS 语义）。
     * 命中测试也不认它（看不见的东西不该能点）。
     */
    uint8_t     visibility;      /* QL_UI_VISIBLE / QL_UI_HIDDEN */
    /*
     * `pointer-events: none`：自己**不参与命中测试**，但子节点照常
     * （子节点可以写 `pointer-events: auto` 重新接受点击）。
     * 常见用法：盖在内容上的装饰层不该挡住下面的按钮。
     */
    uint8_t     pointer_events;  /* 0 = auto，1 = none */
    /* `align-self`：只覆盖**自己**在容器交叉轴上的对齐；0xFF = auto */
    uint8_t     align_self;      /* QLUIAlign，或 QL_UI_ALIGN_SELF_AUTO */
    /* `overflow-x` / `overflow-y`：分轴裁剪（blob v24） */
    uint8_t     overflow_x;      /* QLUIOverflow */
    uint8_t     overflow_y;      /* QLUIOverflow */
    /* `align-content`：多行的交叉轴分配（单行时由"行撑满容器"接管） */
    uint8_t     align_content;   /* QLUIAlignContent */
    /* `cursor`：命中测试时算出来的指针形状提示（见 ql_ui_cursor） */
    uint8_t     cursor;          /* QLUICursor */
    /*
     * `text-decoration-*`（B3）。`font_flags` 里的 UNDERLINE / LINE_THROUGH /
     * OVERLINE 是"画哪几条线"，这里决定**线长什么样**：
     *   deco_style     : 0 solid、1 double、2 dotted、3 dashed（QLUIBorderStyle 同序）
     *   deco_thickness : 线宽 px（0 = 1px）
     *   deco_offset    : 下划线额外下移 px（可为负，`text-underline-offset`）
     *   deco_alpha     : 0 = 用文字色；>0 = 用 deco_color 且按这个 alpha
     */
    uint8_t     deco_style;
    int8_t      deco_thickness;
    int8_t      deco_offset;
    uint8_t     deco_alpha;
    uint32_t    deco_color;
    /* `user-select: none` → 1：这一块里的文字不可被选中 */
    uint8_t     user_select_none;
    /* `scroll-behavior: smooth` → 1：程序化滚动用动画过渡（宿主逐帧驱动） */
    uint8_t     scroll_smooth;
    /*
     * `order`：flex 项的排列顺序（升序，相等时保持 HTML 顺序）。
     * 只影响**子项之间**的顺序；绘制顺序也跟着它（z-index 相同时）。
     */
    int16_t     order;

    /* ----- B4 绘制细节（blob v25） ----- */
    /*
     * `outline`：画在边框**外面**的一圈线，**不占布局**（不改变盒子尺寸、
     * 不影响兄弟位置），所以 focus 环、选中高亮用它最合适。
     *
     * CSS 里 outline 有几条怪规则，本库照抄了最要紧的两条：
     *   - 不参与布局，可以被 `overflow: hidden` 的祖先裁掉（CSS 也说可以）；
     *   - `outline-offset` 决定离边框盒多远，负值会压进边框里面。
     */
    uint32_t    outline_color;
    int8_t      outline_width;   /* px；0 = 不画 */
    int8_t      outline_offset;  /* px，可为负 */
    uint8_t     outline_style;   /* QLUIBorderStyle（outline 也支持 dashed/dotted） */

    /* ----- B4 收尾（blob v26） ----- */
    /*
     * `image-rendering`：0 = auto（GL 用线性采样），1 = pixelated（最近邻放大，
     * 像素画/放大镜要它）。CPU 后端本来就只有最近邻，所以在 CPU 上这个属性
     * 没有可见差异；差异只在 GPU 放大图片时。
     */
    uint8_t     image_rendering;
    /*
     * `scrollbar-width`：0 = 默认 4px、1 = thin(2px)、2 = auto(4px)、3 = none(不画)。
     * `scrollbar-color`：滑块色（0 = 默认中灰）+ 轨道色（alpha 0 = 不画轨道）。
     * 只影响**覆盖式滚动条**（本库的滚动条不占布局）。
     */
    uint8_t     scrollbar_width;
    uint8_t     scrollbar_alpha;    /* 滑块不透明度（0 = 默认 0x96） */
    uint8_t     object_fit2;        /* 保留 */
    uint32_t    scrollbar_color;
    /*
     * `object-position: x y`：图片在 object-fit 之后的盒子里怎么摆。
     * 单位与其它长度一致（px / 百分比，百分比相对于"多出来的余量"）。
     */
    QLUILength  object_position[2];

    /* ----- `background-image: url()`（B4 收尾，blob v27） ----- */
    /*
     * 资源名在**文本池**里的下标（0xFFFF = 没有背景图）。
     * 与 `<img src>` 共用同一套资源表：`ql_ui_register_image()` 按名字填像素。
     */
    uint16_t    bg_image;
    uint8_t     bg_size_mode;   /* 0 = 用 bg_size、1 = cover、2 = contain */
    uint8_t     bg_repeat;      /* 0 repeat、1 no-repeat、2 repeat-x、3 repeat-y */
    QLUILength  bg_size[2];     /* 宽 / 高（auto = 保持比例） */
    QLUILength  bg_pos[2];      /* 百分比（千分比）或 px */

    /* ----- Grid（B6，blob v28） ----- */
    /*
     * 两侧：容器用 `grid`（模板下标）+ `grid_flow`；子项用四个 line/span。
     * 全部在**构建期**算成整数（`grid-area: 名字`、`span 2`、`-1` 都已展开），
     * 运行期不做任何字符串比较。
     */
    uint16_t    grid;           /* doc->grids[] 下标；QL_UI_NO_GRID = 不是 grid 容器 */
    int16_t     grid_col;       /* 起始列线（1-based）；0 = 自动 */
    int16_t     grid_row;       /* 起始行线（1-based）；0 = 自动 */
    uint8_t     grid_col_span;  /* >= 1 */
    uint8_t     grid_row_span;
    uint8_t     grid_flow;      /* QLUIGridFlow */
    uint8_t     justify_items;  /* QLUIAlign */
    uint8_t     justify_self;   /* QLUIAlign，或 QL_UI_ALIGN_SELF_AUTO */
    uint8_t     grid_reserved;
    /* 隐式轨道（`grid-auto-rows/columns`）：只支持单个长度值；0 = auto */
    int16_t     grid_auto_row;
    int16_t     grid_auto_col;

    /* ----- `border-image`（B6，blob v28） ----- */
    uint16_t    border_image;   /* 资源名在文本池里的下标；QL_UI_NO_BORDER_IMAGE = 无 */
    uint8_t     bi_slice_pct;   /* slice 是否按图片尺寸的千分比解释 */
    uint8_t     bi_width_pct;   /* width 是否按千分比解释 */
    uint8_t     bi_repeat;      /* QLUIBorderImageRepeat */
    uint8_t     bi_reserved;
    int16_t     bi_slice[4];    /* 上 右 下 左 */
    int16_t     bi_width[4];    /* 边框宽度（0 = 用 slice） */

    /* ----- 离屏图层：filter / mix-blend-mode / mask（M9.3，blob v29） ----- */
    /*
     * `filter: <fn>+`。**有这几样就必须走离屏图层**：先把子树合成到一张临时图，
     * 再整体处理。逐图元施加"滤镜"和"先合成再滤镜"是两回事（重叠处、模糊半径
     * 吃到的邻居像素都不一样）。
     */
    uint8_t     filter_n;                              /* 0..QL_CFG_UI_FILTER_MAX_OPS */
    uint16_t    filter_op[QL_CFG_UI_FILTER_MAX_OPS];   /* QL_UI_FILTER_PACK 打包 */
    int8_t      ds_dx;          /* `drop-shadow()` 的水平偏移 px */
    int8_t      ds_dy;          /* 垂直偏移 px */
    uint8_t     ds_blur;        /* 模糊半径 px（0 = 硬边） */
    uint8_t     ds_a;           /* 0 = 用 ds_color 自己的 alpha */
    uint32_t    ds_color;       /* 0 = currentColor */
    uint8_t     mix_blend;      /* QLUIBlendMode */
    uint16_t    mask_image;     /* 资源名在文本池里的下标；QL_UI_NO_MASK = 无 */
    /*
     * `mask: url()` 目前只支持"按边框盒拉伸 + 用图片 alpha"这一种，
     * 覆盖 `mask-image` / `-size: 100% 100%` / `-mode: alpha` 三个默认值。
     */

    /* ----- B7 表格（blob v31） -----
     *
     * 这三个只对"表格角色"的节点有意义（table / table-row / table-cell），
     * 其它元素上写了也不生效（与浏览器一致：这些属性只属于表格模型）。
     *
     * `table_layout`：0 = auto（默认，本库按 fixed 语义近似处理），1 = fixed。
     * `border_collapse`：0 = separate（默认），1 = collapse。
     * `table_spacing`：`border-spacing` 的 x / y（px；separate 时才有意义）。
     * `valign`：见 QLUIVerticalAlign —— 表格单元的垂直对齐。
     */
    uint8_t     table_layout;
    uint8_t     border_collapse;
    uint8_t     valign;
    int16_t     table_spacing_x;
    int16_t     table_spacing_y;

    /* ----- 多栏（B7 后半，blob v32） -----
     *
     * 与浏览器同一套属性名，但语义收窄成"**贪心分栏**"：按顺序把子节点塞进
     * 当前栏，塞不下（会超过栏高）就换下一栏，**不把一个子节点劈成两半**。
     *
     * `column_count`：0 = auto（栏数由 `column_width` 反推）；>0 = 栏数上限。
     * `column_width`：auto = 不定宽（栏宽 = (容器宽 - 栏间距×(n-1)) / n）。
     * 两者都给时**栏数优先**（浏览器是"能塞几栏就几栏，最多 count"，差别见文档）。
     * 栏间距直接用 `column-gap`（与 flex 共用同一个字段，auto 时按 16px 算）。
     * `column_rule_*`：栏间分隔线；`column_rule_w == 0` 或 style == none 时不画，
     * `column_rule_color == 0` 表示 currentColor。
     */
    uint8_t     column_count;
    uint8_t     column_rule_w;
    uint8_t     column_rule_style;
    uint8_t     column_reserved;
    uint32_t    column_rule_color;
    QLUILength  column_width;

    /* ----- 文本栈与双向文本（B8，blob v33） -----
     *
     * `direction`：0 = ltr（默认），1 = rtl。它决定段落基准方向，
     *   也决定 `text-align: start/end` 落在哪一边。
     * `unicode_bidi`：0 = normal，1 = embed（当前与 normal 等价），
     *   2 = bidi-override（整段按 `direction` 强制排）。
     * `ligatures` / `kerning`：0 = 关（`font-variant-ligatures: none` /
     *   `font-feature-settings: "liga" 0`），默认 1 = 开。
     */
    uint8_t     direction;
    uint8_t     unicode_bidi;
    uint8_t     ligatures;
    uint8_t     kerning;

    /* ----- B9：字体变体 + 椭圆圆角（blob v34） -----
     *
     * `variant_flags` 位图（见 QL_UI_VARIANT_*）：
     *   SMALL_CAPS   → `font-variant-caps: small-caps`，靠 HarfBuzz 的 `smcp`
     *   TABULAR_NUMS → `font-variant-numeric: tabular-nums`，靠 `tnum`
     * 两者都需要 HarfBuzz（内建整形没有 OpenType 特性）；没 HarfBuzz 时
     * **不报错也不生效**，文档写明（属于"可选增强"）。
     *
     * `radius_v` 是四角半径的**纵向**分量：`border-radius: 10px / 20px` 时
     * 水平 = radius[]、垂直 = radius_v[]。都为 0 表示"等半径"（老语义，逐像素不变）。
     */
    uint8_t     variant_flags;
    uint8_t     radius_reserved;
    int16_t     radius_v[4];   /* 上左 上右 下右 下左，与 radius[] 同序 */
} QLUIStyle;

/* ============================== transform 矩阵（M9.4） ============================== */

/*
 * 一个 2x3 仿射矩阵，字段顺序与 CSS `matrix(a, b, c, d, e, f)` 一致：
 *
 *   [ a c e ]
 *   [ b d f ]
 *
 * 所有量都是 **26.6 定点**：`a..d` 无量纲（64 = 1.0），`e/f` 是 CSS px（64 = 1px）。
 * 为什么进"池"而不是塞进样式：一份文档里通常只有几个不同的 transform，
 * 但样式表可能有几十条 —— 直接放样式里每个样式要长 24 字节（MCU 上是真金白银）。
 */
typedef struct {
    int32_t a, b, c, d, e, f;
} QLUIXform;

#define QL_UI_NO_XFORM   ((int16_t)-1)

/* clip-path: polygon() 的一个顶点（x 相对边框盒宽，y 相对高） */
typedef struct {
    QLUILength x;
    QLUILength y;
} QLClipPt;

#define QL_UI_NO_CLIP_PTS  0xFFFFu

/* ============================== 离屏图层（M9.3） ============================== */


/* ============================== Grid（B6） ============================== */

#define QL_UI_NO_GRID        0xFFFFu
/* 负数网格线（`-1` = 最后一条线）在构建期就已换算成正数，运行期不会见到负值 */

typedef enum {
    QL_UI_GT_FIXED = 0,     /* value = px（% 是 QL_UI_GT_PERCENT） */
    QL_UI_GT_PERCENT,       /* value = 千分比（列相对内容宽，行相对内容高） */
    QL_UI_GT_FR,            /* value = fr 分子 */
    QL_UI_GT_AUTO,          /* max-content（`min-content` / `max-content` 也归到这里） */
} QLUIGridTrackKind;

/*
 * 一条轨道 = 一个 [min, max] 区间：
 *   `100px`      → min = max = FIXED(100)
 *   `1fr`        → min = AUTO(0)  , max = FR(1)
 *   `auto`       → min = max = AUTO
 *   `minmax(a,b)`→ min = a        , max = b
 * 定尺时先满足 min，再把剩余空间按 max 侧的 "拉伸能力" 分配（FR 权重 / AUTO 取内容）。
 */
typedef struct {
    uint8_t min_kind;   /* QLUIGridTrackKind */
    uint8_t max_kind;
    int16_t min_val;
    int16_t max_val;
} QLUIGridTrack;   /* 6 字节 */

/* `grid-template-areas` 里的一个具名区域（行列号都是 1-based） */
typedef struct {
    uint16_t name;      /* 文本池下标 */
    int16_t  col;       /* 起始列线 */
    int16_t  row;       /* 起始行线 */
    uint8_t  col_span;
    uint8_t  row_span;
} QLUIGridArea;   /* 10 字节 */

/* `grid-auto-flow`：前两位给方向，dense 位单独一个 */
typedef enum {
    QL_UI_GRID_FLOW_ROW = 0,
    QL_UI_GRID_FLOW_COLUMN = 1,
    QL_UI_GRID_FLOW_ROW_DENSE = 2,
    QL_UI_GRID_FLOW_COLUMN_DENSE = 3,
} QLUIGridFlow;

/* 一个 grid 容器的模板（所有 grid 容器共用 doc 里的一段池） */
typedef struct {
    uint16_t col_first;    /* 显式列轨道在 doc->grid_tracks[] 里的起点 */
    uint16_t col_count;
    uint16_t row_first;
    uint16_t row_count;
    uint16_t area_first;   /* 具名区域在 doc->grid_areas[] 里的起点 */
    uint16_t area_count;
    uint16_t col_hint;     /* 只有 areas 没写 template-columns 时，由区域推出来的列数 */
    uint16_t row_hint;
} QLUIGrid;   /* 16 字节 */

/*
 * `background-image` 之外，`border-image` 也用同一套资源表。
 * 九宫格切片：`slice` / `width` 各 4 个值（上右下左），单位由 `*_mode` 决定：
 *   0 = px、1 = 千分比（相对图片自身尺寸）
 */
#define QL_UI_NO_BORDER_IMAGE  0xFFFFu
typedef enum {
    QL_UI_BI_STRETCH = 0,   /* 默认：整条边拉伸 */
    QL_UI_BI_REPEAT,        /* 平铺，边缘瓦片可被裁 */
    QL_UI_BI_ROUND,         /* 平铺，缩放瓦片使恰好整数个 */
    QL_UI_BI_SPACE,         /* 平铺，间距均分（不缩放） */
} QLUIBorderImageRepeat;

/*
 * `border-style` / `outline-style`。
 *
 * 只做三种实心图案：`solid` 是普通描边；`dashed` / `dotted` 在绘制期
 * 沿四条边切成小段填色（不改布局、不进 SDF）。`none` 让宽度失效。
 */
typedef enum {
    QL_UI_BORDER_SOLID = 0,
    QL_UI_BORDER_DASHED,
    QL_UI_BORDER_DOTTED,
    QL_UI_BORDER_NONE,
} QLUIBorderStyle;

/*
 * 四边/四角的访问器。
 *
 * 布局代码里大量出现"左右边框一共多宽"，写成 `s->border_w[1] + s->border_w[3]`
 * 既长又容易把顺序写错，所以收在这里。顺序统一是 **上 右 下 左**。
 */
static inline ql_coord_t ql_ui_border_h(const QLUIStyle *s)
{
    return (ql_coord_t)s->border_w[1] + (ql_coord_t)s->border_w[3];
}
static inline ql_coord_t ql_ui_border_v(const QLUIStyle *s)
{
    return (ql_coord_t)s->border_w[0] + (ql_coord_t)s->border_w[2];
}
/* 四边是否有任何一边要画 */
static inline int ql_ui_has_border(const QLUIStyle *s)
{
    for (int i = 0; i < 4; i++)
        if (s->border_w[i] > 0 && s->border_style4[i] != QL_UI_BORDER_NONE)
            return 1;
    return 0;
}
/* 四边宽度/颜色全同？全同时绘制可以走一条"整圈"的快路径 */
static inline int ql_ui_border_uniform(const QLUIStyle *s)
{
    if (s->border_w[0] != s->border_w[1] || s->border_w[0] != s->border_w[2] ||
        s->border_w[0] != s->border_w[3])
        return 0;
    if (s->border_style4[0] != s->border_style4[1] ||
        s->border_style4[0] != s->border_style4[2] ||
        s->border_style4[0] != s->border_style4[3])
        return 0;
    for (int i = 1; i < 4; i++)
        if (s->border_color4[i] != s->border_color4[0])
            return 0;
    return 1;
}
/* 四角半径是否全同（全同时可以用单半径的快路径） */
static inline int ql_ui_radius_uniform(const QLUIStyle *s)
{
    return s->radius[0] == s->radius[1] && s->radius[0] == s->radius[2] &&
           s->radius[0] == s->radius[3];
}
/*
 * 四个圆角里的最大值（给"只支持单半径"的接口/兜底路径用）。
 *
 * 负值是"百分比还没折成 px"的标记（`50%` → -50，见 `resolve_radius_pct()`）；
 * 没布局过的节点（`display: none`）可能还留着负数，这里一律按 0 处理，
 * 免得把负数当半径传进绘制。
 */
static inline ql_coord_t ql_ui_radius_max4(const ql_coord_t r4[4])
{
    ql_coord_t m = 0;
    for (int i = 0; i < 4; i++)
        if (r4[i] > m)
            m = r4[i];
    return m;
}

/* 最大的那个圆角（给"只支持单半径"的旧接口/兜底路径用） */
static inline ql_coord_t ql_ui_radius_max(const QLUIStyle *s)
{
    ql_coord_t m = 0;                       /* 负值 = 未折成 px 的百分比，按 0 */
    for (int i = 0; i < 4; i++)
        if (s->radius[i] > m)
            m = s->radius[i];
    /* 椭圆圆角（`a / b`）取两分量的平均作为等价单半径，见 style_radii4 */
    if (s->radius_v[0] > 0 || s->radius_v[1] > 0 || s->radius_v[2] > 0 ||
        s->radius_v[3] > 0) {
        ql_coord_t mv = 0;
        for (int i = 0; i < 4; i++)
            if (s->radius_v[i] > mv)
                mv = s->radius_v[i];
        m = (m + mv) / 2;
    }
    return m;
}

/* `align-content`（多行 flex 的交叉轴分配） */
typedef enum {
    QL_UI_ALIGN_CONTENT_STRETCH = 0,   /* 默认：行按剩余空间拉伸 */
    QL_UI_ALIGN_CONTENT_START,
    QL_UI_ALIGN_CONTENT_CENTER,
    QL_UI_ALIGN_CONTENT_END,
    QL_UI_ALIGN_CONTENT_SPACE_BETWEEN,
    QL_UI_ALIGN_CONTENT_SPACE_AROUND,
    QL_UI_ALIGN_CONTENT_SPACE_EVENLY,
} QLUIAlignContent;

/*
 * `cursor`：**只是意图**，形状由宿主/后端去设。
 * 命中测试算出"当前位置该用哪个光标"，宿主用 `ql_ui_cursor()` 取出来交给窗口系统
 * （X11 的 `XDefineCursor` / Wayland 的 `wl_cursor`）。这样这套值在 MCU 上也有意义
 * （有触摸屏的板子可以忽略，但语义统一）。
 */
typedef enum {
    QL_UI_CURSOR_AUTO = 0,   /* 用父级/宿主的默认（`auto`） */
    QL_UI_CURSOR_DEFAULT,    /* `default`：箭头 */
    QL_UI_CURSOR_POINTER,    /* `pointer`：手型（可点） */
    QL_UI_CURSOR_TEXT,       /* `text`：文本 I 形（输入框/可选文字） */
    QL_UI_CURSOR_MOVE,       /* `move`：四向箭头 */
    QL_UI_CURSOR_NOT_ALLOWED,/* `not-allowed`：禁止 */
    QL_UI_CURSOR_HELP,       /* `help`：问号 */
    QL_UI_CURSOR_PROGRESS,   /* `progress`：转圈 */
} QLUICursor;

/* `visibility` */
#define QL_UI_VISIBLE   0
#define QL_UI_HIDDEN    1

/* `pointer-events` */
#define QL_UI_POINTER_AUTO  0
#define QL_UI_POINTER_NONE  1

/* `align-self: auto` —— 用容器的 `align-items`（QLUIAlign 的取值是 0..3） */
#define QL_UI_ALIGN_SELF_AUTO  0xFFu

/* 分轴 overflow 的读取入口（运行期只用这两个，不要读 s->overflow） */
static inline uint8_t ql_ui_style_overflow_x(const QLUIStyle *s)
{
    return s->overflow_x;
}
static inline uint8_t ql_ui_style_overflow_y(const QLUIStyle *s)
{
    return s->overflow_y;
}

/* `text-transform` */
#define QL_UI_TEXT_NONE        0
#define QL_UI_TEXT_UPPERCASE   1
#define QL_UI_TEXT_LOWERCASE   2
#define QL_UI_TEXT_CAPITALIZE  3

/* `text-overflow`（只有单行裁切时才有意义：搭配 `white-space: nowrap` + `overflow`） */
#define QL_UI_OVERFLOW_CLIP     0
#define QL_UI_OVERFLOW_ELLIPSIS 1

/* `word-break` / `overflow-wrap`：本库只区分"只在空格/CJK 处断"与"任意字符断" */
#define QL_UI_WORDBREAK_NORMAL 0
#define QL_UI_WORDBREAK_ALL    1

/*
 * white-space 的取值。
 *
 * 空白**保留**（`pre*`）必须在这里能表达，但有一处架构上的取舍要说清楚：
 * 文本的空白折叠发生在**解析期**（见 qlui_parse.c 的 add_text_chunk），
 * 而 `white-space` 是级联之后才知道的。所以本库的做法是把折叠**推迟到建表期**
 * （那时样式已经算好），原始空白一直留到建表 —— 这样 `<pre>` 标签和
 * `div { white-space: pre }` 两条路都能生效，代价只是编译期内存。
 */
#define QL_UI_WS_NOWRAP     0   /* 默认：不折行（与浏览器不同，见 QLUIStyle.white_space） */
#define QL_UI_WS_NORMAL     1   /* 折行；空白折叠 */
#define QL_UI_WS_PRE        2   /* 不折行；空白与换行**全部保留** */
#define QL_UI_WS_PRE_WRAP   3   /* 折行；空白与换行**全部保留** */
#define QL_UI_WS_PRE_LINE   4   /* 折行；折叠空格但**保留换行** */

/* `QLUIStyle.font_family` 的取值 */
#define QL_UI_FAMILY_DEFAULT   0
#define QL_UI_FAMILY_MONO      1

/* 空白是否保留（不折叠）。`<pre>` / `<textarea>` 靠这个判断 */
#define QL_UI_WS_PRESERVES(v)   ((v) >= QL_UI_WS_PRE)
/* 是否自动折行（`pre` 只按 `\n` 硬换行） */
#define QL_UI_WS_WRAPS(v)       ((v) == QL_UI_WS_NORMAL || (v) == QL_UI_WS_PRE_WRAP || \
                                 (v) == QL_UI_WS_PRE_LINE)

/* QLUIKeyframe 里"这个关键帧没设置该属性"的哨兵（不能与合法值撞车） */
#define QL_UI_KF_UNSET   ((int16_t)-32768)

/* 字体变体与装饰线：QLUIStyle.font_flags 的位 */
#define QL_UI_FONT_BOLD           0x01u   /* font-weight >= 600：合成加粗 */
#define QL_UI_FONT_ITALIC         0x02u   /* font-style: italic：合成斜体 */
#define QL_UI_FONT_UNDERLINE      0x04u   /* text-decoration: underline */
#define QL_UI_FONT_LINE_THROUGH   0x08u   /* text-decoration: line-through */
/*
 * `text-decoration: overline`（B3）：
 * 单独占一个宏、存在 deco_style 的高位（font_flags 的低 4 位已经满了）。
 */
#define QL_UI_DECO_OVERLINE       0x80u   /* deco_style 的高位 */
#define QL_UI_DECO_LINE_MASK      0x7Fu

/* `font-variant-*` 位图（QLUIStyle.variant_flags，blob v34） */
#define QL_UI_VARIANT_SMALL_CAPS   0x01u   /* font-variant-caps: small-caps（HarfBuzz `smcp`） */
#define QL_UI_VARIANT_TABULAR_NUMS 0x02u   /* font-variant-numeric: tabular-nums（`tnum`） */

/* `clip-path` 的形状（QLUIStyle.clip_kind，blob v34 起四种） */
#define QL_UI_CLIP_NONE     0u
#define QL_UI_CLIP_CIRCLE   1u
#define QL_UI_CLIP_ELLIPSE  2u
#define QL_UI_CLIP_INSET    3u
#define QL_UI_CLIP_POLYGON  4u

/* `background-image` 缺省：没有背景图 */
#define QL_UI_NO_IMAGE            0xFFFFu

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
    /*
     * transform: scale，千分比。`QL_UI_KF_UNSET` = 这个关键帧没写、用基础值。
     *
     * **曾经用 -1 当"未设置"**，但负缩放自 P4c 之后是合法值（`scaleX(-1)` =
     * 水平镜像，翻转动画靠它），两者撞车 —— 表现就是镜像那一帧被当成"没写"。
     * 所以哨兵值换成 int16 的最小值（-32768），它不在合法范围（±32767）里。
     */
    int16_t  sc_x;
    int16_t  sc_y;
    int16_t  radius;      /* border-radius（负 = 用基础值） */
    int16_t  clip_r;      /* clip-path 千分比（负 = 用基础值） */
    uint8_t  bg_alpha;
    uint8_t  reserved;
    /* 帧上的 `cubic-bezier(...)`（1/1000）；`bez_on` 为真时覆盖 `ease` */
    int16_t  bez[4];
    uint8_t  bez_on;
    uint8_t  reserved2;
} QLUIKeyframe;           /* 44 字节，blob 里顺序存放 */

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
    uint8_t  steps;       /* `steps(n)` 的台阶数；0 = 不用阶跃 */
    uint8_t  bez_on;      /* 1 = 用下面的 bez（动画级默认缓动） */
    /* 动画级默认 `cubic-bezier(...)`（1/1000）：帧上没写缓动时用它 */
    int16_t  bez[4];
    uint16_t reserved;
} QLUIAnimation;

/*
 * `@media` 断点区间：视口宽的闭区间（CSS px）。`hi == 0xFFFF` 表示没有上界。
 *
 * 运行期只做一件事：按当前视口宽找到命中的区间下标，然后取该区间的样式变体。
 * 区间在**构建期**就算好（把 `(min-width)` / `(max-width)` 条件在宽度轴上切段），
 * 所以同一个区间里"哪些媒体块生效"是常量 —— 运行期不需要理解任何条件表达式。
 */
typedef struct {
    uint16_t lo;
    uint16_t hi;
} QLUIMedia;

/* QLUINode.flags 的位 */
#define QL_UI_NODE_PASSWORD  0x01u   /* <input type="password">：显示成圆点 */
#define QL_UI_NODE_OPEN      0x02u   /* <details open>：展开状态 */
#define QL_UI_NODE_CHECKBOX  0x04u   /* <input type="checkbox">（B2） */
#define QL_UI_NODE_CHECKED   0x08u   /* 初始勾选：checked / selected 属性（B2） */
#define QL_UI_NODE_DISABLED  0x10u   /* disabled 属性（B2） */

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
    /* B2：`:disabled` / `:checked` 的变体，以及 `::placeholder` 的样式 */
    uint16_t style_disabled;
    uint16_t style_checked;
    uint16_t style_placeholder;
    /* `placeholder` 属性的文案（文本池下标，0xFFFF = 没有） */
    uint16_t placeholder;
    uint8_t  flags;        /* QL_UI_NODE_* */
    /* B7 表格：`colspan` / `rowspan` 属性（0 与 1 同义 = 不跨） */
    uint8_t  colspan;
    uint8_t  rowspan;
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

/* ----- 图像热区（`<map>` / `<area>`，doc 里的一条） ----- */

typedef enum {
    QL_UI_AREA_RECT = 0,   /* coords = x0 y0 x1 y1 */
    QL_UI_AREA_CIRCLE,     /* coords = cx cy r */
    QL_UI_AREA_POLY,       /* coords = x0 y0 x1 y1 …（偶数个，>= 6） */
    QL_UI_AREA_DEFAULT,    /* 整张图（coords 为空） */
} QLUIAreaShape;

/*
 * 一个热区。坐标是**图片像素**（原点 = 图片左上角），与 HTML `coords` 一致；
 * 运行期按图片实际绘制尺寸缩放后做命中测试（见 `ql_ui_area_at`）。
 *
 * `img` 是关联的 `<img>` 节点序号 —— 在**构建期**就把 `usemap` 与 `<map name>`
 * 对上了，所以运行期不需要按名字查表。
 */
typedef struct {
    uint16_t img;        /* 关联的 QLUINode 序号（一定是 <img>） */
    uint16_t coord_off;  /* 在 doc->area_coords[] 里的起始下标 */
    uint16_t href;       /* text 池下标；0xFFFF = 没有 */
    uint16_t alt;        /* 同上 */
    uint8_t  shape;      /* QLUIAreaShape */
    uint8_t  ncoord;     /* coords 个数（RECT=4 / CIRCLE=3 / POLY=偶数 / DEFAULT=0） */
} QLUIArea;              /* 10 字节 */

/*
 * 一张字面（`@font-face` 一条，B8）。运行期按 (family, weight, italic) 注册到
 * 字体注册表，再按字号取字体 —— 所以这里存的只是"声明"，不含任何字形数据。
 */
typedef struct {
    uint16_t family;   /* 家族名（文本池下标） */
    uint16_t src;      /* 资源名（文本池下标）：路径或 `ql_ui_register_font()` 登记的名字 */
    uint16_t weight;   /* 100..900 */
    uint8_t  italic;
    uint8_t  reserved;
} QLUIFontFace;        /* 8 字节 */

/* 一个 `font-family` 列表：names[first .. first+count) 指向文本池下标 */
typedef struct {
    uint16_t first;
    uint16_t count;
} QLUIFontList;

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
    uint16_t          area_count;       /* <area> 热区条数 */
    uint16_t          area_coord_count; /* area_coords[] 的元素个数 */
    const QLUIImageSrc *images;         /* image_count 条 */
    const uint8_t     *image_data;      /* 所有资源的字节（RAW 的像素也在这里） */
    const QLUIArea    *areas;           /* area_count 条（按 img 节点序号升序） */
    const int16_t     *area_coords;     /* area_coord_count 个 int16 */
    /*
     * `@media`：media_count 个视口宽区间；node_media 每个节点一个入口
     * （0xFFFF = 这个节点在所有区间里都一样，不占池）；media_styles 是池，
     * 每个有变体的节点占 media_count*4 个（base / hover / active / focus）。
     */
    uint16_t          media_count;
    const QLUIMedia  *media;
    const uint16_t   *node_media;
    const uint16_t   *media_styles;
    /*
     * `calc()` / `min()` / `max()` / `clamp()` 的字节码池。样式里
     * `unit == QL_UI_UNIT_CALC` 的字段用 `value` 指向这里。
     * 没用 calc 的文档长度为 0，不占 blob 空间。
     */
    uint16_t          calc_op_count;
    const QLUICalcOp *calc_ops;
    /*
     * Grid（B6）：每条 QLUIStyle 的 `grid` 是 grids[] 的下标；轨道与具名区域
     * 各自一段池，由所有 grid 容器共享（同一份模板经样式去重后只存一份）。
     * 没有 grid 的文档三段都是 0 长度。
     */
    uint16_t          grid_count;
    uint16_t          grid_track_count;
    uint16_t          grid_area_count;
    /*
     * 文本栈（B8）：font_faces 是 @font-face 声明；font_lists 是
     * `font-family` 解析出来的家族列表（样式里的 font_family 是它的下标，
     * 0 恒为"默认"空列表）。
     */
    uint16_t          font_face_count;
    uint16_t          font_list_count;
    uint16_t          font_name_count;
    const QLUIFontFace *font_faces;
    const QLUIFontList *font_lists;
    const uint16_t     *font_names;   /* 文本池下标序列 */

    /* transform 矩阵池（M9.4）：样式的 `xform` 是它的下标 */
    uint16_t          xform_count;
    /*
     * `clip-path: polygon()` 顶点池（blob v34）：每个顶点 2 个 QLUILength
     * （x 百分比相对边框盒宽，y 相对高）。样式的 `clip_pts` 是起始下标，
     * `clip_pt_n` 是个数。
     */
    uint16_t          clip_pt_count;
    const QLClipPt   *clip_pts;
    const QLUIGrid      *grids;
    const QLUIGridTrack *grid_tracks;
    const QLUIGridArea  *grid_areas;
    const QLUIXform     *xforms;
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
    /*
     * 运行期改过的样式副本（懒分配，NULL = 没改过）。
     * `QLUIStyle` 现在 400 字节，绝大多数节点一辈子用不到它 ——
     * 内嵌的话每个节点白背 400 B（MCU 上是几千节点的量级）。
     */
    QLUIStyle       *style_copy;

    const char      *text;

    /*
     * 布局期解析好的四边（px）。`QLUIStyle` 里的 `margin`/`padding` 在含相对单位
     * 时是**原始数值**，这里才是能直接参与算术的 px —— 布局代码一律读这里，
     * 解析在 `resolve_node_edges()` 里做（按包含块宽度，只在换基准时重算）。
     */
    QLUIEdges        margin_px;
    QLUIEdges        padding_px;
    ql_coord_t       edges_basis;   /* 上面那份是按哪个包含块宽度算的 */
    uint8_t          edges_valid;

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
    /*
     * `scroll-behavior: smooth` 的动画状态：`scroll_goal_ms != 0` 表示还在滑。
     * 由 `ql_ui_anim_tick()` 逐帧推进（与 CSS 动画同一个时钟）。
     */
    ql_coord_t       scroll_from_x;
    ql_coord_t       scroll_from_y;
    ql_coord_t       scroll_goal_x;
    ql_coord_t       scroll_goal_y;
    uint32_t         scroll_goal_ms;   /* 0 = 没有进行中的平滑滚动 */
    ql_coord_t       scroll_max_x;
    ql_coord_t       scroll_max_y;

    /*
     * `@media` 断点下的状态变体下标（0xFFFF = 用 doc 里那份）。
     * 只有 doc 带媒体查询、且这个节点在该断点下与基础样式不同时才会被填。
     * 之所以要单独存，是因为"响应式尺寸"与":hover 变体"必须同时生效 ——
     * 否则一悬停就会把手机布局还原成桌面布局。
     */
    uint16_t         m_hover;
    uint16_t         m_active;
    uint16_t         m_focus;

    /* 多栏容器的布局结果（绘制 column-rule 要用；0 = 不是多栏） */
    int16_t          col_n;
    int16_t          col_w;

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
    QL_UI_RT_CHECKED = 1u << 5,   /* 复选/单选/选项当前的勾选状态（B2） */
    QL_UI_RT_DISABLED = 1u << 6,  /* 禁用：不接焦点、不接输入（B2） */
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
    /*
     * 延迟与缓动在这里自己算（不交给 qlanim）：qlanim 只按**线性**推进
     * "延迟 + 时长"这段时间，真正的进度由 `trans_apply()` 按墙钟算出来再套缓动。
     * 这样 delay / steps / cubic-bezier 都不用改动画引擎。
     */
    uint32_t           start_ms;   /* 延迟结束、真正开始动的时刻 */
    uint16_t           dur_ms;
    uint8_t            steps;
    uint8_t            bez_on;
    int16_t            bez[4];
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

/* ============================== 触摸手势 ============================== */

/* 一个触摸点（界面层自己维护：后端只报"哪根手指在哪、还有几根"） */
typedef struct {
    int32_t    id;
    ql_coord_t x, y;
    uint8_t    active;
} QLUITouchPoint;

/* 捏合（双指）回调的相位 */
enum {
    QL_UI_PINCH_BEGIN  = 0,
    QL_UI_PINCH_UPDATE = 1,
    QL_UI_PINCH_END    = 2,
};

/*
 * 双指捏合回调。`scale_fx` 是**相对手势开始时的倍数**（26.6 定点，
 * `QL_SCALE_ONE` = 1.0），`cx/cy` 是两指中心（应用坐标 = CSS px）。
 *
 * 库**不替你决定缩放什么** —— 要缩放整页就把它喂给 `ql_gfx_gl_set_zoom()`，
 * 要缩放某个元素就自己换算。这样一处识别、两种用法都不吃亏。
 */
typedef void (*ql_ui_pinch_cb)(QLUIRoot *ui, ql_coord_t cx, ql_coord_t cy,
                               ql_coord_t scale_fx, int phase, void *user);

typedef void (*ql_ui_click_cb)(QLUIRoot *ui, struct QLUINodeRT *node, void *user_data);

/* 图像热区被点击（`<area>`）；`index` 是 doc->areas 下标 */
typedef void (*ql_ui_area_cb)(QLUIRoot *ui, int index, void *user);

/* 输入框的值变化 / 回车回调（定义在这里是因为 QLUIRoot 要存函数指针） */
typedef void (*ql_ui_input_cb)(QLUIRoot *ui, QLUINodeRT *node, const char *value,
                               void *user);

/*
 * 剪贴板：**平台后端接口**，和图像后端同一个形状。
 *
 * X11 的剪贴板是点对点的异步协议（粘贴要等对方回 SelectionNotify），
 * 所以 `get_text` 返回 0 表示"还没到，下一帧再问" —— 界面层会记住这次粘贴，
 * 在 `ql_ui_anim_tick()` 里重试。没装后端时 Ctrl+C/V 什么也不做（不报错）。
 */
typedef struct {
    void (*set_text)(const char *utf8, void *user);
    /* 1 = 内容已就绪（写进 buf）；0 = 还没到（内部已发请求，稍后再问） */
    int  (*get_text)(char *buf, size_t cap, void *user);
} QLUIClipboard;

void        ql_ui_set_clipboard(QLUIRoot *ui, const QLUIClipboard *cb, void *user);

/* 复制 / 剪切 / 粘贴（操作的是当前焦点输入框的选区；没有选区时复制全文） */
ql_err_t    ql_ui_clipboard_copy(QLUIRoot *ui);
ql_err_t    ql_ui_clipboard_cut(QLUIRoot *ui);
ql_err_t    ql_ui_clipboard_paste(QLUIRoot *ui);

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

    /* ----- 触摸手势（没人碰屏幕时全为 0，零额外开销） ----- */
    QLUITouchPoint touches[QL_CFG_UI_TOUCH_MAX];
    uint8_t        touch_count;      /* 还按着几根手指 */
    uint8_t        touch_moved;      /* 这一轮手势已经超过 slop（算拖动、不算点击） */
    uint8_t        touch_ignore;     /* 捏合结束后剩下的手指不再当鼠标，直到全抬起 */
    int32_t        touch_primary;    /* 单指手势是哪根手指（id） */
    ql_coord_t     touch_x0, touch_y0;    /* 按下点（判断 slop） */
    ql_coord_t     touch_x, touch_y;      /* 上一次位置（算位移） */
    QLUINodeRT    *touch_scroll;      /* 拖动滚动目标（按下时定下，之后不再变） */
    ql_coord_t     touch_scroll_x0, touch_scroll_y0;   /* 目标在按下时的安全位置 */
    uint8_t        pinch_active;
    ql_coord_t     pinch_span0;       /* 手势开始时的两指间距 */
    ql_ui_pinch_cb pinch_cb;
    void          *pinch_user;

    /*
     * Grid 布局的临时区（只增不减；文档里没有 grid 容器时全是 NULL）。
     * 放这里而不是放栈上：一个 grid 容器最多 32 列 × 64 行，压在栈上会
     * 跟"递归布局"叠乘起来。
     *
     * **按栈的纪律切分**：`*_used` 是当前的 bump 指针。grid 可以嵌套，
     * 而测量阶段会递归进入内层 grid —— 共用一个固定数组的话，内层会把
     * 外层刚算好的列宽冲掉（实测表现：内层列宽正确、外层整体缩小一半）。
     * 所以每次进 `layout_grid_children` 先记下 used、用完再还原。
     */
    void          *grid_place;      /* 每项的 (col,row,cspan,rspan) */
    uint16_t       grid_place_cap;
    uint16_t       grid_place_used;
    ql_coord_t    *grid_size;       /* [列宽][列起点][行高][行起点] */
    uint32_t       grid_size_cap;   /* 容量（ql_coord_t 个数） */
    uint32_t       grid_size_used;
    uint8_t       *grid_occ;        /* 占用位图（列主序，只有放置阶段用） */

    /* 输入框池：doc 里没有 <input> 时为 NULL（零字节） */
    struct QLUIInput *inputs;      /* 输入框池（没有 <input>/<textarea> 时为 NULL） */
    uint16_t       input_count;
    char          *ta_arena;       /* <textarea> 的文本 arena（没有则为 NULL） */
    uint16_t       ta_count;
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
    /* 剪贴板后端（可为 NULL：Ctrl+C/V 就什么也不做） */
    const QLUIClipboard *clipboard;
    void          *clipboard_user;
    uint8_t        paste_pending;    /* 已发出粘贴请求，等内容到（异步） */
    ql_ui_area_cb  area_cb;          /* 热区点击回调（可为 NULL） */
    void          *area_user;
    /*
     * 模板实例化（`ql_ui_repeat`）替 `{i}` 生成的字符串。归 root 所有，
     * 因为 `QLUINodeRT.text` 只存指针（不复制）。
     */
    char         **rep_texts;
    uint16_t       rep_count;
    uint16_t       rep_cap;

    /*
     * 输入法**预编辑**（组字中、还没提交的文本）。归 root 所有，因为只有
     * 一个输入框在组字。`preedit_len == 0` 表示没有。
     * 预编辑不算输入框的值：提交时才通过 `QL_INPUT_TEXT` 进值缓冲。
     */
    char           preedit[QL_CFG_UI_INPUT_LEN * 2];
    uint16_t       preedit_len;      /* 字节数（不含结尾 0） */
    uint16_t       preedit_caret;    /* 光标：**字节**偏移，0..preedit_len */
};

/* ----- 生命周期 ----- */
QLUIRoot   *ql_ui_build(const QLUIDoc *doc);
void        ql_ui_free(QLUIRoot *ui);

/*
 * 模板实例化：把 `tpl` 这棵子树复制成 `count` 份（含原来那份），
 * 追加成它的兄弟，并把每份文本里的 `{i}` 换成 **1 起的序号**。
 *
 * 为什么是"运行期复制子树"而不是"编译期展开"：列表长度常常要到运行时才知道
 * （从文件/网络读到几行）。而样式与节点定义在 doc 里是共享只读的，复制一份
 * 子树只是 `count × 子树节点数` 个 `QLUINodeRT`（192 B/节点），没有额外 CSS 成本。
 *
 * 约定：
 *   - **必须在 `ql_ui_build()` 之后、`ql_ui_layout()` 之前调用**（节点数会变）；
 *   - `tpl` 必须有父节点（根节点的"模板"没有意义）；
 *   - `count == 0` 时相当于把模板删掉（子树节点数不变，只是标记 `display:none`）；
 *   - 同一棵树可以多次调用（各自追加）。
 *
 * 返回 QL_OK / QL_ERR_INVAL / QL_ERR_NOMEM / QL_ERR_FULL（节点数超过 uint16）。
 *
 * ⚠️ **调用之后所有 `QLUINodeRT*` 都可能失效**（节点数组会重新分配）。
 * 拿新指针的办法：`ql_ui_root_node(ui)` 然后顺着 `first_child` / `next_sibling` 走，
 * 或者直接用 `ql_ui_find_by_tag()`。
 */
ql_err_t    ql_ui_repeat(QLUIRoot *ui, QLUINodeRT *tpl, uint16_t count);

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
/*
 * 当前位置该用哪个指针形状（QLUICursor）。命中节点往上找第一个写了 `cursor`
 * 的祖先；都没写时输入框给 TEXT、其余给 DEFAULT。
 *
 * "只算意图，不设光标"：宿主拿它去设窗口光标（X11 `XDefineCursor` /
 * Wayland `wl_cursor`）。MCU 上忽略返回值即可。
 */
uint8_t     ql_ui_cursor_at(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
const char *ql_ui_node_text(const QLUINodeRT *node);
const QLFont *ql_ui_node_font(const QLUINodeRT *node);
/* 生效样式：ACTIVE > FOCUS > HOVER > 基础（只影响绘制，不影响布局） */
const QLUIStyle *ql_ui_node_style(const QLUINodeRT *node);
QLRect      ql_ui_node_rect(const QLUINodeRT *node);
const char *ql_ui_tag_name(uint16_t tag);

/*
 * 运行期只允许改文本（结构在构建期固定，见 Web兼容设计 §12.5）。
 *
 * **不复制**：`text` 的指针直接存进节点，**必须一直有效**（字面量、
 * 静态缓冲、或你自己管理生命周期的字符串）。传栈上局部缓冲 = 画面出乱码
 * （指针悬空了，不崩已经是运气）。本库零分配是刻意的，别为了图方便复制。
 */
ql_err_t    ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text);

/* 交互状态：命中测试 + 状态位 + 点击回调 */
void        ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data);

/*
 * 双指捏合：接上回调就能收到 BEGIN / UPDATE / END。
 *
 * 触摸事件由后端原样上报（`QL_INPUT_TOUCH`），**单指当鼠标、双指当捏合**
 * 的判定在界面层做：单指按在可滚动容器里拖动超过 `QL_CFG_UI_TOUCH_SLOP`
 * 就改成拖内容滚动（手机没有滚轮），第二根手指落下则把这次按下作废
 * （不合成 click）并转入捏合。
 */
void        ql_ui_set_pinch_cb(QLUIRoot *ui, ql_ui_pinch_cb cb, void *user);


/* ------------------------------ 输入框 ------------------------------ */

/*
 * `<input>` 的值存在**运行期池**里（`ql_ui_build` 时按需分配），长度上限
 * `QL_CFG_UI_INPUT_LEN - 1` 字节（UTF-8）。这是本库唯一一处"运行期会变的文本"。
 *
 * 编辑：点击定位光标、拖动选字、Backspace/Delete/方向键/Home/End、
 * Shift+方向扩展选区、`QL_INPUT_TEXT` 插入字符。剪贴板走下面的
 * `QLUIClipboard` 后端接口（没装就不响应 Ctrl+C/V，不报错）；
 * 光标**不闪**（闪烁要主循环一直醒着，省电优先）。
 *
 * `<textarea>` 用**同一套** API，只是文本存在共享 arena 里、光标按视觉行定位。
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

/* ------------------------------ 图像热区 ------------------------------ */

/*
 * `<map name="m"><area shape=… coords=… href=…></map>` + `<img usemap="#m">`：
 * 一张图上可以有几块可点区域。
 *
 * **几何在构建期就定型**：解析时把 `usemap` 与 `<map name>` 对上，热区条目直接
 * 记下 `<img>` 的节点序号，所以运行期没有"按名字查表"，也没有字符串比较。
 * 坐标是图片像素（原点 = 图片左上角），命中测试时按图片**实际绘制尺寸**缩放
 * （`object-fit` 生效后的矩形）。
 *
 * 热区不是节点：`<map>` / `<area>` 的 UA 样式是 `display:none`，不产生盒子。
 * 点击回调见 `ql_ui_set_area_cb`。
 */
/* 命中的热区下标（doc->areas 下标）；没命中 / 没有热区返回 -1 */
int         ql_ui_area_at(const QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int         ql_ui_area_count(const QLUIRoot *ui);
/* 第 index 条热区的只读描述；越界返回 NULL */
const QLUIArea *ql_ui_area_get(const QLUIRoot *ui, int index);
/* 热区所属的 <img> 节点；index 越界返回 NULL */
const QLUINodeRT *ql_ui_area_image(const QLUIRoot *ui, int index);
/* 热区在屏幕上（当前布局）的包围盒；index 越界返回空矩形 */
QLRect      ql_ui_area_rect(const QLUIRoot *ui, int index);
/* `href` / `alt`；没有返回 NULL */
const char *ql_ui_area_href(const QLUIRoot *ui, int index);
const char *ql_ui_area_alt(const QLUIRoot *ui, int index);

/* 热区被点击（指针在热区内抬起）：*替代* click_cb，不再重复派发 */
void        ql_ui_set_area_cb(QLUIRoot *ui, ql_ui_area_cb cb, void *user);

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

/*
 * 输入法预编辑（IME preedit）：组字中的文本，**不改**输入框的值。
 *
 * `utf8 == NULL` 或空串表示组字结束。`caret_cp` 是光标在预编辑文本里的
 * **码点**下标（XIM 的 `XNPreeditCaretCallback` 就是这么给的），< 0 表示放到末尾。
 *
 * 为什么要单独一条：`XIMPreeditCallbacks` 风格下由**客户端**负责画组字文本，
 * 输入法只画候选窗。不画的话用户敲拼音时输入框里什么都不动（只剩候选窗），
 * 而画在输入法那边就会和候选窗里的预编辑行重复（两个预览框）。
 */
void        ql_ui_input_set_preedit(QLUIRoot *ui, const char *utf8, int caret_cp);
/*
 * 增量修改预编辑（XIM 的 `XNPreeditDrawCallback` 语义）：把从第 `chg_first` 个
 * **码点**开始的 `chg_len` 个码点替换成 `insert`（可为 NULL = 纯删除）。
 * 输入法发来的是片段改动而不是整串，按整串处理会丢字、还可能劈开半个 UTF-8。
 */
void        ql_ui_input_preedit_apply(QLUIRoot *ui, int chg_first, int chg_len,
                                      const char *insert, int caret_cp);
/* 取当前的预编辑（没有则返回 NULL）；caret_cp 可为 NULL */
const char *ql_ui_input_preedit(const QLUIRoot *ui, int *caret_cp);

/*
 * 光标在屏幕上的位置（CSS px，原点 = 视口左上角）。给**输入法**定位候选窗用。
 *
 * 返回 1 表示有聚焦的输入框，`out_x/out_y` 是光标左上角、`out_h` 是光标高度
 * （输入法要的 spot 一般是"光标左下角"，即 `(x, y + h)`）。没有聚焦输入框返回 0。
 *
 * 为什么需要它：XIM 的 `XIMPreeditNothing` 风格下候选窗由输入法自己画，它靠
 * `XNSpotLocation` 知道往哪儿画。不告诉它，候选窗会跑到窗口左上角/鼠标附近
 * （实测是窗口下方一大截），看起来就是"提示框位置不对"。
 */
int         ql_ui_ime_spot(const QLUIRoot *ui, ql_coord_t *out_x, ql_coord_t *out_y,
                           ql_coord_t *out_h);

/* ===================== 二进制 blob（.qlui） ===================== */

/*
 * 格式（little-endian，各段按 8 字节对齐）：
 *
 *   [ QLUIDocBlobHeader 124 字节 ]
 *   [ nodes      ] node_count      * sizeof(QLUINode)
 *   [ styles     ] style_count     * sizeof(QLUIStyle)
 *   [ text_pool  ] 以 '\0' 分隔的 UTF-8 字节
 *   [ text_ofs   ] text_count      * uint16
 *   [ font_sizes ] font_size_count * uint16
 *   [ anims      ] anim_count      * sizeof(QLUIAnimation)
 *   [ keyframes  ] keyframe_count  * sizeof(QLUIKeyframe)
 *   [ images     ] image_count     * sizeof(QLUIImageSrc)
 *   [ image_data ] image_data_size 字节（编码后的资源字节）
 *   [ areas      ] area_count      * sizeof(QLUIArea)
 *   [ area_coords] area_coord_count * int16
 *   [ media      ] media_count     * sizeof(QLUIMedia)        （v21）
 *   [ node_media ] node_count      * uint16                   （v21）
 *   [ media_styles] media_pool_count * uint16                 （v21）
 *   [ calc_ops   ] calc_op_count    * sizeof(QLUICalcOp)      （v22）
 *   [ grids      ] grid_count       * sizeof(QLUIGrid)        （v28）
 *   [ grid_tracks] grid_track_count * sizeof(QLUIGridTrack)   （v28）
 *   [ grid_areas ] grid_area_count  * sizeof(QLUIGridArea)    （v28）
 *   [ xforms     ] xform_count      * sizeof(QLUIXform)       （v30）
 *   [ font_faces ] font_face_count  * sizeof(QLUIFontFace)    （v33）
 *   [ font_lists ] font_list_count  * sizeof(QLUIFontList)    （v33）
 *   [ font_names ] font_name_count  * uint16                  （v33）
 *   [ clip_pts   ] clip_pt_count    * sizeof(QLClipPt)        （v34）
 *   [ name       ] name_len 字节（不含 '\0'）
 *
 * 之所以能"直接绑指针"，是因为 doc 内部全是 uint16 下标。
 * 但结构体内存布局受编译器/ABI 影响，所以 blob 不是跨架构可移植的：
 * 头里的 struct_size / abi 字段会在加载时逐项校验，不匹配就明确拒绝。
 */
#define QL_UI_BLOB_MAGIC      0x49554C51u   /* 'Q','L','U','I'（小端） */
/*
 * 28: + Grid（B6）：`grid` / `grid_col` / `grid_row` / `grid_*_span` / `grid_flow` /
 *     `justify_items` / `justify_self` / `grid_auto_*`，新增 grids / grid_tracks /
 *     grid_areas 三段池；+ `border-image`（源/切片/宽度/填充方式）。
 *     QLUIStyle 272 → **312**
 * 27: + word-spacing / tab-size / text-decoration-*（B3）、
 *     :disabled / :checked / ::placeholder（B2），QLUINode 20 → 28
 * 26: 四边独立 border-*（宽/样式/色）、四角 radius、box-shadow 两层 + inset、
 *     text-shadow、image-rendering、scrollbar-width/color、object-position
 * 34: + clip-path 的 ellipse / inset / polygon（clip_box[4] / clip_pts / clip_pt_n）；
 *     `font-variant-caps: small-caps` / `font-variant-numeric: tabular-nums`
 *     （variant_flags）；`border-radius: a / b` 椭圆圆角（radius_v[4]）；
 *     `box-shadow` 的 spread（复用 QLUIShadow.reserved）。QLUIStyle 368 → **396**
 * 38: + `backdrop-filter: refract(<N>px)`（backdrop_refract）与 `translate(%%)` 的
 *     `tf_pct` 标记。QLUIStyle 布局变了 → 所有 golden 一起重建
 * 25: + border-style、outline（width/color/offset/style）
 * 24: + overflow-x/y、align-content、cursor、user-select、scroll-behavior、position: sticky
 */
#define QL_UI_BLOB_VERSION    38u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;         /* 固定 180（v34） */
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
    uint16_t area_count;          /* <area> 热区条数 */
    uint16_t area_coord_count;    /* area_coords[] 元素个数 */
    uint32_t off_areas;           /* QLUIArea[] */
    uint32_t off_area_coords;     /* int16[]，全部热区共用一段 */
    uint16_t media_count;         /* @media 区间数（0 = 没有媒体查询） */
    uint16_t reserved_media;      /* 对齐填充 */
    uint32_t off_media;           /* QLUIMedia[media_count] */
    uint32_t off_node_media;      /* uint16[node_count] */
    uint32_t off_media_styles;    /* uint16[media_pool_count] */
    uint32_t media_pool_count;
    uint16_t calc_op_count;       /* calc() 字节码条数（0 = 没用 calc） */
    uint16_t reserved_calc;       /* 对齐填充 */
    uint32_t off_calc_ops;        /* QLUICalcOp[calc_op_count] */
    uint16_t grid_count;          /* Grid 模板条数（v28） */
    uint16_t grid_track_count;    /* 轨道总数（所有模板共用一段） */
    uint16_t grid_area_count;     /* 具名区域总数 */
    uint16_t reserved_grid;       /* 对齐填充 */
    uint32_t off_grids;           /* QLUIGrid[grid_count] */
    uint32_t off_grid_tracks;     /* QLUIGridTrack[grid_track_count] */
    uint32_t off_grid_areas;      /* QLUIGridArea[grid_area_count] */
    /* 文本栈（B8，v33） */
    uint16_t font_face_count;     /* @font-face 条数 */
    uint16_t font_list_count;     /* font-family 列表条数（不含 0 号默认） */
    uint16_t font_name_count;     /* font_names[] 的元素个数 */
    uint16_t reserved_font;       /* 对齐填充 */
    uint32_t off_font_faces;      /* QLUIFontFace[font_face_count] */
    uint32_t off_font_lists;      /* QLUIFontList[font_list_count] */
    uint32_t off_font_names;      /* uint16[font_name_count] */
    uint16_t xform_count;         /* transform 矩阵条数（v30） */
    uint16_t reserved_xform;      /* 对齐填充 */
    uint32_t off_xforms;          /* QLUIXform[xform_count] */
    /* clip-path 多边形顶点（v34） */
    uint16_t clip_pt_count;       /* clip_pts[] 元素个数 */
    uint16_t reserved_clip;       /* 对齐填充 */
    uint32_t off_clip_pts;        /* QLClipPt[clip_pt_count] */
} QLUIDocBlobHeader;

_Static_assert(sizeof(QLUIDocBlobHeader) == 180, "blob 头必须是 180 字节（v34）");
_Static_assert(sizeof(QLUIKeyframe) == 44, "关键帧必须是 44 字节（blob 顺序存放）");

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
