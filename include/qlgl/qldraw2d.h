/**
 * @file    qldraw2d.h
 * @brief   2D 绘制接口：CPU 光栅与 GPU 后端共用的唯一分界。
 *
 * 上层（qlui 布局/控件）只认识这个接口，因此同一份界面可以：
 *   - 用 CPU 画到帧缓冲（MCU / 无 GPU / 离屏导出）
 *   - 用 GPU 画到窗口（PC / 移动）
 * 两个后端的输出必须一致，用 golden image 测试守住。
 *
 * 坐标一律是"目标表面的像素坐标"，原点左上，y 向下。
 */
#ifndef QLGL_DRAW2D_H
#define QLGL_DRAW2D_H

#include "qlgl.h"
#include "qlgeom.h"
#include "qlfont.h"
#include "qlcanvas.h"

typedef struct QLDraw2D QLDraw2D;

/* 装饰线（`text-decoration`）：两条都是 1px，画在基线附近 */
#define QL_TEXT_DECO_UNDERLINE    0x01u
#define QL_TEXT_DECO_LINE_THROUGH 0x02u

/*
 * 一次文本绘制的全部参数。
 *
 * 收成一个结构体是因为参数已经到 8 个了，再加"字体变体/装饰线"就该爆炸。
 * 后端实现只读它，不要往里写。
 *
 * `bold` / `italic` 是**合成**的（不换字体文件）：
 *   - 加粗：把字形再画一遍、右移 1px（等价于 FreeType 的 embolden 的穷人版）；
 *   - 斜体：按行剪切（顶部右移、底部不动），斜率约 0.21。
 * 位数少、效果够用、MCU 也不额外占空间 —— 换真字体文件在 MCU 上不现实。
 */
typedef struct {
    ql_color_t color;
    ql_coord_t line_h;    /* 行高（px）；0 = normal（字体自身的 line_height） */
    ql_coord_t baseline_dy; /* 基线额外下移（px，负数=上移）。`<sup>` 用负值 */
    uint8_t    align;     /* QLTextAlign */
    uint8_t    wrap;      /* 1 = 按 box.w 折行；0 = 只按 '\n' 断 */
    uint8_t    bold;
    uint8_t    italic;
    uint8_t    deco;      /* QL_TEXT_DECO_* 位图 */
    /*
     * 1 = 每行**最后一段**的尾随空格也保留宽度。
     * 普通文本不该留（行尾空格不占宽，对齐才好看），但输入框必须留：
     * 否则在末尾敲空格时光标不动、屏幕上什么也没发生。
     */
    uint8_t    keep_trailing;
    /*
     * CSS 文本细节（`letter-spacing` / `text-indent` / `word-break` / `text-overflow`）。
     * 全部零初始化即为"不生效"，所以老代码 memset 之后的语义不变。
     */
    ql_coord_t letter_spacing;  /* 字间距（px，可为负），加在**每个字符之后** */
    ql_coord_t indent;          /* 首行缩进（px） */
    uint8_t    break_any;       /* 1 = 任意字符之间都能断行（`word-break: break-all`） */
    uint8_t    ellipsis;        /* 1 = 一行放不下时末尾换省略号（`text-overflow: ellipsis`） */
} QLTextStyle;

struct QLDraw2D {
    /* 设置裁剪矩形（绝对坐标）；之后的绘制都被裁到它里面 */
    void (*set_clip)(QLDraw2D *self, QLRect clip);

    /*
     * 整体透明度（0..255），**乘到每一个绘制入口的 alpha 上**。
     * `opacity` 就靠它：一处设置，背景/边框/阴影/文字/图片全部一起淡出，
     * 不会出现"背景淡了、边还硬着"那种半个正确。
     */
    uint8_t alpha;
    /* 圆遮罩（CSS px）。NULL = 该后端不支持，上层据此跳过该特性 */
    void (*set_mask)(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t r);

    /* alpha: 0 透明（跳过），255 不透明 */
    void (*fill_rect)(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha);
    void (*fill_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                            ql_color_t color, uint8_t alpha);
    /* 描边在矩形内侧 */
    void (*stroke_round_rect)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                              ql_coord_t thickness, ql_color_t color);

    /*
     * 线性渐变圆角矩形：from → to，dir 见 QLGradientDir。
     * GPU 侧用"顶点颜色插值"实现，所以不增加顶点、不增加 draw call。
     */
    void (*fill_round_rect_gradient)(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                     ql_color_t from, ql_color_t to, uint8_t dir,
                                     uint8_t alpha);
    /*
     * 软阴影：以 inner（圆角 radius）为轮廓，向外扩散 blur，整体偏移 (dx,dy)。
     * 调用方必须**先画阴影再画本体**。
     */
    void (*fill_shadow)(QLDraw2D *self, QLRect inner, ql_coord_t radius, ql_coord_t blur,
                        ql_coord_t dx, ql_coord_t dy, ql_color_t color, uint8_t alpha);

    /*
     * 直线：Bresenham，thickness > 1 时按方形笔刷加粗（与 ql_canvas_draw_line 同式）。
     * 两个后端都应指向公共实现 ql_draw2d_draw_line()，这样细线不会因后端差像素。
     */
    void (*draw_line)(QLDraw2D *self, ql_coord_t x1, ql_coord_t y1, ql_coord_t x2,
                      ql_coord_t y2, ql_coord_t thickness, ql_color_t color);

    /*
     * 画文本（CSS px）。`line_h` = 行高（px），0 表示 `normal`（用字体自身的
     * line_height = ascent + descent，半行距为 0，与旧行为一致）。
     * 单行垂直居中就靠它：`height: 26px; line-height: 26px`。
     */
    void (*draw_text)(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                      QLRect box, const QLTextStyle *ts);

    /*
     * 毛玻璃（CSS px）：把 `rect` 区域里**已经画好的内容**就地模糊。
     * NULL = 该后端不支持（上层跳过该效果并告警一次）。
     * `sigma` 是高斯标准差（CSS `blur()` 的语义），后端自行换算成盒式半径。
     * 必须在"画元素自己的背景之前"调用，否则会连元素自己一起模糊。
     */
    void (*backdrop_blur)(QLDraw2D *self, QLRect rect, int sigma);

    /*
     * 同上，但把模糊结果裁进圆角矩形（`corner` = 圆角半径，CSS px，0 = 直角）。
     * 可选：为 NULL 时上层退回 `backdrop_blur`，表现为圆角缺口里渗出方形模糊。
     */
    void (*backdrop_blur_round)(QLDraw2D *self, QLRect rect, int sigma,
                               ql_coord_t corner);

    /* 后端可选的批次提交（GPU 用；CPU 为 NULL） */
    /*
     * 位图搬运：把一张 QLImage 画到 dst（设备像素、已含所有变换）。
     * `src == NULL` 取整幅；否则取子矩形（给 object-fit: cover 用）。
     * 缩放、alpha 混合都在后端里做；裁剪与圆遮罩沿用 set_clip / set_mask。
     */
    /*
     * `radius` = 圆角半径（0 = 直角）。图片必须跟着 `border-radius` 裁，
     * 否则"圆形头像"这种最常见的用法就得自己套一层 mask。
     */
    void (*blit_image)(QLDraw2D *self, const QLImage *img, QLRect dst,
                       const QLRect *src, ql_coord_t radius, uint8_t alpha);

    void (*flush)(QLDraw2D *self);

    /*
     * CSS px → 设备像素的缩放（26.6 定点，QL_SCALE_ONE = 1.0），默认 QL_SCALE_ONE。
     *
     * 上层的所有坐标（qlui / 控件 / 自定义绘制）**一律是 CSS px**，
     * 只有后端在入口乘这个系数，所以 DPR 变化不需要动任何上层代码。
     * 由 ql_draw2d_canvas_init / ql_gfx_gl_draw 从"表面"继承，也可手动设。
     */
    ql_coord_t scale_fx;

    /*
     * 绘制期平移（CSS px，26.6 定点，64 = 1 CSS px）。默认 0。
     *
     * 这是动画的落点：`translate(0.5px)` 就是把它设成 32。
     * CPU 后端会把矩形转成带小数的 `QLRectFx` 走**面积覆盖率抗锯齿**，
     * GPU 后端直接加进顶点（顶点本来就是浮点，零成本）。
     * **不影响布局**（与 CSS 的 transform 语义一致），所以滑动/缩放动画零重排。
     *
     * 文本与阴影为了保持清晰/性能，只取它的整数部分。
     */
    ql_coord_t offset_x_fx;
    ql_coord_t offset_y_fx;

    /*
     * 圆遮罩（CSS px 的圆心 + 半径；**r < 0 表示关闭**，r == 0 = 全裁）。
     * CPU 后端换算成设备像素交给 QLCanvas；GPU 后端交给顶点/片元着色器。
     * 它是"裁剪"的一部分：调用方负责在子树画完后恢复。
     */
    ql_coord_t mask_cx;
    ql_coord_t mask_cy;
    ql_coord_t mask_r;      /* < 0 = 关闭 */

    /*
     * 元素缩放（`transform: scale()`，26.6 定点，QL_SCALE_ONE = 1.0）+ 缩放中心。
     * 与 DPR 的 scale_fx 是两回事：那个是设备像素比，这个是"元素自己放大/缩小"。
     * 只支持**等比**：非等比缩放会让圆角、描边、字形的语义变得没有定义。
     */
    ql_coord_t elem_x_fx;   /* 水平缩放（26.6） */
    ql_coord_t elem_y_fx;   /* 垂直缩放（26.6）；与 x 不同 = 非等比（假翻转） */
    ql_coord_t elem_px;     /* 缩放中心（CSS px） */
    ql_coord_t elem_py;
    /*
     * 文本要不要走**几何变换**（逐字形缩放/镜像）。
     *
     * 由 `draw_text` 在排版前决定，emit 时读：
     *   · 等比正缩放且**拿到了重取字号的字体**（矢量字体）→ 0，走"换字号"，清晰；
     *   · 其余（非等比、镜像、或位图字体无法重取字号）→ 1，逐字形做 affine。
     * 位图字体必须走几何路径，否则 `scale(0.5)` 只会缩盒子、字一个像素都不缩。
     */
    int        text_geo;
    ql_coord_t text_px, text_py;   /* 几何变换的中心（设备像素） */

    void *user;   /* 后端私有数据 */
};

/* 折行后的一行：指向原文里的一段（不拷贝） */
typedef struct {
    const char *start;
    int32_t     len;
} QLTextLine;

/*
 * 按像素宽度断行（`white-space: normal` 的实现，布局与绘制共用同一份）。
 *
 * 规则（刻意简单，但覆盖中英文）：
 *   1. `'\n'` 强制断行；
 *   2. 优先在**空格之后**断（空格与行尾连续空白不占宽度）；
 *   3. CJK 字符之间可以随处断（U+2E80 以上视为 CJK，与浏览器的"任意断点"一致）；
 *   4. 一个词比整行还宽时硬断（否则会死循环式溢出）。
 *
 * `out` 可以为 NULL —— 这时只数行数（布局阶段用，不必分配）。
 * 返回总行数；超过 `max_lines` 时只填前 `max_lines` 条，但返回值仍是总数。
 */
int ql_text_wrap(const QLFont *font, const char *text, ql_coord_t max_w,
                 QLTextLine *out, int max_lines);

/*
 * 同上，`keep_trailing != 0` 时保留每行最后一段的尾随空格（输入框用）。
 * 软折行点上的空格仍然去掉 —— 那是被断行"消费"掉的，不属于任何一行。
 */
int ql_text_wrap_ex(const QLFont *font, const char *text, ql_coord_t max_w,
                    QLTextLine *out, int max_lines, int keep_trailing);

/*
 * 折行的可调项（对应 CSS 的 `letter-spacing` / `text-indent` / `word-break`）。
 * 全零 = 老行为。布局数行数与绘制折行**必须传同一份**，否则行数与位置会打架。
 */
typedef struct {
    ql_coord_t letter_spacing;   /* 字间距（px，可为负） */
    ql_coord_t indent;           /* 首行缩进（px），只作用在整段的第一行 */
    uint8_t    break_any;        /* 1 = 任意字符间断行（`word-break: break-all`） */
} QLTextWrapOpt;

/* `opt` 可以为 NULL（等于全零） */
int ql_text_wrap_opt(const QLFont *font, const char *text, ql_coord_t max_w,
                     const QLTextWrapOpt *opt, QLTextLine *out, int max_lines,
                     int keep_trailing);

/* 单行绘制文字的公共实现：后端可直接复用它的排版逻辑 */
typedef void (*ql_draw2d_glyph_fn)(QLDraw2D *self, const QLFont *font,
                                   const QLGlyph *glyph, ql_coord_t pen_x,
                                   ql_coord_t baseline_y, const QLTextStyle *ts);

/*
 * 排一行/多行并逐字发射。`wrap` / `line_h` / 装饰线都在 `ts` 里。
 * 装饰线由这个函数自己画（它知道每行的起止与基线），所以两个后端不用各写一遍。
 */
void ql_draw2d_text_layout(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                           QLRect box, const QLTextStyle *ts,
                           ql_draw2d_glyph_fn emit_glyph);

/*
 * 直线的**公共实现**：CPU 与 GPU 后端都应把 draw_line 指向它。
 * 只用 fill_rect 组合，因此两个后端的几何天然一致（不会因为各自的光栅化差像素）。
 */
void ql_draw2d_draw_line(QLDraw2D *self, ql_coord_t x1, ql_coord_t y1, ql_coord_t x2,
                         ql_coord_t y2, ql_coord_t thickness, ql_color_t color);

/* ----- CPU 后端（包一层 QLCanvas） ----- */
ql_err_t ql_draw2d_canvas_init(QLDraw2D *out, QLCanvas *canvas);

/*
 * 按缩放取"设备像素字号"的同族字体：DPR>1 时字形必须按设备字号重新光栅化，
 * 否则会被拉伸变糊。只有动态字体能真正重取；位图字体原样返回。
 */
const QLFont *ql_draw2d_device_font(const QLFont *font, ql_coord_t scale_fx);

/* 绘制期平移（CSS px，26.6 定点）。下一帧的绘制整体偏移这么多 */
/*
 * 圆遮罩；**r < 0 关闭**（r == 0 = 全裁）。单位与其它绘制入口一致：CSS px。
 *
 * 走 vtable（像 set_clip 一样）而不是纯内联：CPU 后端要把它换算成设备像素
 * 推到 QLCanvas 的像素汇点上，GPU 后端要写进顶点属性。
 */
/* 毛玻璃。后端不支持时返回 0（调用方据此跳过自身背景的半透明处理） */
/* 整体透明度（见 QLDraw2D.alpha）。0 = 全都别画 */
static inline void ql_draw2d_set_alpha(QLDraw2D *d2d, uint8_t alpha)
{
    if (d2d != NULL)
        d2d->alpha = alpha;
}

/* 入口 alpha × 整体 alpha */
static inline uint8_t ql_draw2d_eff_alpha(const QLDraw2D *d2d, uint8_t alpha)
{
    if (d2d == NULL || d2d->alpha >= 255u)
        return alpha;
    return (uint8_t)(((uint32_t)alpha * (uint32_t)d2d->alpha + 127u) / 255u);
}

static inline int ql_draw2d_backdrop_blur(QLDraw2D *d2d, QLRect rect, int sigma)
{
    if (d2d == NULL || d2d->backdrop_blur == NULL || sigma <= 0)
        return 0;
    if (ql_rect_is_empty(rect))
        return 0;
    d2d->backdrop_blur(d2d, rect, sigma);
    return 1;
}

/*
 * 毛玻璃 + 圆角裁剪。`corner <= 0` 或后端没有圆角版本时退回方形模糊。
 * 返回 1 表示做了模糊（调用方据此调整自身背景）。
 */
static inline int ql_draw2d_backdrop_blur_round(QLDraw2D *d2d, QLRect rect,
                                                int sigma, ql_coord_t corner)
{
    if (corner > 0 && d2d != NULL && d2d->backdrop_blur_round != NULL &&
        sigma > 0 && !ql_rect_is_empty(rect)) {
        d2d->backdrop_blur_round(d2d, rect, sigma, corner);
        return 1;
    }
    return ql_draw2d_backdrop_blur(d2d, rect, sigma);
}

static inline void ql_draw2d_set_mask(QLDraw2D *d2d, ql_coord_t cx, ql_coord_t cy,
                                      ql_coord_t r)
{
    if (d2d == NULL)
        return;
    d2d->mask_cx = cx;
    d2d->mask_cy = cy;
    d2d->mask_r = (r < 0) ? -1 : r;
    if (d2d->set_mask != NULL)
        d2d->set_mask(d2d, cx, cy, d2d->mask_r);
}

static inline void ql_draw2d_set_offset(QLDraw2D *d2d, ql_coord_t x_fx, ql_coord_t y_fx)
{
    if (d2d != NULL) {
        d2d->offset_x_fx = x_fx;
        d2d->offset_y_fx = y_fx;
    }
}

/*
 * 元素缩放（`transform: scale()`）：绕 (px, py) 等比缩放后续的所有绘制。
 * CSS px 的"长度"（半径、描边宽度、偏移）也一起缩放，与浏览器一致。
 */
static inline void ql_draw2d_set_elem_scale(QLDraw2D *d2d, ql_coord_t fx,
                                            ql_coord_t px, ql_coord_t py)
{
    if (d2d == NULL)
        return;
    d2d->elem_x_fx = (fx > 0) ? fx : QL_SCALE_ONE;
    d2d->elem_y_fx = d2d->elem_x_fx;
    d2d->elem_px = px;
    d2d->elem_py = py;
}

/* 非等比：水平/垂直分开给。`backface-visibility` 的假翻转就靠这个 */
static inline void ql_draw2d_set_elem_scale_xy(QLDraw2D *d2d, ql_coord_t fx,
                                               ql_coord_t fy, ql_coord_t px,
                                               ql_coord_t py)
{
    if (d2d == NULL)
        return;
    /*
     * 0 是合法值（压到零宽）；**负值也是**（镜像，翻转动画的第二半程）。
     * 这里只挡"非法"，不挡符号 —— 符号就是翻转的方向。
     */
    d2d->elem_x_fx = fx;
    d2d->elem_y_fx = fy;
    d2d->elem_px = px;
    d2d->elem_py = py;
}

/* 直接改缩放（26.6 定点）。后端实现只在入口乘它，不缓存换算后的坐标 */
static inline void ql_draw2d_set_scale(QLDraw2D *d2d, ql_coord_t scale_fx)
{
    if (d2d != NULL)
        d2d->scale_fx = (scale_fx > 0) ? scale_fx : QL_SCALE_ONE;
}

#endif /* QLGL_DRAW2D_H */
