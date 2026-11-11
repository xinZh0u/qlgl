/**
 * @file    qlcanvas.h
 * @brief   画布：一组带裁剪的绘制原语，直接写入显示驱动的帧缓冲。
 *
 * 坐标一律是"屏幕绝对坐标"，裁剪由 canvas->clip 负责。
 * v0.1 不提供图层合成，只支持"按 alpha 与已有像素混合"。
 */
#ifndef QLGL_CANVAS_H
#define QLGL_CANVAS_H

#include "qlgl.h"
#include "qlgeom.h"
#include "qldisp.h"
#include "qlfont.h"

typedef struct {
    uint8_t  *buffer;    /* 帧缓冲起点 */
    uint32_t  stride;    /* 字节/行 */
    ql_coord_t width;    /* 缓冲宽度（像素） */
    ql_coord_t height;   /* 缓冲高度（像素） */
    QLRect    clip;      /* 裁剪区，设备像素 */
    uint8_t   format;    /* QLColorFormat */

    /*
     * 这块表面相对 CSS px 的缩放（26.6 定点，见 qlgeom.h 的 QL_SCALE_ONE）。
     * 画布本身只认设备像素；这个字段是给 QLDraw2D 继承用的：
     * 布局在 CSS px，绘制入口乘它。由 ql_canvas_from_disp 从驱动继承。
     */
    ql_coord_t scale_fx;

    /*
     * 椭圆遮罩（设备像素）；**mask_r < 0 表示关闭**（== 0 表示全裁掉）。
     * rx == ry 时就是圆。见 ql_canvas_set_mask
     */
    ql_coord_t mask_cx;
    ql_coord_t mask_cy;
    ql_coord_t mask_r;
    ql_coord_t mask_ry;

    /*
     * 多边形遮罩（`clip-path: polygon()`，设备像素，绝对坐标）。
     * `mask_pn >= 3` 时生效，此时椭圆遮罩必然关闭（两者互斥，只有一层）。
     * 覆盖率按"到最近边的距离"算，所以边是解析抗锯齿的、与元素尺寸无关。
     */
    uint8_t    mask_pn;
    float      mask_px[QL_CFG_UI_CLIP_PTS_MAX];
    float      mask_py[QL_CFG_UI_CLIP_PTS_MAX];
    float      mask_pmin_x, mask_pmin_y, mask_pmax_x, mask_pmax_y;   /* 包围盒 */

    /*
     * 颜色矩阵（`filter: brightness()/contrast()/...` 的**单批次**实现）。
     * 3x4 行主序、16.16 定点：前 3 行是 rgb→rgb 的线性部分，第 4 列是偏移。
     * `cm_on == 0` 时完全不参与运算（快路径不受影响）。
     */
    int32_t    cm[12];
    uint8_t    cm_on;
} QLCanvas;

/* 由显示驱动构造画布，clip 默认整屏 */
ql_err_t ql_canvas_from_disp(QLCanvas *canvas, QLDispDriver *driver);
ql_err_t ql_canvas_from_disp_clip(QLCanvas *canvas, QLDispDriver *driver, QLRect clip);

void ql_canvas_set_clip(QLCanvas *canvas, QLRect clip);
QLRect ql_canvas_clip(QLCanvas *canvas);

/* 改缩放（26.6 定点）。只影响 QLDraw2D 继承到的值，画布本身仍只认设备像素 */
void ql_canvas_set_scale(QLCanvas *canvas, ql_coord_t scale_fx);

/* CSS px 下的画布尺寸（= 设备尺寸 / 缩放）。布局用这个，不要用 width/height */
static inline ql_coord_t ql_canvas_css_width(const QLCanvas *canvas)
{
    ql_coord_t fx = (canvas->scale_fx > 0) ? canvas->scale_fx : QL_SCALE_ONE;
    return ql_unscale_len_ceil(canvas->width, fx);
}

static inline ql_coord_t ql_canvas_css_height(const QLCanvas *canvas)
{
    ql_coord_t fx = (canvas->scale_fx > 0) ? canvas->scale_fx : QL_SCALE_ONE;
    return ql_unscale_len_ceil(canvas->height, fx);
}

/* 单像素读写（屏幕坐标，自动按 format 转换） */
ql_color_t ql_canvas_read_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y);
void       ql_canvas_write_pixel(QLCanvas *canvas, ql_coord_t x, ql_coord_t y,
                                 ql_color_t color, uint8_t alpha);

/*
 * 圆遮罩（"圆形揭示"用）。
 *
 * clip 是矩形，圆形遮罩不能靠 clip 表达；这里在**像素汇点**上做：
 *   - `fill_row` 把每行跨度与圆的水平范围求交（每行一次 sqrt）；
 *   - `put_px` / 字形 / 阴影把 alpha 乘上圆覆盖率（边缘抗锯齿）。
 * 因此它自动作用于背景、渐变、边框、阴影、文字——不需要每个原语各改一遍。
 *
 * 单位是**设备像素**（QLCanvas 本来就是设备空间）；**r < 0 表示关闭**，
 * r == 0 表示"遮罩存在但半径为 0"（什么都不画）——揭示动画的第一帧就是这个状态。
 * 只支持一层：嵌套遮罩的语义是"取交集"，而两个圆盘的交集不是圆盘，
 * 所以上层（QLDraw2D）会拒绝嵌套并告警。
 */
void ql_canvas_set_mask(QLCanvas *canvas, ql_coord_t cx, ql_coord_t cy, ql_coord_t rx,
                        ql_coord_t ry);

/*
 * 多边形遮罩（`clip-path: polygon()`）。`xs/ys` 是**设备像素绝对坐标**，n >= 3。
 * 传 n = 0 关闭。与椭圆遮罩互斥（设置一个就关掉另一个）。
 *
 * 与"把形状画进一张 64×64 的蒙版图再拉伸"相比，解析覆盖率的边是干净的：
 * 后者在几百像素宽的元素上会糊成 3~4px 的斜坡，而且每个元素还要多一遍离屏合成。
 */
void ql_canvas_set_mask_polygon(QLCanvas *canvas, const float *xs, const float *ys,
                                int n);

/*
 * 颜色矩阵：3x4 行主序、16.16 定点（`[r0 g0 b0 o0  r1 g1 b1 o1  r2 g2 b2 o2]`）。
 * 传 on = 0 关闭。开启后光栅走逐像素路径（SIMD 行填充被绕过）。
 */
void ql_canvas_set_color_matrix(QLCanvas *canvas, const int32_t m[12], int on);
/* 遮罩是否生效（r >= 0） */
int  ql_canvas_mask_active(const QLCanvas *canvas);

/*
 * 毛玻璃（`backdrop-filter: blur()`）：把**矩形区域里已经画好的内容**就地模糊。
 *
 * 为什么就地做：本架构没有保留式场景图，但画布本身就是"前面已经画完的东西"，
 * 所以模糊它等于模糊"元素背后"。调用顺序是"先模糊、再画元素自己的背景"。
 *
 * `rect` 与 `radius` 都是**设备像素**。半径按 2 趟可分离盒式卷积近似高斯
 * （CSS 的参数是高斯标准差，这里取 box 半径 = sigma 并做两趟，视觉接近）。
 * 区域边缘做钳位（不采样区域外），所以不会把外面的颜色"吸进来"。
 *
 * 代价：一次调用要 4 趟（H/V × 2）扫描 + 两块临时缓冲，**只在真有 blur 的元素上发生**。
 * 只支持 8/16/24/32 位的 RGB 系格式；Alpha 通道不参与模糊（合成时才用得到）。
 */
/*
 * 毛玻璃：把 rect 区域里**已经画好的内容**就地模糊。
 *
 * `sat` = `backdrop-filter: saturate(<N>%)` 的百分比（0 或 100 = 不变）。
 * 模糊之后把颜色推艳 —— 玻璃后面那片"均值色"才不会灰扑扑的。
 */
ql_err_t ql_canvas_backdrop_blur(QLCanvas *canvas, QLRect rect, int radius, uint8_t sat);

/*
 * 同上，但把结果裁进**圆角矩形**（`corner` = 圆角半径，设备像素，0 等价于上一个）。
 *
 * 为什么需要它：模糊本身是方框运算，而元素常常是圆角的。不裁的话四个圆角缺口里
 * 会渗出"方的模糊块" —— 颜色跑到元素边框盒之外，CSS 里不该出现。
 *
 * 实现上只处理四个 corner×corner 角落（唯一可能落在圆角之外的部分）：模糊前存下
 * 原像素，模糊后按圆角覆盖率混回去。开销与 corner² 成正比，通常几百个像素。
 */
ql_err_t ql_canvas_backdrop_blur_round(QLCanvas *canvas, QLRect rect, int radius,
                                       ql_coord_t corner, uint8_t sat);

/* ----- 原语 ----- */
void ql_canvas_clear(QLCanvas *canvas, ql_color_t color);

/*
 * 把一张位图按最近邻缩放到 dst（设备像素）。`src == NULL` 取整幅。
 * 遵循 canvas 的 clip 与圆遮罩；源 alpha 与 global alpha 相乘后做源覆盖混合。
 * 无返回值：非法输入（空图、尺寸为 0）直接不画 —— 调用方已经用
 * ql_image_validate() 验过一遍，这里只是兜底。
 */
void ql_canvas_blit_image(QLCanvas *canvas, const QLImage *img, QLRect dst,
                          const QLRect *src, ql_coord_t radius, uint8_t alpha);
void ql_canvas_fill_rect(QLCanvas *canvas, QLRect rect, ql_color_t color);
/* alpha: 0 完全透明（跳过），255 完全不透明 */
void ql_canvas_fill_rect_alpha(QLCanvas *canvas, QLRect rect, ql_color_t color, uint8_t alpha);
void ql_canvas_fill_round_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                               ql_color_t color);
void ql_canvas_fill_round_rect_alpha(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                     ql_color_t color, uint8_t alpha);

/*
 * 四角各自的圆角半径（顺序 左上 右上 右下 左下）。CSS 的
 * `border-radius: 4px 4px 0 0` 就是它；全相同时与单半径版本逐像素一致。
 */
void ql_canvas_fill_round_rect4_alpha(QLCanvas *canvas, QLRect rect,
                                      const ql_coord_t r4[4], ql_color_t color,
                                      uint8_t alpha);
/* 四角圆角矩形沿着四条边的"环"（描边在矩形内侧，与 stroke_rect_alpha 同义） */
void ql_canvas_stroke_rect4_alpha(QLCanvas *canvas, QLRect rect, const ql_coord_t r4[4],
                                  ql_coord_t thickness, ql_color_t color, uint8_t alpha);

/* 内阴影：软边画在 inner **里面**（`box-shadow: inset ...`） */
void ql_canvas_fill_inset_shadow(QLCanvas *canvas, QLRect inner, ql_coord_t radius,
                                 ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                                 ql_color_t color, uint8_t alpha);
/* 线性渐变方向 */
typedef enum {
    QL_GRADIENT_VERTICAL = 0,     /* 从 rect 上边到下边 */
    QL_GRADIENT_HORIZONTAL,       /* 从 rect 左边到右边 */
    /*
     * 以矩形中心为圆心，按到"最远角"的距离归一化（等价 CSS 的 farthest-corner）。
     * 只画在圆角矩形内部，所以圆心一定在图形内。
     */
    QL_GRADIENT_RADIAL,
} QLGradientDir;

/*
 * 渐变圆角矩形（单趟，不需要离屏缓冲）。
 * 垂直方向是快路径：每行颜色相同，直接走行填充，成本≈纯色填充。
 * 水平 / 径向每像素颜色不同，逐像素混合，比垂直慢一个量级
 * （径向用增量法算 x 方向距离平方，没有逐像素乘法）。
 */
/*
 * 渐变：`a1`/`a2` 是两个端色各自的 alpha（CSS 的渐变色标可以带 alpha，
 * `linear-gradient(#FFF, rgba(255,255,255,0))` 就是"上实下透"）。
 */
void ql_canvas_fill_round_rect_gradient(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                        ql_color_t from, ql_color_t to,
                                        QLGradientDir dir, uint8_t a1, uint8_t a2);

/*
 * 软阴影：以 inner 矩形（圆角 radius）为轮廓，向外扩散 blur 像素，
 * 整体偏移 (dx, dy)，用 SDF + smoothstep 衰减（与 GPU 后端同一套公式）。
 *
 * 实现要点：只有"环"（距轮廓 blur 以内）需要逐像素算距离，
 * 内部实心区走 fill_row，因此在 PC 上不明显，在 MCU 上也可用。
 */
void ql_canvas_fill_shadow(QLCanvas *canvas, QLRect inner, ql_coord_t radius,
                           ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                           ql_color_t color, uint8_t alpha);

/* 描边：宽度向外不扩张，向内绘制 */
/*
 * ---- 亚像素（26.6）原语：边缘按面积覆盖率抗锯齿 ----
 *
 * `QLRectFx` 的所有字段都是**设备像素 26.6**（低 6 位是亚像素）。
 * 有了它，动画里的 `translate(0.5px)` 才真的落在半像素上——用整像素矩形
 * 只能对齐到整格，位移是 1px 一跳的。
 *
 * 与整数版本的关系：整数矩形 = 低 6 位全 0 的 fx 矩形，覆盖率恰好全是满值，
 * 结果与 `ql_canvas_fill_rect_alpha` **逐像素相同**（只是慢一点）。
 */
typedef struct {
    ql_coord_t x, y, w, h;   /* 设备像素 26.6 */
} QLRectFx;

#define QL_FX_SHIFT 6
#define QL_FX_ONE   (1 << QL_FX_SHIFT)   /* 1 设备像素 */

/*
 * 转 26.6 定点。**别写 `v << QL_FX_SHIFT`**：坐标可以是负数（画到屏幕外的元素、
 * 负偏移），而"左移负数"在 C 里是未定义行为（UBSan: left shift of negative value）。
 * 乘法是良定义的，编译器也照样会把它变成移位。
 */
static inline ql_coord_t ql_fx_of(ql_coord_t v)
{
    return (ql_coord_t)(v * (ql_coord_t)QL_FX_ONE);
}

static inline QLRectFx ql_rectfx_make(ql_coord_t x_fx, ql_coord_t y_fx, ql_coord_t w_fx,
                                      ql_coord_t h_fx)
{
    QLRectFx r;
    r.x = x_fx;
    r.y = y_fx;
    r.w = w_fx;
    r.h = h_fx;
    return r;
}

/* 整数设备像素矩形 → fx（低 6 位补 0） */
static inline QLRectFx ql_rectfx_from_rect(QLRect r)
{
    return ql_rectfx_make(ql_fx_of(r.x), ql_fx_of(r.y), ql_fx_of(r.w),
                          ql_fx_of(r.h));
}

/*
 * fx → 整数：**向外取整**（起点向下、终点向上），保证覆盖的面积不小于原矩形。
 * 位图搬运按整像素取样，所以这里宁可多算一像素也不能漏。
 */
static inline QLRect ql_rect_from_fx(QLRectFx f)
{
    const ql_coord_t x0 = (ql_coord_t)(f.x / QL_FX_ONE);
    const ql_coord_t y0 = (ql_coord_t)(f.y / QL_FX_ONE);
    const ql_coord_t x1 = (ql_coord_t)((f.x + f.w + QL_FX_ONE - 1) / QL_FX_ONE);
    const ql_coord_t y1 = (ql_coord_t)((f.y + f.h + QL_FX_ONE - 1) / QL_FX_ONE);
    return ql_rect_make(x0, y0, x1 - x0, y1 - y0);
}

void ql_canvas_fill_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_color_t color,
                            uint8_t alpha);
void ql_canvas_fill_round_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_coord_t radius_fx,
                                  ql_color_t color, uint8_t alpha);
void ql_canvas_stroke_rect_fx(QLCanvas *canvas, QLRectFx rect, ql_coord_t radius_fx,
                              ql_coord_t thickness_fx, ql_color_t color);
void ql_canvas_stroke_rect_fx_alpha(QLCanvas *canvas, QLRectFx rect, ql_coord_t radius_fx,
                                    ql_coord_t thickness_fx, ql_color_t color,
                                    uint8_t alpha);

void ql_canvas_stroke_rect(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                           ql_coord_t thickness, ql_color_t color);

/*
 * 同上，带整体 alpha（0 = 不画）。`opacity` 就靠它：描边是唯一没有 alpha 参数的
 * 图元，元素透明度要把边框一起带上才不至于"背景淡了、边还硬着"。
 */
void ql_canvas_stroke_rect_alpha(QLCanvas *canvas, QLRect rect, ql_coord_t radius,
                                 ql_coord_t thickness, ql_color_t color, uint8_t alpha);
void ql_canvas_draw_hline(QLCanvas *canvas, ql_coord_t x1, ql_coord_t x2, ql_coord_t y,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_vline(QLCanvas *canvas, ql_coord_t x, ql_coord_t y1, ql_coord_t y2,
                          ql_coord_t thickness, ql_color_t color);
void ql_canvas_draw_line(QLCanvas *canvas, ql_coord_t x1, ql_coord_t y1,
                         ql_coord_t x2, ql_coord_t y2, ql_coord_t thickness,
                         ql_color_t color);

/* ----- 文本 ----- */
typedef enum {
    QL_TEXT_ALIGN_LEFT = 0,
    QL_TEXT_ALIGN_CENTER,
    QL_TEXT_ALIGN_RIGHT,
    /*
     * 两端对齐：除**最后一行**（与末行后有空行时也除外）外，把剩余空间
     * 平摊进每个字形之间的间隙。段落只有一行时与 LEFT 完全相同（CSS 语义）。
     */
    QL_TEXT_ALIGN_JUSTIFY,
} QLTextAlign;

/*
 * 在 box 内绘制文本：水平按 align 对齐，垂直从 box.y 开始逐行排。
 * 支持 '\n' 换行；不自动折行。len < 0 表示以 '\0' 结尾。
 * 超出 box 或 canvas 裁剪区的像素自动丢弃。
 */
void ql_canvas_draw_text(QLCanvas *canvas, const QLFont *font, const char *text,
                         int32_t len, QLRect box, ql_color_t color, QLTextAlign align);

/* 单个字形绘制（高级用法；坐标是基线笔位） */
void ql_canvas_draw_glyph(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color);

/*
 * 带字体变体的字形绘制（`font-weight: bold` / `font-style: italic` 的实现）。
 *
 * 都是**合成**的，不加载第二个字体文件：
 *   - `italic`：按行剪切，每往上一行右移 `slant/height` 的斜率（约 0.21）；
 *   - `bold`  ：再画一遍、右移 1px。先画斜体那份，第二遍也带上同样的剪切，
 *               否则加粗字的右下角会缺一块。
 * 位图字体与矢量字体走的是同一条路（都逐像素写），所以 MCU 也支持。
 */
void ql_canvas_draw_glyph_ex(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                             ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                             int italic, int bold);

/*
 * 同上，再带一个整体 `alpha`（0 = 不画、255 = 原样）。
 * 元素的 `opacity` 要作用到文字上就走它 —— 字形的覆盖率已经烘进 alpha，
 * 所以这里是"覆盖率 × 元素透明度"，与背景的混合语义一致。
 */
void ql_canvas_draw_glyph_alpha(QLCanvas *canvas, const QLFont *font,
                                const QLGlyph *glyph, ql_coord_t pen_x,
                                ql_coord_t baseline_y, ql_color_t color, int italic,
                                int bold, uint8_t alpha);

/* 同一件事的 affine 版本（`transform: scaleX` + 元素透明度） */
void ql_canvas_draw_glyph_xf_alpha(QLCanvas *canvas, const QLFont *font,
                                   const QLGlyph *glyph, ql_coord_t pen_x,
                                   ql_coord_t baseline_y, ql_color_t color, int italic,
                                   int bold, ql_coord_t x_fx, ql_coord_t y_fx,
                                   ql_coord_t pivot_x, ql_coord_t pivot_y, uint8_t alpha);

/*
 * 带**水平 affine** 的字形绘制：`transform: scaleX(sx)` 用这个。
 *
 *   dst_x = pivot_x + (src_x - pivot_x) * x_fx / QL_SCALE_ONE
 *
 * `x_fx` 有符号（26.6 定点）：负值就是**水平镜像**（翻转动画的后半程）。
 * 字形高度完全不变 —— 这就是"压扁"而不是"缩小"的关键。
 * 采样是最近邻（目的列反查源列），所以压扁时会有横向抽样，与真实缩放一样会略糊。
 */
void ql_canvas_draw_glyph_xf2(QLCanvas *canvas, const QLFont *font, const QLGlyph *glyph,
                              ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color,
                              int italic, int bold,
                              ql_coord_t x_fx, ql_coord_t y_fx,
                              ql_coord_t pivot_x, ql_coord_t pivot_y);

/* 颜色按目标格式量化后的实际值（测试用：RGB565 会有精度损失） */
ql_color_t ql_canvas_quantize(QLCanvas *canvas, ql_color_t color);

#endif /* QLGL_CANVAS_H */
