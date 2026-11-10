/**
 * @file    qlgfx_gl.c
 * @brief   OpenGL ES 3.0 / EGL 后端：SDF 圆角矩形 + 字形图集，单管线单批次。
 */
/* clock_gettime 在 -std=c11 下需要 POSIX 特性宏 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <qlgl/qlgl.h>

/* 一屏内同时可见的位图数量上限（超出就环形替换，重新上传） */
#define QL_GL_IMAGE_CACHE 8

#ifdef QLGL_ENABLE_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>
#ifdef QLGL_HAVE_XSYNC
#include <X11/extensions/sync.h>
#endif
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "qlinput_x11.h"
#include "qlclipboard_x11.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QL_GL_MAX_VERTS     65536
#define QL_GL_ATLAS_SIZE    1024     /* 1 MB R8：够放几百个 CJK 字形 */
#define QL_GL_UV_SLOTS      2048     /* (字体, 字形) → UV 哈希表容量，2 的幂 */

/*
 * 字段顺序必须与 shader 的 attribute 绑定一致：
 *   a_params 一次读 4 个 float = (radius, border, use_tex, pad)
 *   a_uv 读 (u, v)
 * 之前把 u,v 放在 border 与 use_tex 之间，导致 a_params.z 读到 u 坐标，
 * 图集右半的字形被误判成矩形（表现为黑色实心方块）。
 */
typedef struct {
    float x, y;          /* 屏幕像素坐标 */
    float lx, ly;        /* 相对矩形中心（SDF 用） */
    float hx, hy;        /* 半宽半高 */
    float radius;        /* a_params.x */
    float border;        /* a_params.y：<0 填充，>=0 描边宽度 */
    float use_tex;       /* a_params.z */
    float pad;           /* a_params.w 占位 */
    float u, v;          /* a_uv */
    float r, g, b, a;    /* a_color */
    /*
     * 圆遮罩（设备像素，绝对坐标）。mr <= 0 表示不带遮罩。
     * 单独一个属性而不是复用 a_uv/a_params：字形要用 a_uv 采样图集，
     * 二者会冲突；这样顶点长到 80 B，换来"遮罩对任何图元都成立"。
     */
    float mcx, mcy, mr, mpad;
    /* 四角圆角半径（顺序 左上 右上 右下 左下），设备像素 */
    float r_tl, r_tr, r_br, r_bl;
} QLGLVertex;

typedef struct {
    float u0, v0, u1, v1;
    uint8_t valid;
} QLGLUV;

/*
 * (字体, codepoint) → 图集 UV。
 * 以前是 uv[2][256] 的定长表，只认两个内置字体；改成开放寻址哈希后
 * 任意字体（含 FreeType 动态字体）都能按需进图集，且仍是单批次单 draw call。
 *
 * 键必须用 **codepoint 而不是字形数组下标**：动态字体的缓存是"插入时保持有序"的
 * 有序数组，新字形插进中间会把后面所有字形挪位，下标不是稳定标识
 * （早先按下标做键，表现出来的症状是部分文字被画成别的字）。
 */
typedef struct {
    const QLFont *font;      /* NULL = 空槽 */
    uint32_t      codepoint;
    QLGLUV        uv;
} QLGLUVEntry;

struct QLGfxGL {
    Display         *dpy;
    Window           win;
    Atom             wm_delete;
    Atom             wm_protocols;      /* WM_PROTOCOLS 的原子（判断 ClientMessage 用） */
#ifdef QLGL_HAVE_XSYNC
    /* _NET_WM_SYNC_REQUEST：缩放时由窗口管理器"点名要一帧"，我们画完再回执 */
    Atom             net_wm_sync_request;
    XSyncCounter     sync_counter;
    XSyncValue       sync_value;
    int              sync_pending;
#endif
    EGLDisplay       egdpy;
    EGLSurface       surface;
    EGLContext       ctx;
    /*
     * X11 窗口表面的尺寸。EGL 窗口表面**不会**跟着 X 窗口自己变：
     * 窗口被 WM 改大/改小之后旧表面还是老尺寸，于是 `glViewport(w,h)` 画进一块
     * 尺寸不对的缓冲 —— 画面整体贴底、顶上留一条黑（"调整大小后才正常"）。
     * 所以尺寸一变就得重建（eglDestroySurface + eglCreateWindowSurface）。
     */
    EGLConfig        cfg;
    ql_coord_t       surface_w, surface_h;
    int              offscreen;
    int              closed;
    uint8_t          buttons;      /* 当前按住的鼠标按钮位图 */

    /*
     * XIM 输入上下文。NULL = 没有输入法（只吃得下 Latin-1）。
     * 中文/日文这些必须走 XIM —— 字符不来自键盘映射，而是输入法"提交"出来的，
     * 普通 XLookupString 拿不到。
     */
    XIM              xim;
    XIC              xic;
    /* 上一次告诉输入法的光标位置（CSS px；全 -1 表示还没设过） */
    ql_coord_t       ime_x, ime_y, ime_h;
    int              trace_im;      /* QLGL_TRACE_IM=1：打印输入法事件过滤情况 */
    int              resize_pending;   /* 窗口尺寸变过，调用方需要重新 layout */
    /*
     * 窗口的 X11 背景像素（= 上一帧的清除色）。
     * 窗口变大时，**新露出来的那块是 X 服务器用背景像素填的**，不是我们画的；
     * 背景像素没设过就是黑的，于是拖边框缩放时会先闪一片黑再出内容。
     */
    unsigned long    bg_pixel;
    int              bg_pixel_set;

    ql_coord_t       width, height;   /* 设备像素（framebuffer 尺寸） */
    ql_coord_t       scale_fx;        /* CSS px → 设备像素，26.6 定点 */


    GLuint           program;
    GLuint           vbo;
    GLint            u_viewport;
    GLint            u_atlas;
    GLint            u_bg;
    GLuint           atlas_tex;

    QLGLVertex      *verts;
    uint32_t         vert_count;
    uint32_t         vert_capacity;

    QLRect           clip;
    int              clip_active;

    QLDraw2D         draw;

    uint32_t         frame_count;
    uint32_t         draw_calls;
    uint32_t         vertex_count;
    uint32_t         frame_us;
    uint32_t         swap_us;      /* 其中 eglSwapBuffers 等了多久（vsync/合成器） */
    int              vsync_user;   /* 用户要的：1 = 开（默认），0 = 关 */
    int              vsync_eff;    /* 当前生效的 swap interval */
    uint32_t         resize_until_ms;  /* 这个时刻之前不跟 vblank（正在拖边框） */
    uint64_t         last_end_us;      /* 上一帧 swap 结束的单调时钟（微秒） */
    uint32_t         pace_us;          /* 估算的"一帧该多久"（vsync 帧间隔的滑动平均） */
    /*
     * 四角半径覆盖：只有 fill/stroke/gradient4 这些入口会置位，
     * emit_quad 系列读它填顶点的四角半径；全同的情况（绝大多数）为 0。
     */
    int              use_r4;
    ql_coord_t       cur_r4[4];
    uint64_t         cfg_us;      /* 最近一次 ConfigureNotify 的时刻（QLGL_LAG_DEBUG 用） */
    int              present_now;      /* 刚收到 ConfigureNotify：这一帧不要等，立刻推上去 */
    int              keep_vsync_on_resize;  /* 拖动期间也跟 vblank（QLGL_KEEP_VSYNC=1） */
    int              lag_debug;        /* QLGL_LAG_DEBUG=1：逐帧打印"配置→上屏"延迟 */
    struct timespec  t0;

    /* 保留式脏区：和窗口同尺寸的 FBO，帧末 blit 到窗口 */
    GLuint           fbo;
    GLuint           fbo_tex;
    int              retained;     /* 调用方要求开启 */
    int              fbo_valid;    /* FBO 已建好并可用 */
    /*
     * FBO 的**分配**尺寸（≥ 窗口尺寸）。拖动边框时窗口每变一次就重建
     * 一张全屏纹理的话，每步都要 glTexImage2D 一整块显存 —— 那正是
     * "缩放一顿一顿"的来源。按 256 对齐、只增不减，连续拖动就复用了。
     */
    ql_coord_t       fbo_cap_w;
    ql_coord_t       fbo_cap_h;
    int              fbo_fresh;    /* 刚建/刚改尺寸：内容未定义，下一帧必须整屏画 */
    int              fbo_w, fbo_h;

    QLGLUVEntry     *uv;          /* 哈希表，容量 QL_GL_UV_SLOTS */
    uint32_t         uv_used;
    /* 图集货架分配器状态 */
    /* 圆遮罩状态（设备像素） */
    float            mask_cx, mask_cy, mask_rx, mask_ry;
    int              mask_on;
    /* 多边形遮罩状态（设备像素绝对坐标；poly_n < 3 表示关闭） */
    int              poly_n;
    float            poly_xy[QL_CFG_UI_CLIP_PTS_MAX * 2];
    /* 颜色矩阵（`filter` 的纯颜色函数）：16.16 定点，与 QLQC 同一份数据 */
    int32_t          cm[12];
    int              cm_on;

    int              atlas_x, atlas_y, atlas_row_h;
    int              atlas_full_warned;

    /*
     * 毛玻璃（`backdrop-filter: blur()`）。
     * 半分辨率的 ping-pong：目标帧缓冲 →（blit 降采样）tex[0] →(H) fbo[1] →(V) fbo[0]，
     * 最后把 tex[0] 当成一张贴图画回元素矩形里。
     */
    GLuint           blur_prog;
    GLuint           blur_vbo;
    GLint            blur_u_tex, blur_u_texel, blur_u_step, blur_u_uvmax;
    GLuint           blur_tex[2];
    GLuint           blur_fbo[2];
    int              blur_w, blur_h;
    GLuint           bg_dummy_tex;    /* u_bg 的占位：没有毛玻璃时也必须绑着 */

    /*
     * 离屏图层（M9.3：filter / mix-blend-mode / mask）。
     * 每层一张 RGBA 纹理 + FBO（预乘），合成走一个专用程序：
     *   颜色矩阵族 + opacity + 圆角裁剪 + 遮罩 + 混合模式 + drop-shadow。
     * 复用模糊那套 ping-pong 纹理做 blur / drop-shadow 的模糊。
     */
    GLuint           layer_prog;
    GLuint           layer_vbo;
    GLint            lu_tex, lu_blur, lu_backdrop, lu_mask;
    GLint            lu_viewport, lu_texel, lu_opacity, lu_mode, lu_box, lu_radius;
    GLint            lu_uv_tex, lu_uv_blur, lu_local, lu_lbounds;
    /* 图层用的 4 通道模糊（预乘保 alpha）。不能复用毛玻璃那套：
     * 它的片元固定输出 alpha=1（背后是不透明的页面），拿来糊图层会把
     * "透明区域"糊成不透明黑 —— 图层边缘立刻出现黑边。 */
    GLuint           blur_rgba_prog;
    GLint            bu_tex, bu_texel, bu_step, bu_uvmax;
    GLint            lu_flags, lu_nops, lu_op, lu_ds, lu_ds_color, lu_pass, lu_flipy;
    GLuint           layer_bd_tex, layer_bd_fbo;   /* 混合模式要读的背景 */
    int              layer_bd_w, layer_bd_h;
    struct QLGLLayer {
        GLuint          tex, fbo;
        int             w, h;              /* 分配尺寸（只增不减） */
        QLRect          dev;               /* 这次用到的设备矩形 */
        QLDraw2DLayerFx fx;
        ql_coord_t      saved_w, saved_h;
        QLRect          saved_clip;
        int             saved_clip_active;
        GLint           saved_fbo;         /* 进入前的渲染目标（嵌套图层要还给它） */
        GLuint          mask_tex;
        const uint8_t  *mask_key;
    } layer[QL_CFG_UI_LAYER_MAX_DEPTH];
    int              layer_depth;
    GLint            cur_target;       /* 当前渲染目标 FBO（0 = 默认帧缓冲） */

    /*
     * 位图（`<img>`）。纹理按 (px,w,h,stride,format) 缓存：同一张图每帧重复
     * blit 时不再上传。数量很小（一屏几张），线性查找足够。
     */
    GLuint           img_tex[QL_GL_IMAGE_CACHE];
    const uint8_t   *img_key[QL_GL_IMAGE_CACHE];   /* 源指针，兼作缓存键 */
    uint16_t         img_w[QL_GL_IMAGE_CACHE];
    uint16_t         img_h[QL_GL_IMAGE_CACHE];
    uint8_t          img_fmt[QL_GL_IMAGE_CACHE];
    int              img_count;
    int              img_next;        /* 环形替换 */
    GLuint           cur_img_tex;     /* 本批次要采样的图片纹理；0 = 用占位 */
    GLint            u_image;
    GLint            u_cm;      /* vec4[3]：颜色矩阵（3x4 行主序，0..1 归一） */
    GLint            u_cm_on;
    GLint            u_pn;      /* int：多边形遮罩顶点数（< 3 = 关闭） */
    GLint            u_p;       /* vec2[N]：多边形顶点（设备像素） */
};


/* EWMH 的 UTF-8 标题（WM_NAME 是 latin-1 语义，中文会变乱码），与 qldisp_x11.c 同一做法 */
static void x11_set_title(Display *dpy, Window win, const char *title)
{
    const char *utf8 = (title != NULL) ? title : "QLGL";

    XStoreName(dpy, win, utf8);

    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    Atom utf8_str = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, win, net_name, utf8_str, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
    XChangeProperty(dpy, win, net_icon, utf8_str, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
}

/* ------------------------------ 着色器 ------------------------------ */

static const char *VS_SRC =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_local;\n"
    "layout(location=2) in vec2 a_half;\n"
    "layout(location=3) in vec4 a_params;\n"   /* radius, border, use_tex, - */
    "layout(location=4) in vec2 a_uv;\n"
    "layout(location=5) in vec4 a_color;\n"
    "layout(location=6) in vec4 a_mask;\n"
    "layout(location=7) in vec4 a_radii;\n"
    "uniform vec2 u_viewport;\n"
    "out vec2 v_local; out vec2 v_half; out vec4 v_params; out vec2 v_uv; out vec4 v_color;\n"
    "out vec4 v_mask;\n"
    "out vec4 v_radii;\n"
    "void main(){\n"
    "  vec2 ndc = (a_pos / u_viewport) * 2.0 - 1.0;\n"
    "  ndc.y = -ndc.y;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_local = a_local; v_half = a_half; v_params = a_params;\n"
    "  v_uv = a_uv; v_color = a_color; v_mask = a_mask; v_radii = a_radii;\n"
    "}\n";

/*
 * v_params = (radius, border_or_blur, mode, -)
 *   mode 0 = 形状（填充/描边），1 = 字形（采样图集），2 = 软阴影
 *   mode 2 时 border 存的是 blur 半径，距离衰减与 CPU 的 shadow_coverage 同一公式，
 *          保证两条后端视觉一致（golden 测试容忍 40/通道）。
 */
/* 把配置宏变成字符串字面量（着色器数组长度跟着 QL_CFG_UI_CLIP_PTS_MAX 走） */
#define QL_STR2(x) #x
#define QL_STR(x)  QL_STR2(x)

static const char *FS_SRC =
    "#version 300 es\n"
    /*
     * 必须 highp：uv 要分辨 1024 宽的图集（1/1024 ≈ 2^-10），mediump 的 10 位尾数
     * 刚好卡在边界上，纹素会落到两个纹素之间 → 字形发虚。
     */
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "in vec2 v_local; in vec2 v_half; in vec4 v_params; in vec2 v_uv; in vec4 v_color;\n"
    "in vec4 v_mask;\n"
    "in vec4 v_radii;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_bg;\n"
    "uniform sampler2D u_image;\n"
    "uniform vec2 u_viewport;\n"
    /*
     * 颜色矩阵（`filter` 的纯颜色函数，B9）：3x4 行主序（每行一个 vec4，
     * xyz 是线性部分、w 是偏移，单位 0..1）。u_cm_on = 0 时完全不参与运算。
     *
     * 因为是**单批次**渲染，矩阵只能当 uniform 传 —— 所以 `gl_set_cm()` 在
     * 矩阵变化时会先 flush 一次（一个有 filter 的元素多一次 draw call，
     * 换来不走离屏图层：省一块 RGBA 缓冲 + 两遍绘制）。
     */
    "uniform vec4 u_cm[3];\n"
    "uniform int u_cm_on;\n"
    /*
     * 多边形遮罩（`clip-path: polygon()`）。顶点是**设备像素绝对坐标**，
     * u_pn < 3 表示关闭。与 u_cm 一样是 uniform —— 改它之前必须先 flush。
     *
     * 覆盖率 = 内点判定 + 到最近边的距离，与 CPU 侧 poly_cov() 逐字对应。
     */
    "uniform int u_pn;\n"
    "uniform vec2 u_p[" QL_STR(QL_CFG_UI_CLIP_PTS_MAX) "];\n"
    "out vec4 frag;\n"
    "float sdRoundBox(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
    "}\n"
    "vec4 shade(){\n"
    /*
     * 四角半径：按象限从 v_radii 里挑（局部坐标 x 右为正、y 下为正）。
     * 四个值相同时退化成单半径 —— 与 CPU 侧 round_row_span4 同一套语义。
     */
    "  float radius = (v_local.x > 0.0)\n"
    "      ? ((v_local.y > 0.0) ? v_radii.z : v_radii.y)\n"
    "      : ((v_local.y > 0.0) ? v_radii.w : v_radii.x);\n"
    "  float border = v_params.y;\n"
    "  float mode = v_params.z;\n"
    "  float cov;\n"
    /*
     * 圆遮罩先算成 m，**所有出口**都要乘上它。
     * 原来这段放在 mode 分派之后，径向渐变分支提前 return，于是
     * `clip-path` + `radial-gradient` 在 GPU 上会静默丢掉遮罩（CPU 有，GPU 没有）。
     */
    "  float m = 1.0;\n"
    "  if (v_mask.z >= 0.0) {\n"
    "    vec2 fp = vec2(gl_FragCoord.x, u_viewport.y - gl_FragCoord.y);\n"
    "    vec2 rr = vec2(v_mask.z, v_mask.w);\n"
    /*
     * 半径 <= 0 一律当成"全裁"：与 CPU 侧 mask_cov_at 的 `r == 0 → 0` 一致。
     * 不做这个判断的话 `min(rr.x, rr.y) == 0` 会把 md 乘成 0，
     * smoothstep 得 0.5 ⇒ 整块半透明（曾经让 golden 的 clip 用例偏 47%）。
     */
    "    if (rr.x <= 0.0 || rr.y <= 0.0) {\n"
    "      m = 0.0;\n"
    "    } else {\n"
    "      float md = (length((fp - v_mask.xy) / rr) - 1.0) * min(rr.x, rr.y);\n"
    "      m = 1.0 - smoothstep(-0.5, 0.5, md);\n"
    "    }\n"
    "  }\n"
    /*
     * 多边形遮罩：与 CPU 的 poly_cov() 同一套算术（奇偶内点 + 到最近边距离）。
     * 用 gl_FragCoord 而不是插值量 —— 遮罩本来就是屏幕空间的。
     */
    "  if (u_pn >= 3) {\n"
    "    vec2 fp = vec2(gl_FragCoord.x, u_viewport.y - gl_FragCoord.y);\n"
    "    bool inside = false;\n"
    "    float best = 1e9;\n"
    "    for (int i = 0, j = u_pn - 1; i < u_pn; j = i++) {\n"
    "      vec2 a = u_p[j];\n"
    "      vec2 b = u_p[i];\n"
    "      vec2 e = b - a;\n"
    "      float t = dot(fp - a, e) / (dot(e, e) + 1e-6);\n"
    "      t = clamp(t, 0.0, 1.0);\n"
    "      best = min(best, length(fp - (a + e * t)));\n"
    "      if (((a.y > fp.y) != (b.y > fp.y)) &&\n"
    "          (fp.x < (b.x - a.x) * (fp.y - a.y) / ((b.y - a.y) + 1e-9) + a.x))\n"
    "        inside = !inside;\n"
    "    }\n"
    "    float sd = inside ? -best : best;\n"
    "    m *= 1.0 - smoothstep(-0.5, 0.5, sd);\n"
    "  }\n"
    /* 形状判定必须按 mode **从大到小**排，别再写成区间夹逼（容易漏档） */
    /*
     * mode 6 = A8 蒙版（rgb 取顶点色，alpha 取纹理红通道）；
     * mode 5 = 位图（直接采样 RGBA）。两者都不做形状 AA：位图自身是矩形，
     * 边缘对齐由调用方保证（与字形同一个道理，外扩反而会糊）。
     */
    /*
     * mode 7 = 内阴影（`box-shadow: inset`）：同一个 SDF，但覆盖率从**边界向内**衰减。
     * 必须排在最前面：下面的分支是按"从大到小"夹逼写的，7 会先命中 mode>4.5 的图片分支。
     */
    "  if (mode > 6.5) {\n"
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    float dd = -d;\n"
    "    float cov = 1.0 - smoothstep(0.0, max(border, 1.0), dd);\n"
    "    return vec4(v_color.rgb, v_color.a * cov * m);\n"
    "  }\n"
    "  if (mode > 4.5) {\n"
    /* 图片也要跟着 border-radius 裁：圆角用同一个 SDF 算覆盖率 */
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    float cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    "    if (mode > 5.5) {\n"
    "      return vec4(v_color.rgb, v_color.a * texture(u_image, v_uv).r * cov * m);\n"
    "    }\n"
    "    vec4 t = texture(u_image, v_uv);\n"
    "    return vec4(t.rgb, t.a * v_color.a * cov * m);\n"
    "  }\n"
    "  if (mode > 3.5) {\n"
    /* 毛玻璃：uv 盖住整个矩形，形状仍是同一个圆角 SDF */
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    "  } else if (mode > 2.5) {\n"
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    "  } else if (mode > 1.5) {\n"
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    float b = max(border, 0.5);\n"
    "    float t = clamp(d / b, 0.0, 1.0);\n"
    "    cov = 1.0 - t * t * (3.0 - 2.0 * t);\n"
    "  } else if (mode > 0.5) {\n"
    "    cov = texture(u_atlas, v_uv).r;\n"
    "  } else {\n"
    "    float d = sdRoundBox(v_local, v_half, radius);\n"
    "    float outer = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    "    if (border < 0.0) {\n"
    "      cov = outer;\n"
    "    } else {\n"
    "      float inner = 1.0 - smoothstep(-0.5, 0.5, d + border);\n"
    "      cov = max(outer - inner, 0.0);\n"
    "    }\n"
    "  }\n"
    "  if (mode > 3.5) {\n"
    "    return vec4(texture(u_bg, v_uv).rgb, v_color.a * cov * m);\n"
    "  }\n"
    "  if (mode > 2.5 && mode < 3.5) {\n"
    /*
     * 径向渐变：终止色打包在 v_uv.x 里（RGB888 塞进 float32，24 位尾数恰好精确）。
     * 用 v_local/v_half 算归一化距离，与 CPU 的 farthest-corner 同一公式。
     */
    /* packed 是 GLSL 保留字，别用 */
    "    float pk = v_uv.x;\n"
    "    float cb = mod(pk, 256.0);\n"
    "    float cg = mod(floor(pk / 256.0), 256.0);\n"
    "    float cr = floor(pk / 65536.0);\n"
    "    float t = clamp(length(v_local) / max(length(v_half), 0.5), 0.0, 1.0);\n"
    "    vec3 c2 = vec3(cr, cg, cb) / 255.0;\n"
    "    return vec4(mix(v_color.rgb, c2, t), v_color.a * cov * m);\n"
    "  }\n"
    "  return vec4(v_color.rgb, v_color.a * cov * m);\n"
    "}\n"
    /*
     * 所有分支算完再统一施加颜色矩阵 —— 这是唯一一个"出口"，
     * 不会像当年的圆遮罩那样漏掉某个 return。
     */
    "void main(){\n"
    "  vec4 c = shade();\n"
    "  if (u_cm_on != 0) {\n"
    "    vec3 o = vec3(u_cm[0].x, u_cm[1].x, u_cm[2].x) * c.r\n"
    "      + vec3(u_cm[0].y, u_cm[1].y, u_cm[2].y) * c.g\n"
    "      + vec3(u_cm[0].z, u_cm[1].z, u_cm[2].z) * c.b\n"
    "      + vec3(u_cm[0].w, u_cm[1].w, u_cm[2].w);\n"
    "    c.rgb = clamp(o, 0.0, 1.0);\n"
    "  }\n"
    "  frag = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        QL_LOGE("gl: shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build_program(QLGfxGL *g)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    g->program = glCreateProgram();
    glAttachShader(g->program, vs);
    glAttachShader(g->program, fs);
    glLinkProgram(g->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g->program, (GLsizei)sizeof(log), NULL, log);
        QL_LOGE("gl: program link failed: %s", log);
        return 0;
    }

    g->u_viewport = glGetUniformLocation(g->program, "u_viewport");
    g->u_atlas = glGetUniformLocation(g->program, "u_atlas");
    g->u_bg = glGetUniformLocation(g->program, "u_bg");
    g->u_image = glGetUniformLocation(g->program, "u_image");
    g->u_cm = glGetUniformLocation(g->program, "u_cm");
    g->u_cm_on = glGetUniformLocation(g->program, "u_cm_on");
    g->u_pn = glGetUniformLocation(g->program, "u_pn");
    g->u_p = glGetUniformLocation(g->program, "u_p[0]");
    return 1;
}

/* ------------------------------ 字形图集（动态） ------------------------------ */

/*
 * 取一行字形覆盖率并展开成 8bpp。
 * 注意：静态位图字体可能是 4bpp/1bpp（每行 strided 打包），不能直接 memcpy
 * box_w 字节——奇数宽度时 stride < box_w，会读越界（ASan 抓到的第一个错就是它）。
 */
static void fetch_glyph_row(const QLFont *font, const QLGlyph *glyph, uint32_t row,
                            uint8_t *dst)
{
    if (font->bpp == 8) {
        const uint8_t *src = ql_font_glyph_row(font, glyph, (int32_t)row);
        if (src != NULL)
            memcpy(dst, src, glyph->box_w);
        return;
    }
    for (uint32_t x = 0; x < glyph->box_w; x++)
        dst[x] = ql_font_glyph_alpha(font, glyph, (int32_t)x, (int32_t)row);
}


static uint32_t uv_hash(const QLFont *font, uint32_t codepoint)
{
    /* 指针低位对齐会撞，先右移；再混一点 codepoint */
    uintptr_t p = (uintptr_t)font;
    uint32_t h = (uint32_t)((p >> 4) ^ (p >> 13)) * 2654435761u;
    h ^= codepoint * 2246822519u;
    return h & (QL_GL_UV_SLOTS - 1);
}

static QLGLUVEntry *uv_find(QLGfxGL *g, const QLFont *font, uint32_t codepoint)
{
    if (g->uv == NULL)
        return NULL;
    uint32_t i = uv_hash(font, codepoint);
    for (uint32_t probes = 0; probes < QL_GL_UV_SLOTS; probes++) {
        QLGLUVEntry *e = &g->uv[i];
        if (e->font == NULL)
            return NULL;
        if (e->font == font && e->codepoint == codepoint)
            return e;
        i = (i + 1) & (QL_GL_UV_SLOTS - 1);
    }
    return NULL;
}

static QLGLUVEntry *uv_insert(QLGfxGL *g, const QLFont *font, uint32_t codepoint)
{
    if (g->uv == NULL || g->uv_used >= QL_GL_UV_SLOTS)
        return NULL;
    uint32_t i = uv_hash(font, codepoint);
    while (g->uv[i].font != NULL) {
        if (g->uv[i].font == font && g->uv[i].codepoint == codepoint)
            return &g->uv[i];
        i = (i + 1) & (QL_GL_UV_SLOTS - 1);
    }
    g->uv[i].font = font;
    g->uv[i].codepoint = codepoint;
    g->uv[i].uv.valid = 0;
    g->uv_used++;
    return &g->uv[i];
}

/* 在图集里给一个字形找位置；满了返回 0（该字形不画，但不会崩） */
static int atlas_pack(QLGfxGL *g, int w, int h, int *out_x, int *out_y)
{
    if (w <= 0 || h <= 0)
        return 0;

    if (g->atlas_x + w + 1 > QL_GL_ATLAS_SIZE) {
        g->atlas_x = 1;
        g->atlas_y += g->atlas_row_h + 1;
        g->atlas_row_h = 0;
    }
    if (g->atlas_y + h + 1 > QL_GL_ATLAS_SIZE) {
        if (!g->atlas_full_warned) {
            QL_LOGW("gl: glyph atlas full (%dx%d), further glyphs are dropped",
                    QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE);
            g->atlas_full_warned = 1;
        }
        return 0;
    }

    *out_x = g->atlas_x;
    *out_y = g->atlas_y;
    g->atlas_x += w + 1;
    if (h > g->atlas_row_h)
        g->atlas_row_h = h;
    return 1;
}

/*
 * 上传一个字形到图集。
 * 字形位图是"每字形独立分配"的（动态字体），所以不能直接当一块连续内存上传；
 * 小字形先拼进 64×64 的临时缓冲，一次 glTexSubImage2D 搞定（比逐行上传快一个量级）；
 * 超大字形（>64px）退回逐行，正确性优先。
 */
#define QL_GL_GLYPH_SCRATCH 64
static uint8_t s_glyph_scratch[QL_GL_GLYPH_SCRATCH * QL_GL_GLYPH_SCRATCH];

static void atlas_upload_glyph(QLGfxGL *g, const QLFont *font, const QLGlyph *glyph,
                               int dst_x, int dst_y)
{
    glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    uint32_t w = glyph->box_w, h = glyph->box_h;

    if (w <= QL_GL_GLYPH_SCRATCH && h <= QL_GL_GLYPH_SCRATCH) {
        for (uint32_t row = 0; row < h; row++)
            fetch_glyph_row(font, glyph, row, &s_glyph_scratch[(size_t)row * w]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y, (GLsizei)w, (GLsizei)h,
                        GL_RED, GL_UNSIGNED_BYTE, s_glyph_scratch);
        return;
    }

    for (uint32_t row = 0; row < h && row < QL_GL_GLYPH_SCRATCH; row++) {
        fetch_glyph_row(font, glyph, row, s_glyph_scratch);
        glTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y + (int)row, (GLsizei)w, 1,
                        GL_RED, GL_UNSIGNED_BYTE, s_glyph_scratch);
    }
}

/* 把字形位图拷进 CPU 侧图集镜像（初始化时批量填内置字体用） */
static void blit_glyph_cpu(const QLFont *font, const QLGlyph *glyph, uint8_t *dst,
                           int dst_x, int dst_y)
{
    for (uint32_t row = 0; row < glyph->box_h; row++) {
        fetch_glyph_row(font, glyph, row,
                        dst + (size_t)(dst_y + (int)row) * QL_GL_ATLAS_SIZE +
                        (size_t)dst_x);
    }
}

static void atlas_init(QLGfxGL *g)
{
    g->atlas_x = 1;
    g->atlas_y = 1;
    g->atlas_row_h = 0;
    g->atlas_full_warned = 0;
    g->uv_used = 0;

    /*
     * 内置位图字体的字形是固定的，一次性拼进 CPU 镜像再整块上传，
     * 比"每个字形来一次 glTexSubImage2D"快得多（软件 GL 上尤其明显）。
     * 之后动态字体新出现的字形再按需增量上传。
     */
    uint8_t *pixels = (uint8_t *)ql_mem_calloc(QL_GL_ATLAS_SIZE * QL_GL_ATLAS_SIZE, 1);
    if (pixels != NULL) {
        static const QLFont *const builtins[] = { &ql_font_mono_12, &ql_font_mono_16 };
        float inv = 1.0f / (float)QL_GL_ATLAS_SIZE;

        for (size_t f = 0; f < QL_ARRAY_SIZE(builtins); f++) {
            const QLFont *font = builtins[f];
            for (uint16_t i = 0; i < font->glyph_count; i++) {
                const QLGlyph *glyph = &font->glyphs[i];
                if (glyph->box_w == 0 || glyph->box_h == 0)
                    continue;
                int px = 0, py = 0;
                if (!atlas_pack(g, glyph->box_w, glyph->box_h, &px, &py))
                    break;
                blit_glyph_cpu(font, glyph, pixels, px, py);

                QLGLUVEntry *entry = uv_insert(g, font, glyph->codepoint);
                if (entry == NULL)
                    break;
                entry->uv.u0 = (float)px * inv;
                entry->uv.v0 = (float)py * inv;
                entry->uv.u1 = (float)(px + (int)glyph->box_w) * inv;
                entry->uv.v1 = (float)(py + (int)glyph->box_h) * inv;
                entry->uv.valid = 1;
            }
        }

        glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE, 0,
                     GL_RED, GL_UNSIGNED_BYTE, pixels);
        ql_mem_free(pixels);
    } else {
        glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE, 0,
                     GL_RED, GL_UNSIGNED_BYTE, NULL);
    }

    /* 图集必须 LINEAR：字形是"覆盖率"位图，放大时靠插值保持笔画柔和 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (getenv("QLGL_GL_DUMP_ATLAS") != NULL && pixels != NULL) {
        FILE *f = fopen("/tmp/qlgl_atlas.pgm", "wb");
        if (f != NULL) {
            fprintf(f, "P5\n%d %d\n255\n", QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE);
            fwrite(pixels, 1, (size_t)QL_GL_ATLAS_SIZE * QL_GL_ATLAS_SIZE, f);
            fclose(f);
        }
    }
}

/*
 * 确保该字形在图集里，返回它的 UV；首次使用时打包 + 上传。
 * 这一步只在"没见过这个字"时发生，命中哈希表是纯粹的读。
 */
static int ensure_glyph_uv(QLGfxGL *g, const QLFont *font, const QLGlyph *glyph,
                           QLGLUV *out)
{
    if (font == NULL || glyph == NULL || font->glyphs == NULL)
        return 0;
    if (glyph->box_w == 0 || glyph->box_h == 0)
        return 0;

    uint32_t codepoint = glyph->codepoint;

    QLGLUVEntry *entry = uv_find(g, font, codepoint);
    if (entry != NULL && entry->uv.valid) {
        *out = entry->uv;
        return 1;
    }

    int px = 0, py = 0;
    if (!atlas_pack(g, glyph->box_w, glyph->box_h, &px, &py))
        return 0;

    atlas_upload_glyph(g, font, glyph, px, py);

    if (entry == NULL)
        entry = uv_insert(g, font, codepoint);
    if (entry == NULL)
        return 0;

    float inv = 1.0f / (float)QL_GL_ATLAS_SIZE;
    entry->uv.u0 = (float)px * inv;
    entry->uv.v0 = (float)py * inv;
    entry->uv.u1 = (float)(px + (int)glyph->box_w) * inv;
    entry->uv.v1 = (float)(py + (int)glyph->box_h) * inv;
    entry->uv.valid = 1;
    *out = entry->uv;
    return 1;
}

/* ------------------------------ 顶点发射 ------------------------------ */

static void emit_vertex(QLGfxGL *g, const QLGLVertex *v)
{
    if (g->vert_count >= g->vert_capacity)
        return;      /* 溢出就丢：宁可少画不可崩 */

    /*
     * 圆遮罩在**唯一出口**统一填：所有 emit_* 都从这里进，
     * 于是背景/边框/渐变/阴影/字形自动全部支持，不需要各改一遍。
     */
    QLGLVertex out = *v;
    out.mcx = g->mask_cx;
    out.mcy = g->mask_cy;
    out.mr = (g->mask_on) ? g->mask_rx : -1.0f;
    out.mpad = (g->mask_on) ? g->mask_ry : -1.0f;
    g->verts[g->vert_count++] = out;
}

/* 顶点颜色按位置插值：CPU 侧是逐行/逐像素 lerp，GPU 让插值器代劳，视觉等价 */
static void color_at(ql_color_t a, ql_color_t b, int t, float *out)
{
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    uint32_t inv = (uint32_t)(255 - t);
    uint32_t u = (uint32_t)t;
    out[0] = (float)(((uint32_t)QL_COLOR_GET_R(a) * inv +
                      (uint32_t)QL_COLOR_GET_R(b) * u) / 255u) / 255.0f;
    out[1] = (float)(((uint32_t)QL_COLOR_GET_G(a) * inv +
                      (uint32_t)QL_COLOR_GET_G(b) * u) / 255u) / 255.0f;
    out[2] = (float)(((uint32_t)QL_COLOR_GET_B(a) * inv +
                      (uint32_t)QL_COLOR_GET_B(b) * u) / 255u) / 255.0f;
}

/*
 * 几何裁剪：把四边形的范围与当前裁剪求交。
 *
 * 本后端是**单批次**（整帧一次 glDrawArrays），所以不能用 scissor 逐图元裁剪，
 * 只能在发顶点时就把四边形切到裁剪框里。形状参数（hx/hy/radius/border/渐变跨度）
 * 必须继续用**未裁剪**的矩形，否则片元里的 SDF 会算错——被裁掉的只是光栅化范围。
 */
static int clip_quad(QLGfxGL *g, float bx0, float by0, float bx1, float by1,
                     float *qx0, float *qy0, float *qx1, float *qy1)
{
    float x0 = bx0, y0 = by0, x1 = bx1, y1 = by1;
    if (g->clip_active) {
        if (x0 < (float)g->clip.x)
            x0 = (float)g->clip.x;
        if (y0 < (float)g->clip.y)
            y0 = (float)g->clip.y;
        if (x1 > (float)ql_rect_right(g->clip))
            x1 = (float)ql_rect_right(g->clip);
        if (y1 > (float)ql_rect_bottom(g->clip))
            y1 = (float)ql_rect_bottom(g->clip);
    }
    if (x0 >= x1 || y0 >= y1)
        return 0;
    *qx0 = x0;
    *qy0 = y0;
    *qx1 = x1;
    *qy1 = y1;
    return 1;
}

/*
 * 绘制期平移：CSS px 26.6 → 设备像素（浮点，保留小数）。
 * 每次 emit 现算，不做缓存——M5.5 的逐节点 transform 会在帧中途改它。
 */
static inline float off_x_of(QLGfxGL *g)
{
    return (float)((int64_t)g->draw.offset_x_fx * (int64_t)g->scale_fx) /
           (float)(QL_FX_ONE * QL_SCALE_ONE);
}

static inline float off_y_of(QLGfxGL *g)
{
    return (float)((int64_t)g->draw.offset_y_fx * (int64_t)g->scale_fx) /
           (float)(QL_FX_ONE * QL_SCALE_ONE);
}

/* 文本用整数部分，保证字形清晰 */
static inline float round_to_int(float v)
{
    return (float)((int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
}

/*
 * 发射一个（可带圆角/描边/纹理/剪切）的四边形。
 *
 * `shear` 只给合成斜体用：把**顶边**两个顶点右移这么多像素，底边不动，
 * uv 仍按剪切前的角点取（否则采样会跟着歪）。
 */
static void emit_quad_shear(QLGfxGL *g, QLRect r, ql_coord_t radius, ql_coord_t border,
                            ql_color_t color, uint8_t alpha,
                            float u0, float v0, float u1, float v1, int use_tex,
                            float shear)
{
    alpha = ql_draw2d_eff_alpha(&g->draw, alpha);
    if (ql_rect_is_empty(r) || alpha == 0)
        return;

    /*
     * 形状（填充/描边）外扩 1px 给 SDF 的抗锯齿留余量；**字形不外扩**。
     *
     * 字形位图本身已经带抗锯齿（覆盖率已经烘进 alpha 里），再外扩只会把图元
     * 拉大 (w+2)/w 倍：字宽 8px 就是 25%，字体变胖、字距被吃掉、笔画糊成一片。
     * 而且图集里字形只隔 1px，外扩会采到邻字的像素。
     */
    const float pad = (use_tex == 1) ? 0.0f : 1.0f;
    const float bx0 = (float)r.x - pad;
    const float by0 = (float)r.y - pad;
    const float bx1 = (float)ql_rect_right(r) + pad;
    const float by1 = (float)ql_rect_bottom(r) + pad;

    float x0, y0, x1, y1;
    if (!clip_quad(g, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1))
        return;

    float cx = (float)r.x + (float)r.w * 0.5f;
    float cy = (float)r.y + (float)r.h * 0.5f;
    const float hx = (float)r.w * 0.5f;
    const float hy = (float)r.h * 0.5f;

    /* 圆角半径不能超过短边一半 */
    float half_min = (hx < hy ? hx : hy);
    float rad = (float)radius;
    if (rad > half_min) rad = half_min;
    if (rad < 0.0f) rad = 0.0f;

    float cr = (float)QL_COLOR_GET_R(color) / 255.0f;
    float cg = (float)QL_COLOR_GET_G(color) / 255.0f;
    float cb = (float)QL_COLOR_GET_B(color) / 255.0f;
    float ca = (float)alpha / 255.0f;

    float b = (border < 0) ? -1.0f : (float)border;

    /* 四角半径：默认四角相同；`use_r4` 时按角取（同样夹到短边一半） */
    float r4v[4];
    if (g->use_r4) {
        for (int i = 0; i < 4; i++) {
            float rr = (float)g->cur_r4[i];   /* 调用方已经换成设备像素 */
            if (rr > half_min) rr = half_min;
            if (rr < 0.0f) rr = 0.0f;
            r4v[i] = rr;
        }
    } else {
        r4v[0] = r4v[1] = r4v[2] = r4v[3] = rad;
    }

    /* 绘制期平移：形状吃全偏移（亚像素），字形只吃整数部分（保持清晰） */
    const float ox = (use_tex == 1) ? round_to_int(off_x_of(g)) : off_x_of(g);
    const float oy = (use_tex == 1) ? round_to_int(off_y_of(g)) : off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;
    cx += ox; cy += oy;

    /* uv 用剪切前的角点；位置用剪切后的（顶边右移 shear） */
    const float xsu[4] = { x0, x1, x1, x0 };
    const float xs[4] = { x0 + shear, x1 + shear, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };

    /*
     * uv 的插值基准必须是**加了偏移后**的形状矩形：xs[] 已经含 ox，
     * 而 bx0/bx1 是位移前的值。少加这一层，被 transform 平移的节点上的字形
     * 会按位移量错采图集（平移 -70px 时采到完全不相干的字形）。
     */
    const float ux0 = bx0 + ox, ux1 = bx1 + ox;
    const float uy0 = by0 + oy, uy1 = by1 + oy;

    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = hx; v.hy = hy;
    v.radius = rad;
    v.r_tl = r4v[0];
    v.r_tr = r4v[1];
    v.r_br = r4v[2];
    v.r_bl = r4v[3];
    v.border = b;
    v.use_tex = use_tex ? 1.0f : 0.0f;
    v.r = cr; v.g = cg; v.b = cb; v.a = ca;

    /* 两个三角形：(0,1,2) (0,2,3) */
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx;
        v.ly = ys[k] - cy;
        /*
         * uv 由顶点在形状矩形里的相对位置线性插值：
         * 未裁剪时与"角点对 uv"完全一致；裁剪后仍然对得上图集。
         * 字形不外扩且落在整数像素上，所有顶点算出来就是 0 或 1，
         * 于是纹理采样恰好落在纹素中心（GL_LINEAR 也不会糊）。
         */
        v.u = u0 + ((ux1 > ux0) ? (xsu[k] - ux0) / (ux1 - ux0) : 0.0f) * (u1 - u0);
        v.v = v0 + ((uy1 > uy0) ? (ys[k] - uy0) / (uy1 - uy0) : 0.0f) * (v1 - v0);
        emit_vertex(g, &v);
    }
}

static inline QLRect gsx(QLGfxGL *g, QLRect r);
static inline ql_coord_t gsl(QLGfxGL *g, ql_coord_t v);

static void emit_quad(QLGfxGL *g, QLRect r, ql_coord_t radius, ql_coord_t border,
                      ql_color_t color, uint8_t alpha,
                      float u0, float v0, float u1, float v1, int use_tex)
{
    emit_quad_shear(g, r, radius, border, color, alpha, u0, v0, u1, v1, use_tex, 0.0f);
}

/*
 * 位图四边形。与 emit_quad_shear 的区别：
 *   - 不外扩（和字形一样，外扩会糊）；
 *   - 不吃剪切（图片不参与合成斜体）；
 *   - uv 是"整张图的一个窗口"（给将来 object-fit: cover 用），而且
 *     **必须按未裁剪的矩形插值**，否则裁剪后采样会错位（与字形同一个坑）。
 */
static void emit_image_quad(QLGfxGL *g, QLRect r, ql_coord_t radius, ql_color_t tint,
                            uint8_t alpha, float u0, float v0, float u1, float v1,
                            float mode)
{
    if (ql_rect_is_empty(r) || alpha == 0)
        return;

    float x0, y0, x1, y1;
    if (!clip_quad(g, (float)r.x, (float)r.y, (float)ql_rect_right(r),
                   (float)ql_rect_bottom(r), &x0, &y0, &x1, &y1))
        return;

    const float ox = off_x_of(g), oy = off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;

    const float cx = (float)r.x + (float)r.w * 0.5f;
    const float cy = (float)r.y + (float)r.h * 0.5f;
    const float hx = (float)r.w * 0.5f;
    const float hy = (float)r.h * 0.5f;

    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = hx;
    v.hy = hy;
    v.radius = (float)radius;         /* 圆角：片元里用同一个 SDF 算覆盖率 */
    {
        const float rr = (float)radius;
        v.r_tl = v.r_tr = v.r_br = v.r_bl = rr;
    }
    v.border = -1.0f;
    v.use_tex = mode;                 /* 5 = 位图，6 = A8 蒙版（着色用顶点色） */
    v.r = (float)QL_COLOR_GET_R(tint) / 255.0f;
    v.g = (float)QL_COLOR_GET_G(tint) / 255.0f;
    v.b = (float)QL_COLOR_GET_B(tint) / 255.0f;
    v.a = (float)alpha / 255.0f;

    /* uv 的插值基准：位移后、未裁剪的矩形（见注释） */
    const float ux0 = (float)r.x + ox, ux1 = (float)ql_rect_right(r) + ox;
    const float uy0 = (float)r.y + oy, uy1 = (float)ql_rect_bottom(r) + oy;

    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 6; i++) {
        const int k = order[i];
        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx - ox;
        v.ly = ys[k] - cy - oy;
        v.u = u0 + ((ux1 > ux0) ? (xs[k] - ux0) / (ux1 - ux0) : 0.0f) * (u1 - u0);
        v.v = v0 + ((uy1 > uy0) ? (ys[k] - uy0) / (uy1 - uy0) : 0.0f) * (v1 - v0);
        emit_vertex(g, &v);
    }
}

/* 渐变：从 / 到两个颜色按顶点位置插值，不需要新增顶点属性 */
/*
 * 径向渐变的第二色：打包成一个 float 塞进 a_uv.x。
 * RGB888 最大 0xFFFFFF < 2^24，float32 的尾数也是 24 位，所以**逐位精确**，
 * 不需要给顶点加新属性（单批次、零结构增长）。
 */
static float pack_color(ql_color_t c)
{
    return (float)(((uint32_t)QL_COLOR_GET_R(c) << 16) |
                   ((uint32_t)QL_COLOR_GET_G(c) << 8) |
                   (uint32_t)QL_COLOR_GET_B(c));
}

static void emit_quad_gradient(QLGfxGL *g, QLRect r, ql_coord_t radius,
                               ql_color_t from, ql_color_t to, uint8_t dir,
                               uint8_t alpha)
{
    if (ql_rect_is_empty(r) || alpha == 0)
        return;
    /* 注意别把 RADIAL 一起归一成 VERTICAL —— 那样径向会静默变成竖直渐变 */
    if (dir != QL_GRADIENT_HORIZONTAL && dir != QL_GRADIENT_RADIAL)
        dir = QL_GRADIENT_VERTICAL;

    float x0, y0, x1, y1;
    if (!clip_quad(g, (float)r.x - 1.0f, (float)r.y - 1.0f,
                   (float)ql_rect_right(r) + 1.0f, (float)ql_rect_bottom(r) + 1.0f,
                   &x0, &y0, &x1, &y1))
        return;

    float cx = (float)r.x + (float)r.w * 0.5f;
    float cy = (float)r.y + (float)r.h * 0.5f;
    float hx = (float)r.w * 0.5f;
    float hy = (float)r.h * 0.5f;

    float half_min = (hx < hy ? hx : hy);
    float rad = (float)radius;
    if (rad > half_min) rad = half_min;
    if (rad < 0.0f) rad = 0.0f;

    float ca = (float)alpha / 255.0f;
    const int radial = (dir == QL_GRADIENT_RADIAL);
    ql_coord_t span = (dir == QL_GRADIENT_HORIZONTAL) ? r.w : r.h;
    if (span < 1) span = 1;

    /* 绘制期平移直接加进顶点（浮点，零成本） */
    const float ox = off_x_of(g), oy = off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;
    cx += ox; cy += oy;

    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };

    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = hx; v.hy = hy;
    v.radius = rad;
    v.r_tl = v.r_tr = v.r_br = v.r_bl = rad;
    v.border = -1.0f;
    v.use_tex = 0.0f;
    v.a = ca;

    if (radial) {
        /* 起始色走 v_color，终止色打包进 uv.x，形状仍是同一个 SDF */
        v.use_tex = 3.0f;
        v.u = pack_color(to);
        v.radius = rad;
        const float r3[3] = { (float)QL_COLOR_GET_R(from) / 255.0f,
                              (float)QL_COLOR_GET_G(from) / 255.0f,
                              (float)QL_COLOR_GET_B(from) / 255.0f };
        v.r = r3[0]; v.g = r3[1]; v.b = r3[2];
    }

    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        if (!radial) {
            /* 按顶点自身位置算插值位置，与 CPU 的逐行/逐像素 lerp 对齐 */
            int t;
            if (dir == QL_GRADIENT_HORIZONTAL)
                t = (int)(((xs[k] - (float)r.x) / (float)span) * 255.0f);
            else
                t = (int)(((ys[k] - (float)r.y) / (float)span) * 255.0f);

            float rgb[3];
            color_at(from, to, t, rgb);
            v.r = rgb[0]; v.g = rgb[1]; v.b = rgb[2];
        }

        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx;
        v.ly = ys[k] - cy;
        emit_vertex(g, &v);
    }
}

/*
 * 软阴影：quad 覆盖"轮廓外扩 blur"，但 lx/ly 相对**轮廓中心**、hx/hy 是轮廓半尺寸，
 * 所以片元里的 sdRoundBox 算的就是到轮廓的距离。不需要新增任何顶点属性。
 */
static void emit_shadow(QLGfxGL *g, QLRect inner, ql_coord_t radius, ql_coord_t blur,
                        ql_coord_t dx, ql_coord_t dy, ql_color_t color, uint8_t alpha)
{
    if (ql_rect_is_empty(inner) || alpha == 0)
        return;
    if (blur < 0)
        blur = 0;

    QLRect shaped = ql_rect_make(inner.x + dx, inner.y + dy, inner.w, inner.h);
    float pad = (float)blur + 1.0f;

    float x0, y0, x1, y1;
    if (!clip_quad(g, (float)shaped.x - pad, (float)shaped.y - pad,
                   (float)ql_rect_right(shaped) + pad,
                   (float)ql_rect_bottom(shaped) + pad, &x0, &y0, &x1, &y1))
        return;

    float cx = (float)shaped.x + (float)shaped.w * 0.5f;
    float cy = (float)shaped.y + (float)shaped.h * 0.5f;
    float hx = (float)shaped.w * 0.5f;
    float hy = (float)shaped.h * 0.5f;

    float half_min = (hx < hy ? hx : hy);
    float rad = (float)radius;
    if (rad > half_min) rad = half_min;
    if (rad < 0.0f) rad = 0.0f;

    /* 绘制期平移直接加进顶点（浮点，零成本） */
    const float ox = off_x_of(g), oy = off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;
    cx += ox; cy += oy;

    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };

    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = hx; v.hy = hy;
    v.radius = rad;
    v.r_tl = v.r_tr = v.r_br = v.r_bl = rad;
    v.border = (float)blur;
    v.use_tex = 2.0f;                 /* 2 = 阴影模式 */
    v.r = (float)QL_COLOR_GET_R(color) / 255.0f;
    v.g = (float)QL_COLOR_GET_G(color) / 255.0f;
    v.b = (float)QL_COLOR_GET_B(color) / 255.0f;
    v.a = (float)alpha / 255.0f;

    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx;
        v.ly = ys[k] - cy;
        emit_vertex(g, &v);
    }
}

static void gl_fill_round_rect_gradient(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                        ql_color_t from, ql_color_t to, uint8_t dir,
                                        uint8_t alpha)
{
    emit_quad_gradient((QLGfxGL *)self->user, rect, radius, from, to, dir, alpha);
}

static void emit_inset_shadow(QLGfxGL *g, QLRect inner, ql_coord_t radius,
                              ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                              ql_color_t color, uint8_t alpha);

static void gl_fill_inset_shadow(QLDraw2D *self, QLRect inner, ql_coord_t radius,
                                 ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                                 ql_color_t color, uint8_t alpha)
{
    emit_inset_shadow((QLGfxGL *)self->user, inner, radius, blur, dx, dy, color, alpha);
}

static void gl_fill_round_rect4(QLDraw2D *self, QLRect rect, const ql_coord_t r4[4],
                                ql_color_t color, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    for (int i = 0; i < 4; i++)
        g->cur_r4[i] = r4[i];
    g->use_r4 = 1;
    emit_quad(g, gsx(g, rect), gsl(g, ql_ui_radius_max4(r4)), -1, color, alpha, 0, 0, 0, 0,
              0);
    g->use_r4 = 0;
}

static void gl_stroke_round_rect4(QLDraw2D *self, QLRect rect, const ql_coord_t r4[4],
                                  ql_coord_t thickness, ql_color_t color)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    for (int i = 0; i < 4; i++)
        g->cur_r4[i] = r4[i];
    g->use_r4 = 1;
    emit_quad(g, gsx(g, rect), gsl(g, ql_ui_radius_max4(r4)), gsl(g, thickness), color,
              255, 0, 0, 0, 0, 0);
    g->use_r4 = 0;
}

static void gl_fill_round_rect_gradient4(QLDraw2D *self, QLRect rect,
                                         const ql_coord_t r4[4], ql_color_t from,
                                         ql_color_t to, uint8_t dir, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    for (int i = 0; i < 4; i++)
        g->cur_r4[i] = r4[i];
    g->use_r4 = 1;
    emit_quad_gradient(g, rect, ql_ui_radius_max4(r4), from, to, dir, alpha);
    g->use_r4 = 0;
}

static void emit_inset_shadow(QLGfxGL *g, QLRect inner, ql_coord_t radius,
                              ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                              ql_color_t color, uint8_t alpha)
{
    if (ql_rect_is_empty(inner) || alpha == 0)
        return;
    if (blur < 0)
        blur = 0;
    /* 边界 = inner 平移 (dx,dy)；只在 inner 里面画 */
    const QLRect shaped = ql_rect_make(inner.x + dx, inner.y + dy, inner.w, inner.h);

    float x0, y0, x1, y1;
    if (!clip_quad(g, (float)inner.x, (float)inner.y, (float)ql_rect_right(inner),
                   (float)ql_rect_bottom(inner), &x0, &y0, &x1, &y1))
        return;

    float cx = (float)shaped.x + (float)shaped.w * 0.5f;
    float cy = (float)shaped.y + (float)shaped.h * 0.5f;
    float hx = (float)shaped.w * 0.5f;
    float hy = (float)shaped.h * 0.5f;

    float half_min = (hx < hy ? hx : hy);
    float rad = (float)radius;
    if (rad > half_min) rad = half_min;
    if (rad < 0.0f) rad = 0.0f;

    const float ox = off_x_of(g), oy = off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;
    cx += ox; cy += oy;

    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };

    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = hx; v.hy = hy;
    v.radius = rad;
    v.r_tl = v.r_tr = v.r_br = v.r_bl = rad;
    v.border = (float)((blur > 0) ? blur : 1);
    v.use_tex = 7.0f;                 /* 7 = 内阴影 */
    v.r = (float)QL_COLOR_GET_R(color) / 255.0f;
    v.g = (float)QL_COLOR_GET_G(color) / 255.0f;
    v.b = (float)QL_COLOR_GET_B(color) / 255.0f;
    v.a = (float)alpha / 255.0f;

    static const int order2[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order2[i];
        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx;
        v.ly = ys[k] - cy;
        emit_vertex(g, &v);
    }
}

static void gl_fill_shadow(QLDraw2D *self, QLRect inner, ql_coord_t radius,
                           ql_coord_t blur, ql_coord_t dx, ql_coord_t dy,
                           ql_color_t color, uint8_t alpha)
{
    emit_shadow((QLGfxGL *)self->user, inner, radius, blur, dx, dy, color, alpha);
}

/* ------------------------------ QLDraw2D 实现 ------------------------------ */

/* 元素缩放（transform: scale）绕中心等比缩放，按"边"算，相邻矩形不裂缝 */
static inline int elem_identity_gl(const QLGfxGL *g)
{
    return g->draw.elem_x_fx == QL_SCALE_ONE && g->draw.elem_y_fx == QL_SCALE_ONE;
}

static inline ql_coord_t elem_len_fx_gl(const QLGfxGL *g)
{
    const ql_coord_t ax = (g->draw.elem_x_fx < 0) ? -g->draw.elem_x_fx
                                                  : g->draw.elem_x_fx;
    const ql_coord_t ay = (g->draw.elem_y_fx < 0) ? -g->draw.elem_y_fx
                                                  : g->draw.elem_y_fx;
    return (ax < ay) ? ax : ay;
}

static inline QLRect elem_rect_gl(QLGfxGL *g, QLRect r)
{
    if (elem_identity_gl(g))
        return r;
    ql_coord_t fx = g->draw.elem_x_fx;
    ql_coord_t fy = g->draw.elem_y_fx;
    ql_coord_t px = g->draw.elem_px, py = g->draw.elem_py;
    ql_coord_t x0 = px + (ql_coord_t)((int64_t)(r.x - px) * fx / QL_SCALE_ONE);
    ql_coord_t x1 = px + (ql_coord_t)((int64_t)(ql_rect_right(r) - px) * fx /
                                      QL_SCALE_ONE);
    ql_coord_t y0 = py + (ql_coord_t)((int64_t)(r.y - py) * fy / QL_SCALE_ONE);
    ql_coord_t y1 = py + (ql_coord_t)((int64_t)(ql_rect_bottom(r) - py) * fy /
                                      QL_SCALE_ONE);
    /* 负缩放（镜像）会让两端交换顺序：先归一化 */
    if (x1 < x0) {
        const ql_coord_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        const ql_coord_t t = y0;
        y0 = y1;
        y1 = t;
    }
    return ql_rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* 缩放只发生在后端入口：上层给的全是 CSS px，emit_* 收的一律是设备像素 */
static inline QLRect gsx(QLGfxGL *g, QLRect r)
{
    r = elem_rect_gl(g, r);
    return (g->scale_fx == QL_SCALE_ONE) ? r : ql_scale_rect(r, g->scale_fx);
}

static inline ql_coord_t gsl(QLGfxGL *g, ql_coord_t v)
{
    if (!elem_identity_gl(g))
        v = (ql_coord_t)((int64_t)v * elem_len_fx_gl(g) / QL_SCALE_ONE);
    return (g->scale_fx == QL_SCALE_ONE) ? v : ql_scale_len(v, g->scale_fx);
}

static void gl_set_clip(QLDraw2D *self, QLRect clip)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    g->clip = ql_rect_intersect(gsx(g, clip), ql_rect_make(0, 0, g->width, g->height));
    g->clip_active = 1;

    /* 立即应用 scissor（GL 原点在左下，y 要翻转） */
    int x = (int)g->clip.x;
    int y = (int)(g->height - ql_rect_bottom(g->clip));
    glScissor(x, y, (GLsizei)g->clip.w, (GLsizei)g->clip.h);
}

/* ------------------------------ 毛玻璃（backdrop blur） ------------------------------ */

static void flush_batch(QLGfxGL *g);   /* 定义在后面（end() 那一段） */

/*
 * 两趟可分离模糊用的最小程序：一个全屏三角形/四边形 + 一次 9 抽头高斯。
 * `u_step` = 抽头间距（像素），由框半径除以 4 得到，抽头数固定 9 个
 * （GLSL ES 3.0 允许非常量循环上限，但固定 9 抽头的代价更可预测）。
 */
static const char *BLUR_VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char *BLUR_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"      /* 一个纹素 = (1/w, 1/h) */
    "uniform vec2 u_step;\n"       /* 抽头间距（已按方向与半径算好） */
    /*
     * 纹理是"只增不减"复用的：一块 270px 宽的玻璃用完后，下一个 94px 宽的
     * 玻璃会复用同一张更大的纹理，真正有效的内容只占左上角 u_uvmax 那一块。
     * 不夹紧的话，边缘的抽头会读到上一次残留的像素（画面里出现"别处的颜色"）。
     */
    "uniform vec2 u_uvmax;\n"
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  const float w[5] = float[5](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);\n"
    "  vec2 c = clamp(v_uv, vec2(0.0), u_uvmax);\n"
    "  vec3 acc = texture(u_tex, c).rgb * w[0];\n"
    "  for (int i = 1; i < 5; i++) {\n"
    "    vec2 o = u_step * float(i);\n"
    "    acc += texture(u_tex, clamp(c + o, vec2(0.0), u_uvmax)).rgb * w[i];\n"
    "    acc += texture(u_tex, clamp(c - o, vec2(0.0), u_uvmax)).rgb * w[i];\n"
    "  }\n"
    "  frag = vec4(acc, 1.0);\n"
    "}\n";

static GLuint build_blur_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, BLUR_VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, BLUR_FS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, (GLsizei)sizeof(log), NULL, log);
        QL_LOGE("gl: blur program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* 半分辨率 ping-pong 缓冲：只在需要的尺寸变大时重建 */
static int blur_ensure(QLGfxGL *g, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g->blur_tex[0] != 0 && g->blur_w >= w && g->blur_h >= h)
        return 1;

    for (int i = 0; i < 2; i++) {
        if (g->blur_fbo[i] != 0) {
            glDeleteFramebuffers(1, &g->blur_fbo[i]);
            g->blur_fbo[i] = 0;
        }
        if (g->blur_tex[i] != 0) {
            glDeleteTextures(1, &g->blur_tex[i]);
            g->blur_tex[i] = 0;
        }
    }

    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &g->blur_tex[i]);
        glBindTexture(GL_TEXTURE_2D, g->blur_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &g->blur_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g->blur_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            QL_LOGE("gl: blur FBO incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g->blur_w = w;
    g->blur_h = h;
    return 1;
}

/*
 * 一趟模糊：从 tex[src] 画到 fbo[dst]，dx/dy 是方向（纹素为单位的抽头间距）。
 * `uw/uh` 是**有效区域**的归一化尺寸（<= 1）：纹理是复用的，只有左上角这块
 * 是这次要处理的内容，采样必须夹在这里面。
 */
static void blur_pass(QLGfxGL *g, int src, int dst, float dx, float dy, float uw,
                      float uh)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[dst]);
    glViewport(0, 0, g->blur_w, g->blur_h);
    /* 只写有效区域：外面那些纹素是上一次的残留，不必碰 */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)(uw * (float)g->blur_w), (GLsizei)(uh * (float)g->blur_h));
    glUseProgram(g->blur_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->blur_tex[src]);
    glUniform1i(g->blur_u_tex, 0);
    glUniform2f(g->blur_u_texel, 1.0f / (GLfloat)g->blur_w, 1.0f / (GLfloat)g->blur_h);
    glUniform2f(g->blur_u_step, dx, dy);
    glUniform2f(g->blur_u_uvmax, uw, uh);

    static const float quad[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, g->blur_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

/*
 * 把设备像素矩形里"已经画好的内容"模糊掉，然后作为一张贴图画回同一位置。
 *
 * 步骤：
 *   1. flush 当前批次（不提交的话"背后"还是空的）；
 *   2. 从目标帧缓冲 blit 到半分辨率 tex[0]（GL_LINEAR 顺带降采样）；
 *   3. 两趟可分离模糊（H 到 fbo[1]，V 回 fbo[0]）；
 *   4. 恢复帧缓冲/视口，发一个 mode 4 的四边形（片元里采样 u_bg）。
 */
static void gl_backdrop_blur_round(QLDraw2D *self, QLRect rect, int sigma,
                                   ql_coord_t corner);

/* 直角版本 = 圆角半径为 0，保留旧入口（外部可能只认识 backdrop_blur） */
static void gl_backdrop_blur(QLDraw2D *self, QLRect rect, int sigma)
{
    gl_backdrop_blur_round(self, rect, sigma, 0);
}

static void gl_backdrop_blur_round(QLDraw2D *self, QLRect rect, int sigma,
                                   ql_coord_t corner)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL || g->blur_prog == 0)
        return;

    /*
     * CSS px → 设备像素：`gsx` 已经含一次 elem scale（transform: scale）和 dpr，
     * 这里再补上**绘制期平移**（transform: translate）—— 别的绘制入口是在
     * emit 顶点时加 `off_x_of/off_y_of`，毛玻璃走的是"自己 blit 一小块"，得自己加。
     * 少了这一步，带 transform 的玻璃糊的是它没移动之前那块地方。
     */
    const float ox = off_x_of(g), oy = off_y_of(g);
    QLRect dev = gsx(g, rect);
    dev.x += (ql_coord_t)(ox < 0.0f ? ox - 0.5f : ox + 0.5f);
    dev.y += (ql_coord_t)(oy < 0.0f ? oy - 0.5f : oy + 0.5f);
    dev = ql_rect_intersect(dev, ql_rect_make(0, 0, g->width, g->height));
    if (ql_rect_is_empty(dev))
        return;

    ql_coord_t sc = gsl(g, sigma);
    int rad = (int)(sc / 2);            /* 半分辨率下，盒半径 = 半分辨率像素数 */
    if (rad < 1)
        rad = 1;
    if (rad > 32)
        rad = 32;

    const int bw = (int)((dev.w + 1) / 2);
    const int bh = (int)((dev.h + 1) / 2);

    /* 目标帧缓冲：保留模式下是 FBO，否则是默认帧缓冲 */
    const GLuint target = (g->fbo_valid) ? g->fbo : 0;

    flush_batch(g);

    if (!blur_ensure(g, bw, bh))
        return;

    /* 1. 降采样复制：源是自上而下的设备坐标，GL 的 y 反过来 */
    const GLint sy0 = (GLint)(g->height - ql_rect_bottom(dev));
    const GLint sy1 = (GLint)(g->height - dev.y);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->blur_fbo[0]);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer((GLint)dev.x, sy0, (GLint)ql_rect_right(dev), sy1, 0, 0, bw,
                      bh, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* 2. 两趟可分离模糊（9 抽头，抽头间距 = 半径/4 个纹素） */
    const float span = (float)rad / 4.0f;
    /* 纹理可能比这次需要的更大：有效区域是左上角 bw x bh */
    const float uw = (float)bw / (float)g->blur_w;
    const float uh = (float)bh / (float)g->blur_h;
    blur_pass(g, 0, 1, span / (float)g->blur_w, 0.0f, uw, uh);
    blur_pass(g, 1, 0, 0.0f, span / (float)g->blur_h, uw, uh);

    /* 3. 回到目标帧缓冲，继续往顶点缓冲里摞 */
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, (GLsizei)g->width, (GLsizei)g->height);

    /* 4. 画回去：mode 4 采样 u_bg。纹理是自下而上的，所以 v 要翻 */
    QLGLVertex v;
    memset(&v, 0, sizeof(v));
    v.hx = (float)dev.w * 0.5f;
    v.hy = (float)dev.h * 0.5f;
    v.use_tex = 4.0f;
    v.border = -1.0f;
    /*
     * 圆角半径走的是同一个 SDF：片元里 `cov` 会把方框边缘的 alpha 压到 0，
     * 于是模糊结果只写进圆角内，四个缺口保留原本已画好的内容 —— 正是 CSS 语义。
     */
    v.radius = (corner > 0) ? (float)gsl(g, corner) : 0.0f;
    v.r_tl = v.r_tr = v.r_br = v.r_bl = v.radius;
    v.a = 1.0f;
    v.r = 1.0f;
    v.g = 1.0f;
    v.b = 1.0f;

    const float cx = (float)dev.x + (float)dev.w * 0.5f;
    const float cy = (float)dev.y + (float)dev.h * 0.5f;
    const float xs[4] = { (float)dev.x, (float)ql_rect_right(dev),
                          (float)ql_rect_right(dev), (float)dev.x };
    const float ys[4] = { (float)dev.y, (float)dev.y, (float)ql_rect_bottom(dev),
                          (float)ql_rect_bottom(dev) };
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        v.x = xs[k];
        v.y = ys[k];
        v.lx = xs[k] - cx;
        v.ly = ys[k] - cy;
        /*
         * uv **不能铺满整张纹理**：纹理是复用的，这次的内容只在左上角
         * bw x bh 那一块（uw/uh 是它相对整张纹理的比例）。
         * 铺满的话，小玻璃会显示"上一次那块更大的玻璃被压扁"的内容 ——
         * 现象就是模糊位置和背后的物体对不上。
         */
        v.u = (xs[k] - (float)dev.x) / (float)(dev.w > 0 ? dev.w : 1) * uw;
        v.v = (1.0f - (ys[k] - (float)dev.y) / (float)(dev.h > 0 ? dev.h : 1)) * uh;
        emit_vertex(g, &v);
    }
}

static void gl_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t rx,
                        ql_coord_t ry)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (rx < 0 || ry < 0) {
        g->mask_on = 0;
        g->mask_rx = -1.0f;
        g->mask_ry = -1.0f;
        return;
    }
    QLRect pt = gsx(g, ql_rect_make(cx, cy, 0, 0));
    g->mask_cx = (float)pt.x;
    g->mask_cy = (float)pt.y;
    g->mask_rx = (float)gsl(g, rx);
    g->mask_ry = (float)gsl(g, ry);
    g->mask_on = 1;
}

/*
 * 多边形遮罩。顶点是 uniform（不像圆遮罩那样能塞进顶点属性），所以换遮罩
 * 之前必须先 flush，否则前面攒的那批顶点会用上新遮罩。
 */
static void gl_set_mask_polygon(QLDraw2D *self, const ql_coord_t *xs,
                                const ql_coord_t *ys, int n)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL)
        return;
    if (xs == NULL || ys == NULL || n < 3) {
        if (g->poly_n == 0)
            return;
        flush_batch(g);
        g->poly_n = 0;
        return;
    }
    if (n > QL_CFG_UI_CLIP_PTS_MAX)
        n = QL_CFG_UI_CLIP_PTS_MAX;
    /* 同一份形状（一帧内每个子树都会重新设一次）就不要白 flush */
    int same = (g->poly_n == n);
    for (int i = 0; same && i < n; i++)
        if (g->poly_xy[i * 2] != (float)gsl(g, xs[i]) ||
            g->poly_xy[i * 2 + 1] != (float)gsl(g, ys[i]))
            same = 0;
    if (same)
        return;
    flush_batch(g);
    g->poly_n = n;
    for (int i = 0; i < n; i++) {
        g->poly_xy[i * 2] = (float)gsl(g, xs[i]);
        g->poly_xy[i * 2 + 1] = (float)gsl(g, ys[i]);
    }
}

/*
 * 颜色矩阵。单批次渲染下矩阵只能当 uniform 传，所以变了就先 flush ——
 * 代价是"一个带 filter 的元素多一次 draw call"，换来不走近屏图层。
 * 矩阵没变时直接返回（一帧内同一份 filter 只 flush 一次）。
 */
static void gl_set_cm(QLDraw2D *self, const int32_t m[12], int on)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL)
        return;
    if (on && m != NULL) {
        int same = 1;
        for (int i = 0; i < 12; i++)
            if (g->cm[i] != m[i]) {
                same = 0;
                break;
            }
        if (same && g->cm_on)
            return;
        flush_batch(g);
        for (int i = 0; i < 12; i++)
            g->cm[i] = m[i];
        g->cm_on = 1;
        return;
    }
    if (!g->cm_on)
        return;
    flush_batch(g);
    g->cm_on = 0;
}

/* 把一张 QLImage 传成纹理（命中缓存就直接复用）；失败返回 0 */
static GLuint gl_image_tex(QLGfxGL *g, const QLImage *img)
{
    for (int i = 0; i < g->img_count; i++) {
        if (g->img_key[i] == img->px && g->img_w[i] == img->w &&
            g->img_h[i] == img->h && g->img_fmt[i] == img->format)
            return g->img_tex[i];
    }

    int slot;
    if (g->img_count < QL_GL_IMAGE_CACHE) {
        slot = g->img_count++;
        glGenTextures(1, &g->img_tex[slot]);
        if (g->img_tex[slot] == 0)
            return 0;
    } else {
        slot = g->img_next;
        g->img_next = (g->img_next + 1) % QL_GL_IMAGE_CACHE;
    }

    GLenum ifmt = GL_RGBA8, fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    switch (img->format) {
    case QL_IMAGE_RGBA8888:
        break;
    case QL_IMAGE_RGB888:
        ifmt = GL_RGB8; fmt = GL_RGB;
        break;
    case QL_IMAGE_RGB565:
        /* ES3 里 5_6_5 的合法组合是 **sized** 的 GL_RGB565（配 GL_RGB8 会 INVALID_OPERATION，
         * 纹理没数据 → 采样全 0 → 图片整块消失） */
        ifmt = GL_RGB565; fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5;
        break;
    case QL_IMAGE_A8:
        ifmt = GL_R8; fmt = GL_RED;
        break;
    default:
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, g->img_tex[slot]);
    /* 行宽不一定是 w*bpp：用 UNPACK_ROW_LENGTH 让 GL 按 stride 取行 */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLint row_len = (GLint)(img->stride / ql_image_format_bpp(img->format));
    if (row_len > (GLint)img->w)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);
    else
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, (GLsizei)img->w, (GLsizei)img->h, 0, fmt,
                 type, img->px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    /*
     * 滤波必须与 CPU 侧一致：CPU 默认最近邻（QL_CFG_IMAGE_BILINEAR=0），
     * 这里也用 NEAREST —— 否则放大后半透明边缘会被 GL 插成渐变，
     * CPU/GPU 的逐像素比对（golden）当场就红。
     */
#if QL_CFG_IMAGE_BILINEAR
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g->img_key[slot] = img->px;
    g->img_w[slot] = img->w;
    g->img_h[slot] = img->h;
    g->img_fmt[slot] = img->format;
    return g->img_tex[slot];
}

/*
 * 位图绘制。**必须单独成批**：整帧的顶点共用一个纹理（图集），而图片是另一张
 * 纹理，所以这里 flush 两次 —— 之前攒的先落地，图片单独画，之后继续攒。
 * 与毛玻璃是同一个套路（它也要中途提交）。
 */
static void gl_blit_image(QLDraw2D *self, const QLImage *img, QLRect dst,
                          const QLRect *src, ql_coord_t radius, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL || img == NULL || img->px == NULL || alpha == 0)
        return;
    if (!ql_image_validate(img))
        return;

    /* 采样窗口：默认整幅；给 object-fit: cover 留的口子 */
    QLRect s = (src != NULL) ? *src : ql_rect_make(0, 0, img->w, img->h);
    s = ql_rect_intersect(s, ql_rect_make(0, 0, img->w, img->h));
    if (ql_rect_is_empty(s))
        return;

    flush_batch(g);
    const GLuint tex = gl_image_tex(g, img);
    if (tex == 0) {
        flush_batch(g);
        return;
    }
    g->cur_img_tex = tex;

    /*
     * uv 的方向：上传时第一行数据对应 v = 0，而第一行是图像**顶部**，
     * 所以四边形的上边用 v0、下边用 v1（**不翻**）。FBO 那种自下而上的纹理才要翻。
     */
    const float iw = (float)img->w;
    const float ih = (float)img->h;
    const float u0 = (float)s.x / iw;
    const float u1 = (float)(s.x + s.w) / iw;
    const float v0 = (float)s.y / ih;
    const float v1 = (float)(s.y + s.h) / ih;

    const float mode = (img->format == QL_IMAGE_A8) ? 6.0f : 5.0f;
    emit_image_quad(g, gsx(g, dst), gsl(g, radius), 0xFFFFFF, alpha, u0, v0, u1, v1,
                    mode);
    flush_batch(g);
    g->cur_img_tex = 0;
}

static void gl_fill_rect(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    emit_quad(g, gsx(g, rect), 0, -1, color, alpha, 0, 0, 0, 0, 0);
}

static void gl_fill_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                               ql_color_t color, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    emit_quad(g, gsx(g, rect), gsl(g, radius), -1, color, alpha, 0, 0, 0, 0, 0);
}

static void gl_stroke_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                 ql_coord_t thickness, ql_color_t color)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (thickness <= 0)
        return;
    emit_quad(g, gsx(g, rect), gsl(g, radius), gsl(g, thickness), color, 255, 0, 0, 0, 0,
              0);
}

static void gl_emit_glyph(QLDraw2D *self, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y,
                          const QLTextStyle *ts)
{
    QLGfxGL *g = (QLGfxGL *)self->user;

    QLGLUV uv;
    if (!ensure_glyph_uv(g, font, glyph, &uv))
        return;

    /* 合成斜体：顶边右移（与 CPU 侧同一斜率，见 qlcanvas.c） */
    float shear = 0.0f;
    if (ts->italic != 0 && glyph->box_h > 1)
        shear = (float)glyph->box_h * 0.21f;

    /*
     * 非等比 / 镜像：字形**高度不变**，只把 quad 的水平区间过一遍 affine。
     * 等比正缩放时字形已经换过字号，这里保持原样（那条路必须逐像素不变）。
     */
    const ql_coord_t x_fx = g->draw.elem_x_fx;
    const ql_coord_t y_fx = g->draw.elem_y_fx;
    const ql_coord_t px = g->draw.text_px;
    const ql_coord_t py = g->draw.text_py;

    const int passes = (ts->bold != 0) ? 2 : 1;
    for (int i = 0; i < passes; i++) {
        QLRect r = ql_rect_make(pen_x + glyph->ofs_x + i, baseline_y + glyph->ofs_y,
                                glyph->box_w, glyph->box_h);
        float u0 = uv.u0, u1 = uv.u1, v0 = uv.v0, v1 = uv.v1;
        if (g->draw.text_geo) {
            /* 两轴 affine（含镜像）；镜像时纹理也要翻 */
            const ql_coord_t x0 = px + (ql_coord_t)((int64_t)(r.x - px) * x_fx /
                                                    QL_SCALE_ONE);
            const ql_coord_t x1 = px +
                                  (ql_coord_t)((int64_t)(ql_rect_right(r) - px) * x_fx /
                                               QL_SCALE_ONE);
            const ql_coord_t y0 = py + (ql_coord_t)((int64_t)(r.y - py) * y_fx /
                                                    QL_SCALE_ONE);
            const ql_coord_t y1 = py +
                                  (ql_coord_t)((int64_t)(ql_rect_bottom(r) - py) * y_fx /
                                               QL_SCALE_ONE);
            r.x = (x0 < x1) ? x0 : x1;
            r.w = (x0 < x1) ? (x1 - x0) : (x0 - x1);
            r.y = (y0 < y1) ? y0 : y1;
            r.h = (y0 < y1) ? (y1 - y0) : (y0 - y1);
            if (r.w <= 0 || r.h <= 0)
                continue;
            if (x_fx < 0) {
                const float t = u0;
                u0 = u1;
                u1 = t;
            }
            if (y_fx < 0) {
                const float t = v0;
                v0 = v1;
                v1 = t;
            }
        }
        emit_quad_shear(g, r, 0, -1, ts->color, 255, u0, v0, u1, v1, 1, shear);
    }
}

static void gl_draw_text(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                         QLRect box, const QLTextStyle *ts)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (elem_len_fx_gl(g) == 0)
        return;         /* 压到零宽：同 CPU，字一个都不画 */

    /*
     * 与 CPU 侧同一套判定（见 qldraw2d.c 的 cpu_draw_text）：
     * 只有"等比 + 正 + 真拿到了另一个字号的字体"才走换字号，否则逐字形几何变换。
     */
    const ql_coord_t font_fx =
        (ql_coord_t)((int64_t)g->scale_fx * elem_len_fx_gl(g) / QL_SCALE_ONE);
    const int uniform_pos = (g->draw.elem_x_fx > 0 &&
                             g->draw.elem_x_fx == g->draw.elem_y_fx);
    const QLFont *scaled = ql_draw2d_device_font(font, font_fx);
    const int crisp = uniform_pos && scaled != font;
    const QLFont *dev = crisp ? scaled : font;
    g->draw.text_geo = (!crisp && (g->draw.elem_x_fx != QL_SCALE_ONE ||
                                   g->draw.elem_y_fx != QL_SCALE_ONE));
    g->draw.text_px = (g->scale_fx != QL_SCALE_ONE)
                          ? ql_scale_len(g->draw.elem_px, g->scale_fx)
                          : g->draw.elem_px;
    g->draw.text_py = (g->scale_fx != QL_SCALE_ONE)
                          ? ql_scale_len(g->draw.elem_py, g->scale_fx)
                          : g->draw.elem_py;
    /* 排版在设备像素里进行：字形位图与 quad 是一一对应的，不能再缩放 */
    /* 行高是 CSS px；排版在设备像素里做，所以按同样的比例换算 */
    QLTextStyle local = *ts;
    /* 行高按**垂直轴**缩放，理由见 qldraw2d.c 的 cpu_draw_text */
    const ql_coord_t vert_fx =
        (crisp) ? (ql_coord_t)((int64_t)g->scale_fx * g->draw.elem_y_fx / QL_SCALE_ONE)
                : g->scale_fx;
    if (local.line_h > 0)
        local.line_h = ql_scale_len(local.line_h, vert_fx);

    QLRect tb = crisp ? gsx(g, box)
                      : ((g->scale_fx == QL_SCALE_ONE) ? box
                                                       : ql_scale_rect(box, g->scale_fx));
    ql_draw2d_text_layout(self, dev, text, len, tb, &local, gl_emit_glyph);
}

#if QL_FEAT_CSS_FILTER_LAYER
/* ------------------------------ 离屏图层（M9.3） ------------------------------ */

/*
 * 合成程序：把一个图层的纹理按 CSS 的语义画回目标表面。
 *
 * 与 CPU 侧（qllayer.c）是同一套数学，两边靠 test_gl_vs_cpu 的像素差分守住：
 *   · 颜色矩阵族在**直通色**上做（先除 alpha，再乘回去）；
 *   · 预乘 RGBA 存储，普通混合就是 src + dst*(1-a)；
 *   · 混合模式（multiply/screen/...）需要背景，所以多绑一张背景纹理。
 *
 * `u_pass`：0 = drop-shadow（只画阴影），1 = 图层本体。
 */
static const char *LAYER_FS =
    "#version 300 es\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_blur;\n"
    "uniform sampler2D u_backdrop;\n"
    "uniform sampler2D u_mask;\n"
    "uniform vec2  u_viewport;\n"
    "uniform vec2  u_texel;\n"      /* 1/图层尺寸 */
    "uniform vec2  u_uv_tex;\n"     /* 图层纹理里"有效区域"的占比（纹理可能更大） */
    "uniform vec2  u_uv_blur;\n"
    "uniform float u_opacity;\n"
    "uniform int   u_mode;\n"
    "uniform int   u_flags;\n"      /* bit0 有 blur、bit1 有 backdrop、bit2 有 mask */
    "uniform int   u_nops;\n"
    "uniform vec2  u_op[6];\n"      /* (kind, arg) */
    "uniform vec4  u_box;\n"        /* 圆角裁剪盒（目标设备像素 x,y,w,h） */
    "uniform float u_radius;\n"
    "uniform vec4  u_ds;\n"         /* (dx, dy, blur, color_alpha) */
    "uniform vec3  u_ds_color;\n"
    "uniform int   u_pass;\n"
    "uniform int   u_flipy;\n"
    "uniform int   u_local;\n"          /* 1 = 用局部 CSS 坐标判圆角/遮罩（有旋转时） */
    "uniform vec4  u_lbounds;\n"        /* 图层矩形（CSS px：x,y,w,h，未旋转） */
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"

    "float sdRoundBox(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
    "}\n"

    "vec3 apply_ops(vec3 c){\n"
    "  for (int i = 0; i < 6; i++) {\n"
    "    if (i >= u_nops) break;\n"
    "    int k = int(u_op[i].x);\n"
    "    float a = u_op[i].y;\n"
    "    if (k == 1) { c = c * a; }\n"
    "    else if (k == 2) { c = (c - 0.5) * a + 0.5; }\n"
    "    else if (k == 3) {\n"
    "      float lum = dot(c, vec3(0.213, 0.715, 0.072));\n"
    "      c = mix(vec3(lum), c, a);\n"
    "    }\n"
    "    else if (k == 4) {\n"
    "      float lum = dot(c, vec3(0.213, 0.715, 0.072));\n"
    "      c = mix(c, vec3(lum), a);\n"
    "    }\n"
    "    else if (k == 5) {\n"
    "      vec3 s = vec3(dot(c, vec3(0.393, 0.769, 0.189)),\n"
    "                    dot(c, vec3(0.349, 0.686, 0.168)),\n"
    "                    dot(c, vec3(0.272, 0.534, 0.131)));\n"
    "      c = mix(c, s, a);\n"
    "    }\n"
    "    else if (k == 6) { c = mix(c, 1.0 - c, a); }\n"
    "    else if (k == 7) {\n"
    "      float ang = radians(a);\n"
    "      float cs = cos(ang), sn = sin(ang);\n"
    /*
     * 逐行写死点积，不用 mat3：GLSL 的 mat3(a,b,c,...) 是**按列**填的，
     * 照抄 CPU 的行主序矩阵会得到转置（色相旋转不对称 → 颜色明显不对）。
     */
    "      vec3 o = c;\n"
    "      c.r = dot(vec3(0.213 + cs * 0.787 - sn * 0.213,\n"
    "                    0.715 - cs * 0.715 - sn * 0.715,\n"
    "                    0.072 - cs * 0.072 + sn * 0.928), o);\n"
    "      c.g = dot(vec3(0.213 - cs * 0.213 + sn * 0.143,\n"
    "                    0.715 + cs * 0.285 + sn * 0.140,\n"
    "                    0.072 - cs * 0.072 - sn * 0.283), o);\n"
    "      c.b = dot(vec3(0.213 - cs * 0.213 - sn * 0.787,\n"
    "                    0.715 - cs * 0.715 + sn * 0.715,\n"
    "                    0.072 + cs * 0.928 + sn * 0.072), o);\n"
    "    }\n"
    "  }\n"
    "  return clamp(c, 0.0, 1.0);\n"
    "}\n"

    "float blend_ch(float cb, float cs, int mode){\n"
    "  if (mode == 1) return cb * cs;\n"
    "  if (mode == 2) return cb + cs - cb * cs;\n"
    "  if (mode == 3) return (cb < 0.5) ? (2.0 * cb * cs) : (1.0 - 2.0 * (1.0 - cb) * (1.0 - cs));\n"
    "  if (mode == 4) return min(cb, cs);\n"
    "  if (mode == 5) return max(cb, cs);\n"
    "  if (mode == 6) return abs(cb - cs);\n"
    "  return cs;\n"
    "}\n"

    "void main(){\n"
    /* 目标表面的设备像素坐标（y 向下） */
    /*
     * 目标表面的设备坐标（y 向下）。画到最终目标时要翻（内容是按翻转的投影
     * 画的），画到外层图层的 FBO 时不翻 —— 与顶点那边同一个开关。
     */
    /*
     * v_uv 的方向：v=1 在**顶边**、v=0 在底边（与图层纹理一致 —— 内容是按
     * "CSS y 向下"画进 FBO 的，GL 纹理的第 0 行在下面，所以顶边落在 v≈1）。
     * 因此凡是要"设备 y 向下"的量（遮罩图、局部坐标、阴影偏移）都要自己翻。
     */
    "  float fyd = (u_flipy != 0) ? (u_viewport.y - gl_FragCoord.y) : gl_FragCoord.y;\n"
    "  vec2 dp = vec2(gl_FragCoord.x, fyd);\n"
    /*
     * 圆角 / 遮罩的判定点：
     *   · 没有旋转时用目标设备坐标（与 CPU 侧快路逐像素一致）；
     *   · 有旋转时目标坐标会被转歪，必须换成**局部 CSS 坐标** ——
     *     四边形是平行四边形，仿射量（v_uv）在它上面线性插值正好就是局部坐标。
     */
    "  vec2 uv = v_uv;\n"
    "  vec2 lp = dp;\n"
    "  if (u_local != 0)\n"
    "    lp = u_lbounds.xy + vec2(uv.x, 1.0 - uv.y) * u_lbounds.zw;\n"
    "  float cov = 1.0;\n"
    "  if (u_radius > 0.0) {\n"
    "    vec2 c = u_box.xy + u_box.zw * 0.5;\n"
    "    float d = sdRoundBox(lp - c, u_box.zw * 0.5, u_radius);\n"
    "    cov = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    "    if (cov <= 0.0) { frag = vec4(0.0); return; }\n"
    "  }\n"

    "  vec4 src;\n"
    "  if (u_pass == 0) {\n"
    /* 阴影：取（已模糊的）图层 alpha，按偏移采样，染色 */
    "    vec2 suv = vec2(uv.x - u_ds.x * u_texel.x, uv.y + u_ds.y * u_texel.y)\n"
    "                   * u_uv_blur;\n"
    "    float a = texture(u_blur, suv).a * u_ds.w * u_opacity;\n"
    "    src = vec4(u_ds_color * a, a);\n"
    "  } else {\n"
    "    src = ((u_flags & 1) != 0) ? texture(u_blur, uv * u_uv_blur)\n"
    "                                : texture(u_tex, uv * u_uv_tex);\n"
    "    float a = src.a;\n"
    "    if (a > 0.0) {\n"
    "      vec3 c = src.rgb / a;\n"
    "      c = apply_ops(c);\n"
    "      src = vec4(c * a, a);\n"
    "    }\n"
    "    src *= u_opacity;\n"
    "  }\n"

    "  if ((u_flags & 4) != 0) {\n"
    "    float ma = texture(u_mask, vec2(uv.x, 1.0 - uv.y)).a;\n"
    "    src *= ma;\n"
    "  }\n"
    "  src *= cov;\n"
    "  if (src.a <= 0.0) { frag = vec4(0.0); return; }\n"

    "  if (u_mode == 0 || u_pass == 0) {\n"
    "    frag = src;\n"
    "    return;\n"
    "  }\n"
    /* 非普通混合：从背景纹理取色，按 CSS 公式算，再与 src 的 alpha 合成 */
    "  vec4 bd = texture(u_backdrop, uv);\n"
    "  vec3 cb = bd.rgb;\n"
    "  vec3 cs = src.rgb / max(src.a, 1e-5);\n"
    "  vec3 bb = vec3(blend_ch(cb.r, cs.r, u_mode), blend_ch(cb.g, cs.g, u_mode),\n"
    "                 blend_ch(cb.b, cs.b, u_mode));\n"
    "  vec3 outv = cb * (1.0 - src.a) + bb * src.a;\n"
    "  frag = vec4(outv, 1.0);\n"
    "}\n";

/* 4 通道（预乘）版本的 9 抽头高斯：alpha 一起糊，输出不强制 1 */
static const char *BLUR_RGBA_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform vec2 u_step;\n"
    "uniform vec2 u_uvmax;\n"
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  const float w[5] = float[5](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);\n"
    "  vec2 c = clamp(v_uv, vec2(0.0), u_uvmax);\n"
    "  vec4 acc = texture(u_tex, c) * w[0];\n"
    "  for (int i = 1; i < 5; i++) {\n"
    "    vec2 o = u_step * float(i);\n"
    "    acc += texture(u_tex, clamp(c + o, vec2(0.0), u_uvmax)) * w[i];\n"
    "    acc += texture(u_tex, clamp(c - o, vec2(0.0), u_uvmax)) * w[i];\n"
    "  }\n"
    "  frag = acc;\n"
    "}\n";

static GLuint build_blur_rgba_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, BLUR_VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, BLUR_RGBA_FS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, (GLsizei)sizeof(log), NULL, log);
        QL_LOGE("gl: layer blur program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint build_layer_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, BLUR_VS);   /* 顶点与模糊那套一样 */
    GLuint fs = compile(GL_FRAGMENT_SHADER, LAYER_FS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, (GLsizei)sizeof(log), NULL, log);
        QL_LOGE("gl: layer program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* 让第 i 层有一张 >= w x h 的 RGBA 纹理 + FBO（只增不减） */
static int layer_ensure(QLGfxGL *g, int i, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    struct QLGLLayer *L = &g->layer[i];
    if (L->tex != 0 && L->w >= w && L->h >= h)
        return 1;

    /*
     * 建纹理/FBO 会临时切目标。**必须存/还原当前绑定**：嵌套图层时外层正
     * 画在自己的 FBO 上，这里一句 `glBindFramebuffer(0)` 就把外层的输出
     * 全丢到默认帧缓冲去了（表现：外层图层整个不见）。
     */
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    if (L->tex != 0) {
        glDeleteTextures(1, &L->tex);
        glDeleteFramebuffers(1, &L->fbo);
        L->tex = 0;
        L->fbo = 0;
    }
    glGenTextures(1, &L->tex);
    glBindTexture(GL_TEXTURE_2D, L->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &L->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, L->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, L->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        QL_LOGE("gl: layer FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    L->w = w;
    L->h = h;
    return 1;
}

/* 背景（混合模式用）：一张按需增长的纹理 + FBO */
static int layer_bd_ensure(QLGfxGL *g, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g->layer_bd_tex != 0 && g->layer_bd_w >= w && g->layer_bd_h >= h)
        return 1;
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    if (g->layer_bd_tex != 0) {
        glDeleteTextures(1, &g->layer_bd_tex);
        glDeleteFramebuffers(1, &g->layer_bd_fbo);
        g->layer_bd_tex = 0;
        g->layer_bd_fbo = 0;
    }
    glGenTextures(1, &g->layer_bd_tex);
    glBindTexture(GL_TEXTURE_2D, g->layer_bd_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g->layer_bd_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->layer_bd_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g->layer_bd_tex, 0);
    const int ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    if (!ok) {
        QL_LOGE("gl: layer backdrop FBO incomplete");
        return 0;
    }
    g->layer_bd_w = w;
    g->layer_bd_h = h;
    return 1;
}

/* 画一个覆盖 0..1 的全屏四边形（与模糊那套共用 vbo） */
static void layer_draw_quad(QLGfxGL *g)
{
    static const float quad[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, g->layer_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

/* 把一张纹理拷进模糊 ping-pong 的 tex[0]（step 0 = 只做一次 9 抽头归一化拷贝） */
static void layer_copy_into_blur(QLGfxGL *g, GLuint src_tex, int w, int h)
{
    if (!blur_ensure(g, w, h))
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[1]);
    glViewport(0, 0, g->blur_w, g->blur_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(g->blur_rgba_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(g->bu_tex, 0);
    glUniform2f(g->bu_texel, 1.0f / (GLfloat)g->blur_w, 1.0f / (GLfloat)g->blur_h);
    glUniform2f(g->bu_step, 0.0f, 0.0f);
    glUniform2f(g->bu_uvmax, (GLfloat)w / (GLfloat)g->blur_w,
                (GLfloat)h / (GLfloat)g->blur_h);
    layer_draw_quad(g);
    /* blur_fbo[1] 的结果再搬到 tex[0]（用同一趟拷贝） */
    glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[0]);
    glScissor(0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, g->blur_tex[1]);
    layer_draw_quad(g);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g->cur_target);
}

/* 对 tex[0] 里的内容做两趟可分离模糊（结果仍在 tex[0]） */
/* 一趟 4 通道模糊：tex[src] → fbo[dst] */
static void layer_blur_pass(QLGfxGL *g, int src, int dst, float step_px, int horizontal,
                            float uw, float uh)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[dst]);
    glViewport(0, 0, g->blur_w, g->blur_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)(uw * (float)g->blur_w), (GLsizei)(uh * (float)g->blur_h));
    glDisable(GL_BLEND);
    glUseProgram(g->blur_rgba_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->blur_tex[src]);
    glUniform1i(g->bu_tex, 0);
    glUniform2f(g->bu_texel, 1.0f / (GLfloat)g->blur_w, 1.0f / (GLfloat)g->blur_h);
    glUniform2f(g->bu_step, horizontal ? step_px / (GLfloat)g->blur_w : 0.0f,
                horizontal ? 0.0f : step_px / (GLfloat)g->blur_h);
    glUniform2f(g->bu_uvmax, uw, uh);
    layer_draw_quad(g);
}

static void layer_blur_tex(QLGfxGL *g, int w, int h, int radius)
{
    if (radius <= 0 || !blur_ensure(g, w, h))
        return;
    const float uw = (GLfloat)w / (GLfloat)g->blur_w;
    const float uh = (GLfloat)h / (GLfloat)g->blur_h;
    const float step_px = (float)((radius + 1) / 2);   /* 与 CPU 侧同一步长 */
    layer_blur_pass(g, 0, 1, step_px, 1, uw, uh);
    layer_blur_pass(g, 1, 0, step_px, 0, uw, uh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/*
 * 把渲染目标、viewport、scissor 切回主表面。
 *
 * 模糊那几趟会绑定 blur FBO 并把 viewport 改成模糊纹理的尺寸 —— 不切回来的话，
 * 后面合成的四边形会按错误的投影画出去（表现：图层完全不见）。
 */
static void gl_bind_target(QLGfxGL *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g->cur_target);
    glViewport(0, 0, (GLsizei)g->width, (GLsizei)g->height);
    glEnable(GL_SCISSOR_TEST);
    if (g->clip_active)
        glScissor((int)g->clip.x, (int)(g->height - ql_rect_bottom(g->clip)),
                  (GLsizei)g->clip.w, (GLsizei)g->clip.h);
    else
        glScissor(0, 0, (GLsizei)g->width, (GLsizei)g->height);
}

/* 上传一张 mask 图到第 i 层的纹理（按像素指针做缓存键） */
static GLuint layer_mask_tex(QLGfxGL *g, int i, const QLImage *m)
{
    struct QLGLLayer *L = &g->layer[i];
    if (m == NULL || m->px == NULL)
        return g->bg_dummy_tex;
    if (L->mask_tex != 0 && L->mask_key == m->px)
        return L->mask_tex;

    /* 统一展开成 RGBA8888（只用到 alpha，但复用一条上传路径最省事） */
    const int w = (int)m->w, h = (int)m->h;
    GLuint tex = 0;
    for (int fmt_try = 0; fmt_try < 2; fmt_try++) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (m->format == QL_IMAGE_RGBA8888) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         m->px);
            break;
        }
        if (m->format == QL_IMAGE_A8) {
            /* A8 → 单通道纹理，采样 .a 恒为 1，所以片元里读 .r 更保险；
             * 这里统一升成 RGBA，alpha 与亮度都填 A8 的值。 */
            uint8_t *buf = (uint8_t *)ql_mem_alloc((size_t)w * (size_t)h * 4u);
            if (buf == NULL) {
                glDeleteTextures(1, &tex);
                return g->bg_dummy_tex;
            }
            for (int y = 0; y < h; y++) {
                const uint8_t *row = m->px + (size_t)y * m->stride;
                for (int x = 0; x < w; x++) {
                    const uint8_t a = row[x];
                    uint8_t *o = buf + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    o[0] = o[1] = o[2] = a;
                    o[3] = a;
                }
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         buf);
            ql_mem_free(buf);
            break;
        }
        /* 其他格式（RGB565/RGB888）：当成"全不遮"，不建纹理 */
        glDeleteTextures(1, &tex);
        tex = 0;
        break;
    }
    if (L->mask_tex != 0)
        glDeleteTextures(1, &L->mask_tex);
    L->mask_tex = tex;
    L->mask_key = m->px;
    return (tex != 0) ? tex : g->bg_dummy_tex;
}

/*
 * CSS px 矩形 → 设备像素整数矩形（与 CPU 侧 cpu_layer_dev_rect 同一口径）。
 *
 * **必须把绘制期平移算进去**：`gsx()` 只做了 elem scale 与 dpr，平移是顶点
 * 发射时才加的。图层的 FBO 尺寸 / 位置如果按 `gsx` 算，嵌套图层就会错位
 * （外层图层已经在自己的坐标系里了，内层还按"整屏"算），表现为内容被压缩。
 */
static QLRect gl_layer_dev_rect(QLGfxGL *g, QLRect bounds)
{
    const QLRect e = elem_rect_gl(g, bounds);
    const ql_coord_t scale = (g->scale_fx > 0) ? g->scale_fx : QL_SCALE_ONE;
    const int64_t ax = (int64_t)e.x * QL_FX_ONE + g->draw.offset_x_fx;
    const int64_t ay = (int64_t)e.y * QL_FX_ONE + g->draw.offset_y_fx;
    const int64_t bx = (int64_t)(e.x + e.w) * QL_FX_ONE + g->draw.offset_x_fx;
    const int64_t by = (int64_t)(e.y + e.h) * QL_FX_ONE + g->draw.offset_y_fx;
    /* 设备 26.6 → 整数像素，向外取整（宁大勿小） */
    const ql_coord_t x0 = (ql_coord_t)(((ax * scale) >> QL_FX_SHIFT) >> QL_FX_SHIFT);
    const ql_coord_t y0 = (ql_coord_t)(((ay * scale) >> QL_FX_SHIFT) >> QL_FX_SHIFT);
    const ql_coord_t x1 =
        (ql_coord_t)(((((bx * scale) >> QL_FX_SHIFT) + QL_FX_ONE - 1) >> QL_FX_SHIFT));
    const ql_coord_t y1 =
        (ql_coord_t)(((((by * scale) >> QL_FX_SHIFT) + QL_FX_ONE - 1) >> QL_FX_SHIFT));
    if (x1 <= x0 || y1 <= y0)
        return ql_rect_make(0, 0, 0, 0);
    return ql_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static int gl_layer_begin(QLDraw2D *self, QLRect bounds, const QLDraw2DLayerFx *fx)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL || fx == NULL || g->layer_prog == 0)
        return 0;
    if (g->layer_depth >= QL_CFG_UI_LAYER_MAX_DEPTH)
        return 0;

    const QLRect dev = gl_layer_dev_rect(g, bounds);
    if (ql_rect_is_empty(dev))
        return 0;
    const int i = g->layer_depth;
    if (!layer_ensure(g, i, (int)dev.w, (int)dev.h))
        return 0;

    struct QLGLLayer *L = &g->layer[i];
    flush_batch(g);           /* 之前攒的几何先落到原目标上 */

    {
        GLint cur = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        L->saved_fbo = cur;
        g->cur_target = (GLint)L->fbo;
    }
    L->dev = dev;
    L->fx = *fx;
    L->saved_w = g->width;
    L->saved_h = g->height;
    L->saved_clip = g->clip;
    L->saved_clip_active = g->clip_active;

    glBindFramebuffer(GL_FRAMEBUFFER, L->fbo);
    glViewport(0, 0, (GLsizei)dev.w, (GLsizei)dev.h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)dev.w, (GLsizei)dev.h);

    /* 之后的顶点按"图层左上角为原点"生成 */
    g->width = dev.w;
    g->height = dev.h;
    g->clip = ql_rect_make(0, 0, dev.w, dev.h);
    g->clip_active = 0;
    self->offset_x_fx -= (ql_coord_t)(((int64_t)dev.x * QL_FX_ONE * QL_FX_ONE) /
                                     ((g->scale_fx > 0) ? g->scale_fx : QL_SCALE_ONE));
    self->offset_y_fx -= (ql_coord_t)(((int64_t)dev.y * QL_FX_ONE * QL_FX_ONE) /
                                     ((g->scale_fx > 0) ? g->scale_fx : QL_SCALE_ONE));
    g->layer_depth++;
    return 1;
}

/* 把第 i 层合成回原目标。`pass` 见 LAYER_FS */
static void gl_layer_draw(QLGfxGL *g, int i, int pass, GLuint tex, GLuint blur_tex,
                          GLuint backdrop_tex, GLuint mask_tex, int blend_on,
                          GLuint blur_tex_w, GLuint blur_tex_h)
{
    struct QLGLLayer *L = &g->layer[i];
    const QLDraw2DLayerFx *fx = &L->fx;

    glUseProgram(g->layer_prog);
    /* dp 一律取"相对目标表面的设备 y（向下）"：图层目标就是它自己的 FBO 坐标 */
    glUniform1i(g->lu_flipy, 1);
    glUniform2f(g->lu_viewport, (GLfloat)g->width, (GLfloat)g->height);
    glUniform2f(g->lu_texel,
                1.0f / (GLfloat)((L->dev.w > 0) ? L->dev.w : 1),
                1.0f / (GLfloat)((L->dev.h > 0) ? L->dev.h : 1));
    glUniform1f(g->lu_opacity, (GLfloat)fx->opacity / 255.0f);
    glUniform1i(g->lu_mode, blend_on ? (int)fx->blend : 0);
    glUniform1i(g->lu_pass, pass);
    {
        /*
         * 圆角裁剪与遮罩拉伸都以**元素的边框盒**为准，不是外扩后的图层矩形：
         * 图层矩形含滤镜外扩（blur 半径那么一圈），拿它做圆角的话
         * 模糊光晕会跑到圆角外头去（CPU 侧用的是 fx->box，这里是同一口径）。
         */
        QLRect b = gl_layer_dev_rect(g, fx->box);
        glUniform4f(g->lu_box, (GLfloat)b.x, (GLfloat)b.y, (GLfloat)b.w, (GLfloat)b.h);
    }
    glUniform1f(g->lu_radius, (GLfloat)fx->corner);
    glUniform1i(g->lu_nops, (int)fx->nops);
    for (int k = 0; k < 6; k++) {
        GLfloat kind = 0.0f, arg = 0.0f;
        if (k < (int)fx->nops) {
            kind = (GLfloat)QL_UI_FILTER_KIND(fx->op[k]);
            arg = (GLfloat)QL_UI_FILTER_ARG(fx->op[k]) / 1000.0f;
            if (QL_UI_FILTER_KIND(fx->op[k]) == QL_UI_FILTER_HUE_ROTATE)
                arg = (GLfloat)QL_UI_FILTER_ARG(fx->op[k]);   /* 角度 */
        }
        glUniform2f(g->lu_op + k, kind, arg);
    }
    glUniform4f(g->lu_ds, (GLfloat)fx->ds_dx, (GLfloat)fx->ds_dy, (GLfloat)fx->ds_blur,
                (GLfloat)((fx->ds_a != 0) ? fx->ds_a : 255) / 255.0f);
    glUniform3f(g->lu_ds_color, (GLfloat)QL_COLOR_GET_R(fx->ds_color) / 255.0f,
                (GLfloat)QL_COLOR_GET_G(fx->ds_color) / 255.0f,
                (GLfloat)QL_COLOR_GET_B(fx->ds_color) / 255.0f);

    glUniform2f(g->lu_uv_tex, (GLfloat)L->dev.w / (GLfloat)((L->w > 0) ? L->w : 1),
                (GLfloat)L->dev.h / (GLfloat)((L->h > 0) ? L->h : 1));
    if (blur_tex != 0 && blur_tex_w > 0 && blur_tex_h > 0)
        glUniform2f(g->lu_uv_blur, (GLfloat)L->dev.w / (GLfloat)blur_tex_w,
                    (GLfloat)L->dev.h / (GLfloat)blur_tex_h);
    else
        glUniform2f(g->lu_uv_blur, (GLfloat)L->dev.w / (GLfloat)((L->w > 0) ? L->w : 1),
                    (GLfloat)L->dev.h / (GLfloat)((L->h > 0) ? L->h : 1));

    int flags = 0;
    if (blur_tex != 0) flags |= 1;
    if (backdrop_tex != 0) flags |= 2;
    if (mask_tex != 0) flags |= 4;
    glUniform1i(g->lu_flags, flags);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g->lu_tex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (blur_tex != 0) ? blur_tex : g->bg_dummy_tex);
    glUniform1i(g->lu_blur, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (backdrop_tex != 0) ? backdrop_tex : g->bg_dummy_tex);
    glUniform1i(g->lu_backdrop, 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (mask_tex != 0) ? mask_tex : g->bg_dummy_tex);
    glUniform1i(g->lu_mask, 3);

    if (blend_on) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    /* 四边形覆盖图层矩形：用 uv 0..1 与目标设备坐标都算得出来 */
    /* 有非轴对齐变换时：圆角/遮罩改在局部坐标里判，四边形用变换后的四角 */
    const int use_xform = (fx->has_xform != 0);
    glUniform1i(g->lu_local, use_xform ? 1 : 0);
    glUniform4f(g->lu_lbounds, (GLfloat)fx->bounds_css.x, (GLfloat)fx->bounds_css.y,
                (GLfloat)fx->bounds_css.w, (GLfloat)fx->bounds_css.h);

    {
        /*
         * 顶点是**归一化表面坐标**（BLUR_VS 里再 *2-1 转成 NDC），不是设备像素。
         * y 要翻转：CSS 的 y 向下，GL 的 NDC y 向上；而图层纹理的第 0 行
         * 正是 CSS 的顶边（渲染进 FBO 时 y=0 落在纹理底部），所以 uv.y=0
         * 对应 CSS 顶边 —— 两边一致，不需要额外的 uv 翻转。
         */
        /*
         * 目标是窗口本身还是外层图层的 FBO，两者的尺寸都已经落在 g->width/height
         * 里；图层矩形也是相对当前目标的（进图层时 offset 已经减掉了外层原点）。
         */
        const float vw = (float)((g->width > 0) ? g->width : 1);
        const float vh = (float)((g->height > 0) ? g->height : 1);
        const float u0 = (float)L->dev.x / vw;
        const float u1 = (float)(L->dev.x + L->dev.w) / vw;
        /*
         * y 要翻：BLUR_VS 把 [0,1] 的表面坐标映射成 NDC（+1 在上），而目标的
         * 内容是按"CSS y 向下"画的（主 VS 里 `ndc.y = -ndc.y`），所以设备顶边
         * 落在表面坐标的 1 那一侧。
         */
        const float t0 = 1.0f - (float)L->dev.y / vh;
        const float t1 = 1.0f - (float)(L->dev.y + L->dev.h) / vh;
        float quad[16] = {
            u0, t0, 0.0f, 1.0f,
            u1, t0, 1.0f, 1.0f,
            u0, t1, 0.0f, 0.0f,
            u1, t1, 1.0f, 0.0f,
        };
        if (use_xform) {
            /*
             * 旋转 / 斜切：把四个角按矩阵摆过去。
             * 矩阵定义在 CSS px 空间；设备空间是同一份线性变换，只是平移与原点
             * 要乘 dpr（等比缩放与线性变换可交换）。
             * 四边形仍是平行四边形，所以 uv 在它上面的双线性插值 = 精确的仿射映射。
             */
            const float dpr = (g->scale_fx > 0) ? (float)g->scale_fx / (float)QL_SCALE_ONE
                                                : 1.0f;
            const float ma = (float)fx->mat[0] / (float)QL_FX_ONE;
            const float mb = (float)fx->mat[1] / (float)QL_FX_ONE;
            const float mc = (float)fx->mat[2] / (float)QL_FX_ONE;
            const float md = (float)fx->mat[3] / (float)QL_FX_ONE;
            const float me = (float)fx->mat[4] / (float)QL_FX_ONE * dpr;
            const float mf = (float)fx->mat[5] / (float)QL_FX_ONE * dpr;
            const float ox = (float)fx->origin_x * dpr;
            const float oy = (float)fx->origin_y * dpr;
            const float dx[4] = { (float)L->dev.x, (float)(L->dev.x + L->dev.w),
                                  (float)L->dev.x, (float)(L->dev.x + L->dev.w) };
            const float dy[4] = { (float)L->dev.y, (float)L->dev.y,
                                  (float)(L->dev.y + L->dev.h),
                                  (float)(L->dev.y + L->dev.h) };
            for (int k = 0; k < 4; k++) {
                const float ux = dx[k] - ox;
                const float uy = dy[k] - oy;
                const float nx = ox + ma * ux + mc * uy + me;
                const float ny = oy + mb * ux + md * uy + mf;
                quad[k * 4 + 0] = nx / vw;
                quad[k * 4 + 1] = 1.0f - ny / vh;
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, g->layer_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void *)(2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        (void)vw;
        (void)vh;
    }
    glActiveTexture(GL_TEXTURE0);
}

static int gl_layer_end(QLDraw2D *self)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL || g->layer_depth <= 0)
        return 0;
    const int i = g->layer_depth - 1;
    struct QLGLLayer *L = &g->layer[i];
    const QLDraw2DLayerFx fx = L->fx;

    flush_batch(g);           /* 图层内容全部写进 FBO */
    /* 恢复目标表面与坐标（嵌套时这里要回到外层图层的 FBO，不是主表面） */
    g->cur_target = L->saved_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)L->saved_fbo);
    g->width = L->saved_w;
    g->height = L->saved_h;
    g->clip = L->saved_clip;
    g->clip_active = L->saved_clip_active;
    glViewport(0, 0, (GLsizei)g->width, (GLsizei)g->height);
    self->offset_x_fx += (ql_coord_t)(((int64_t)L->dev.x * QL_FX_ONE * QL_FX_ONE) /
                                      ((g->scale_fx > 0) ? g->scale_fx : QL_SCALE_ONE));
    self->offset_y_fx += (ql_coord_t)(((int64_t)L->dev.y * QL_FX_ONE * QL_FX_ONE) /
                                      ((g->scale_fx > 0) ? g->scale_fx : QL_SCALE_ONE));
    glEnable(GL_SCISSOR_TEST);
    if (g->clip_active)
        glScissor((int)g->clip.x, (int)(g->height - ql_rect_bottom(g->clip)),
                  (GLsizei)g->clip.w, (GLsizei)g->clip.h);
    else
        glScissor(0, 0, (GLsizei)g->width, (GLsizei)g->height);

    /* 背景（混合模式要读它）：把目标表面当前内容抓一份 */
    GLuint backdrop = 0;
    if (fx.blend != QL_UI_BLEND_NORMAL && layer_bd_ensure(g, (int)L->dev.w, (int)L->dev.h)) {
        const QLRect d = L->dev;
        /*
         * **blit 也会受 scissor 影响**（片元流水线之外的少数例外之一）。
         * 脏区重绘时 scissor 是"这一小块脏区"的设备坐标，而这张背景纹理的
         * 目标矩形在 (0,0)，两块不重叠 → 整次拷贝被静默丢弃，混合模式读到的
         * 背景是黑的（multiply/overlay/darken 全黑、screen/difference 退化成原色）。
         * 离屏全屏重绘时 scissor 恰好覆盖 (0,0)，所以对拍用例一直是绿的。
         * 另外两处 blit（毛玻璃降采样、帧末整块上屏）早就这么做了，只有这里漏了。
         */
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)g->cur_target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->layer_bd_fbo);
        /* GL 的 y 轴朝上：把目标表面的 y 翻过来 */
        glBlitFramebuffer((int)d.x, (int)(g->height - (d.y + d.h)),
                          (int)(d.x + d.w), (int)(g->height - d.y), 0, 0, (int)d.w,
                          (int)d.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g->cur_target);
        backdrop = g->layer_bd_tex;
    }

    GLuint mask_tex = 0;
    if (fx.mask != NULL)
        mask_tex = layer_mask_tex(g, i, fx.mask);

    /* drop-shadow 先画（在图层下面）：它自己要用一趟模糊 */
    if (fx.has_drop) {
        if (fx.ds_blur > 0) {
            layer_copy_into_blur(g, L->tex, (int)L->dev.w, (int)L->dev.h);
            layer_blur_tex(g, (int)L->dev.w, (int)L->dev.h, (int)fx.ds_blur);
            gl_bind_target(g);
            gl_layer_draw(g, i, 0, L->tex, g->blur_tex[0], 0, mask_tex, 0,
                          (GLuint)g->blur_w, (GLuint)g->blur_h);
        } else {
            gl_bind_target(g);
            gl_layer_draw(g, i, 0, L->tex, L->tex, 0, mask_tex, 0, 0, 0);
        }
    }
    /* 图层本体：有 blur 就把模糊结果当源 */
    GLuint src_blur = 0;
    if (fx.blur > 0) {
        layer_copy_into_blur(g, L->tex, (int)L->dev.w, (int)L->dev.h);
        layer_blur_tex(g, (int)L->dev.w, (int)L->dev.h, (int)fx.blur);
        src_blur = g->blur_tex[0];
    }
    gl_bind_target(g);
    gl_layer_draw(g, i, 1, L->tex, src_blur, backdrop, mask_tex,
                  fx.blend != QL_UI_BLEND_NORMAL,
                  (src_blur != 0) ? (GLuint)g->blur_w : 0,
                  (src_blur != 0) ? (GLuint)g->blur_h : 0);

    /*
     * 把混合状态**还给主批次**：图层合成用的是"预乘"公式
     * （ONE / ONE_MINUS_SRC_ALPHA），而主批次是"直通色"公式
     * （SRC_ALPHA / ONE_MINUS_SRC_ALPHA）。
     *
     * 这里曾经直接 `glDisable(GL_BLEND)`，结果图层之后画的**所有文字都变成
     * 实心色块**（字形覆盖率乘不上去了）—— 而且只在真窗口里看得出：
     * 离屏对拍的用例里没有文字，一直是绿的。
     */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    L->fx.has_drop = 0;
    g->layer_depth = i;
    return 0;
}
#endif /* QL_FEAT_CSS_FILTER_LAYER */

/* ------------------------------ 生命周期 ------------------------------ */

static void now_us(struct timespec *t)
{
    clock_gettime(CLOCK_MONOTONIC, t);
}

/* 单调时钟的毫秒数（32 位，回绕按差值判断，与动画时钟同一套约定） */
static uint64_t now_us_mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

QLGfxGL *ql_gfx_gl_create(const char *title, ql_coord_t width, ql_coord_t height,
                          uint32_t flags)
{
    if (width <= 0 || height <= 0)
        return NULL;

    QLGfxGL *g = (QLGfxGL *)ql_mem_calloc(1, sizeof(QLGfxGL));
    if (g == NULL)
        return NULL;

    g->width = width;
    g->height = height;
    g->scale_fx = QL_SCALE_ONE;
    g->offscreen = (flags & QL_GFX_GL_OFFSCREEN) ? 1 : 0;
    g->mask_rx = -1.0f;
    g->mask_ry = -1.0f;
    g->draw.mask_r = -1;
    g->draw.mask_ry = -1;
    g->draw.elem_x_fx = QL_SCALE_ONE;
    g->draw.elem_y_fx = QL_SCALE_ONE;
    g->vert_capacity = QL_GL_MAX_VERTS;
    g->verts = (QLGLVertex *)ql_mem_calloc(QL_GL_MAX_VERTS, sizeof(QLGLVertex));
    g->uv = (QLGLUVEntry *)ql_mem_calloc(QL_GL_UV_SLOTS, sizeof(QLGLUVEntry));
    if (g->verts == NULL || g->uv == NULL) {
        ql_mem_free(g->verts);
        ql_mem_free(g->uv);
        ql_mem_free(g);
        return NULL;
    }

    /* --- X11 窗口（离屏模式不建窗口） --- */
    g->dpy = XOpenDisplay(NULL);
    if (g->dpy == NULL) {
        QL_LOGE("gl: cannot open X display (DISPLAY=%s)",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        goto fail;
    }

    int screen = DefaultScreen(g->dpy);

    /* --- EGL --- */
    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint num_cfg = 0;

    /*
     * 必须显式指定 X11 平台：在 Wayland 会话里 eglGetDisplay() 会自动选
     * Wayland 平台，然后把 X11 的 Display* 当 wl_display* 用 → 直接段错误。
     */
    g->egdpy = EGL_NO_DISPLAY;
#if defined(EGL_PLATFORM_X11_KHR)
    {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display != NULL) {
            g->egdpy = get_platform_display(EGL_PLATFORM_X11_KHR,
                                            (void *)g->dpy, NULL);
        }
    }
#endif
    if (g->egdpy == EGL_NO_DISPLAY)
        g->egdpy = eglGetDisplay((EGLNativeDisplayType)g->dpy);
    if (g->egdpy == EGL_NO_DISPLAY || !eglInitialize(g->egdpy, NULL, NULL)) {
        QL_LOGE("gl: eglInitialize failed");
        goto fail;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        QL_LOGE("gl: eglBindAPI(ES) failed");
        goto fail;
    }
    if (!eglChooseConfig(g->egdpy, cfg_attrs, &cfg, 1, &num_cfg) || num_cfg < 1) {
        QL_LOGE("gl: eglChooseConfig failed (no ES3 config)");
        goto fail;
    }

    if (g->offscreen) {
        static const EGLint pb_attrs[] = {
            EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE,
        };
        EGLint pb_real[5];
        pb_real[0] = EGL_WIDTH;  pb_real[1] = width;
        pb_real[2] = EGL_HEIGHT; pb_real[3] = height;
        pb_real[4] = EGL_NONE;
        QL_UNUSED(pb_attrs);
        g->surface = eglCreatePbufferSurface(g->egdpy, cfg, pb_real);
    } else {
        XVisualInfo vinfo;
        if (!XMatchVisualInfo(g->dpy, screen, 24, TrueColor, &vinfo) &&
            !XMatchVisualInfo(g->dpy, screen, 32, TrueColor, &vinfo)) {
            QL_LOGE("gl: no TrueColor visual found");
            goto fail;
        }
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.colormap = XCreateColormap(g->dpy, RootWindow(g->dpy, screen),
                                       vinfo.visual, AllocNone);
        swa.border_pixel = 0;
        /*
         * `FocusChangeMask` 是 **XIM 必需的**：输入法只在输入上下文"有焦点"时
         * 才截键（`XSetICFocus` 在 FocusIn 里调）。少这一位就永远收不到 FocusIn，
         * 输入法于是完全不理会按键 —— 表现就是"ibus/fcitx 都装了、拼音也打了，
         * 但输入框里只有字母"。这个位不影响别的逻辑（焦点事件在 switch 里处理）。
         */
        swa.event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask |
                         KeyPressMask | KeyReleaseMask |
                         ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | EnterWindowMask | LeaveWindowMask;
        g->win = XCreateWindow(g->dpy, RootWindow(g->dpy, screen), 0, 0,
                               (unsigned)width, (unsigned)height, 0, vinfo.depth,
                               InputOutput, vinfo.visual,
                               CWBorderPixel | CWColormap | CWEventMask, &swa);
        if (g->win == 0) {
            QL_LOGE("gl: XCreateWindow failed");
            goto fail;
        }
        x11_set_title(g->dpy, g->win, (title != NULL) ? title : "QLGL GL");
        g->wm_delete = XInternAtom(g->dpy, "WM_DELETE_WINDOW", False);
        g->wm_protocols = XInternAtom(g->dpy, "WM_PROTOCOLS", False);
        XSetWMProtocols(g->dpy, g->win, &g->wm_delete, 1);
#ifdef QLGL_HAVE_XSYNC
        /*
         * _NET_WM_SYNC_REQUEST：把"我正在缩放，画完这一帧会告诉你"这套协议接上。
         *
         * 不接的话，窗口管理器在交互式缩放时不知道我们何时画完：
         * 它可能连着发好几次 ConfigureNotify、也可能等一阵再把尺寸一次跳到位，
         * 肉眼就是"拖动时窗口大小停住、然后突然跳到目标大小"。
         * 接上之后 WM 每发一次 resize 都等我们回执，节奏和我们的帧对齐。
         * GTK / Qt 都这么做，这是 X11 上"丝滑缩放"的标准姿势。
         */
        g->net_wm_sync_request = XInternAtom(g->dpy, "_NET_WM_SYNC_REQUEST", False);
        {
            XSyncValue zero;
            XSyncIntToValue(&zero, 0);
            g->sync_counter = XSyncCreateCounter(g->dpy, zero);
        }
        if (g->sync_counter != None) {
            const Atom prop = XInternAtom(g->dpy, "_NET_WM_SYNC_REQUEST_COUNTER", False);
            const long xid = (long)g->sync_counter;
            XChangeProperty(g->dpy, g->win, prop, XA_CARDINAL, 32, PropModeReplace,
                            (const unsigned char *)&xid, 1);

            Atom protos[2];
            protos[0] = g->wm_delete;
            protos[1] = g->net_wm_sync_request;
            XSetWMProtocols(g->dpy, g->win, protos, 2);
        }
#endif
        XMapWindow(g->dpy, g->win);

        /*
         * 输入法（fcitx5 / ibus 都提供 XIM 服务）。拿不到就降级成"只能打 Latin-1"
         * —— 很多精简环境根本没装输入法，那不是错误。
         *
         * XIM 要三件事**同时**成立，缺一条就"装了输入法也打不出中文"：
         *   1. 进程调用过 `setlocale(LC_ALL, "")`（例程在 ex_setup_locale 里做）；
         *   2. `XSetLocaleModifiers("")` —— 读 `XMODIFIERS` 并初始化 XIM 的传输层。
         *      漏掉这一句时 `XOpenIM` 照样成功、IC 也建得出来、`XNFilterEvents`
         *      也报 0x3，但 `XFilterEvent()` **永远返回 False**，输入法根本不接管
         *      按键 —— 表现就是"拼音敲进去只有字母"。这条是实测出来的（见
         *      《QLRender 阅读指南》踩坑表）。
         *   3. 窗口选上 `FocusChangeMask`，收到 FocusIn 后 `XSetICFocus()`
         *      （输入法只给"有焦点的输入上下文"转事件）。
         */
        if (!XSupportsLocale())
            QL_LOGW("gl: locale 不支持（没有 setlocale(LC_ALL, \"\")？）—— XIM 可能不可用");
        (void)XSetLocaleModifiers("");
        g->xim = XOpenIM(g->dpy, NULL, NULL, NULL);
        if (g->xim == NULL) {
            /* `XMODIFIERS` 没设也很常见（比如从没继承桌面会话环境）：挨个试常见的 */
            static const char *const alts[] = { "@im=ibus", "@im=fcitx" };
            for (size_t i = 0; i < sizeof(alts) / sizeof(alts[0]) && g->xim == NULL; i++) {
                (void)XSetLocaleModifiers(alts[i]);
                g->xim = XOpenIM(g->dpy, NULL, NULL, NULL);
                if (g->xim != NULL)
                    QL_LOGI("gl: XMODIFIERS 没设，改用 %s 连上了输入法", alts[i]);
            }
        }
        if (g->xim != NULL) {
            g->xic = XCreateIC(g->xim,
                               XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                               XNClientWindow, g->win,
                               XNFocusWindow, g->win,
                               NULL);
        }
        if (g->xic != NULL) {
            /*
             * 输入法会声明"我还需要关注哪些事件"（通常是 KeyPress/KeyRelease，
             * 有的还会要 PropertyNotify / ClientMessage 之类）。必须把这些位
             * 并进窗口的事件掩码，否则那些事件根本不会送到我们这里，
             * 输入法就收不到它想要的通知。
             */
            long im_events = NoEventMask;
            if (XGetICValues(g->xic, XNFilterEvents, &im_events, NULL) == NULL &&
                im_events != NoEventMask) {
                XSelectInput(g->dpy, g->win, swa.event_mask | im_events);
            }
            /*
             * 窗口可能在建 IC 之前就已经拿到焦点（比如刚启动就被 WM 激活），
             * 那时没有 FocusIn 可等 —— 主动看一眼当前焦点，直接设上。
             */
            Window focus_win = None;
            int revert = 0;
            XGetInputFocus(g->dpy, &focus_win, &revert);
            if (focus_win == g->win)
                XSetICFocus(g->xic);
        } else {
            QL_LOGW("gl: 没有可用的 XIM 输入法，只能输入 Latin-1（装好 ibus/fcitx 并保证 "
                    "XMODIFIERS=@im=ibus|fcitx，且进程调过 setlocale(LC_ALL, \"\")）");
        }

        g->surface = eglCreateWindowSurface(g->egdpy, cfg, (EGLNativeWindowType)g->win, NULL);
        g->surface_w = width;
        g->surface_h = height;
    }
    g->cfg = cfg;

    if (g->surface == EGL_NO_SURFACE) {
        QL_LOGE("gl: eglCreateSurface failed (0x%x)", (unsigned)eglGetError());
        goto fail;
    }

    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g->ctx = eglCreateContext(g->egdpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g->ctx == EGL_NO_CONTEXT) {
        QL_LOGE("gl: eglCreateContext failed (0x%x)", (unsigned)eglGetError());
        goto fail;
    }
    if (!eglMakeCurrent(g->egdpy, g->surface, g->surface, g->ctx)) {
        QL_LOGE("gl: eglMakeCurrent failed (0x%x)", (unsigned)eglGetError());
        goto fail;
    }

    /*
     * 默认开垂直同步：拖动边框缩放时，窗口每变一次尺寸就有一帧"新露出的区域
     * 还没画"。不跟 vblank 的话主循环会以几百 Hz 空转、每次 swap 都塞一个新
     * 尺寸的画面给 compositor，看起来就是"黑一下再出来"。
     * 想要跑满帧率的基准测试可以 `ql_gfx_gl_set_vsync(gfx, 0)` 关掉。
     */
    g->vsync_user = 1;
    g->vsync_eff = 1;
    g->pace_us = 16666;    /* 没测到之前按 60Hz 估 */
    /*
     * 环境变量而不是 API：拖动期间不跟 vblank 是为了绕开 Mesa Present 在
     * 重配窗口时等 1s 的老问题（见 ql_gfx_gl_end）。如果某些驱动上看到撕裂，
     * 设 QLGL_KEEP_VSYNC=1 就退回"拖动时也跟 vblank"。
     */
    g->keep_vsync_on_resize = getenv("QLGL_KEEP_VSYNC") != NULL;
    g->trace_im = getenv("QLGL_TRACE_IM") != NULL;
    g->lag_debug = getenv("QLGL_LAG_DEBUG") != NULL;
    if (!g->offscreen)
        eglSwapInterval(g->egdpy, 1);

    QL_LOGI("gl: %s | %s | %s", glGetString(GL_RENDERER), glGetString(GL_VERSION),
            glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (!build_program(g))
        goto fail;

    glGenBuffers(1, &g->vbo);
    glGenTextures(1, &g->atlas_tex);
    atlas_init(g);

    /* 毛玻璃：独立的小程序 + 一个单位四边形 + 1x1 采样器占位 */
    g->blur_prog = build_blur_program();
    glGenBuffers(1, &g->blur_vbo);
    {
        const uint8_t white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &g->bg_dummy_tex);
        glBindTexture(GL_TEXTURE_2D, g->bg_dummy_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
    }
    if (g->blur_prog != 0) {
        g->blur_u_tex = glGetUniformLocation(g->blur_prog, "u_tex");
        g->blur_u_texel = glGetUniformLocation(g->blur_prog, "u_texel");
        g->blur_u_step = glGetUniformLocation(g->blur_prog, "u_step");
        g->blur_u_uvmax = glGetUniformLocation(g->blur_prog, "u_uvmax");
    }

#if QL_FEAT_CSS_FILTER_LAYER
    /* 离屏图层（M9.3）：一个合成程序 + 一条四边形 VBO */
    g->layer_prog = build_layer_program();
    g->blur_rgba_prog = build_blur_rgba_program();
    if (g->blur_rgba_prog != 0) {
        g->bu_tex = glGetUniformLocation(g->blur_rgba_prog, "u_tex");
        g->bu_texel = glGetUniformLocation(g->blur_rgba_prog, "u_texel");
        g->bu_step = glGetUniformLocation(g->blur_rgba_prog, "u_step");
        g->bu_uvmax = glGetUniformLocation(g->blur_rgba_prog, "u_uvmax");
    }
    glGenBuffers(1, &g->layer_vbo);
    if (g->layer_prog != 0) {
        g->lu_tex = glGetUniformLocation(g->layer_prog, "u_tex");
        g->lu_blur = glGetUniformLocation(g->layer_prog, "u_blur");
        g->lu_backdrop = glGetUniformLocation(g->layer_prog, "u_backdrop");
        g->lu_mask = glGetUniformLocation(g->layer_prog, "u_mask");
        g->lu_viewport = glGetUniformLocation(g->layer_prog, "u_viewport");
        g->lu_texel = glGetUniformLocation(g->layer_prog, "u_texel");
        g->lu_uv_tex = glGetUniformLocation(g->layer_prog, "u_uv_tex");
        g->lu_uv_blur = glGetUniformLocation(g->layer_prog, "u_uv_blur");
        g->lu_local = glGetUniformLocation(g->layer_prog, "u_local");
        g->lu_lbounds = glGetUniformLocation(g->layer_prog, "u_lbounds");
        g->lu_opacity = glGetUniformLocation(g->layer_prog, "u_opacity");
        g->lu_mode = glGetUniformLocation(g->layer_prog, "u_mode");
        g->lu_flags = glGetUniformLocation(g->layer_prog, "u_flags");
        g->lu_nops = glGetUniformLocation(g->layer_prog, "u_nops");
        g->lu_op = glGetUniformLocation(g->layer_prog, "u_op[0]");
        g->lu_ds = glGetUniformLocation(g->layer_prog, "u_ds");
        g->lu_ds_color = glGetUniformLocation(g->layer_prog, "u_ds_color");
        g->lu_pass = glGetUniformLocation(g->layer_prog, "u_pass");
        g->lu_flipy = glGetUniformLocation(g->layer_prog, "u_flipy");
        g->lu_box = glGetUniformLocation(g->layer_prog, "u_box");
        g->lu_radius = glGetUniformLocation(g->layer_prog, "u_radius");
    }
#endif

    g->cur_target = 0;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);

    g->clip = ql_rect_make(0, 0, width, height);

    g->draw.set_clip = gl_set_clip;
    g->draw.set_mask = gl_set_mask;
    g->draw.set_mask_polygon = gl_set_mask_polygon;
    g->draw.set_cm = gl_set_cm;
    g->draw.backdrop_blur = gl_backdrop_blur;
    g->draw.backdrop_blur_round = gl_backdrop_blur_round;
#if QL_FEAT_CSS_FILTER_LAYER
    g->draw.layer_begin = gl_layer_begin;
    g->draw.layer_end = gl_layer_end;
#endif
    g->draw.fill_rect = gl_fill_rect;
    g->draw.fill_round_rect = gl_fill_round_rect;
    g->draw.fill_round_rect_gradient = gl_fill_round_rect_gradient;
    g->draw.fill_shadow = gl_fill_shadow;
    g->draw.fill_inset_shadow = gl_fill_inset_shadow;
    g->draw.fill_round_rect4 = gl_fill_round_rect4;
    g->draw.fill_round_rect_gradient4 = gl_fill_round_rect_gradient4;
    g->draw.stroke_round_rect4 = gl_stroke_round_rect4;
    g->draw.stroke_round_rect = gl_stroke_round_rect;
    g->draw.draw_line = ql_draw2d_draw_line;
    g->draw.draw_text = gl_draw_text;
    g->draw.blit_image = gl_blit_image;
    g->draw.flush = NULL;
    g->draw.scale_fx = g->scale_fx;
    g->draw.alpha = 255;       /* 整体透明度默认不透明 */
    g->draw.offset_x_fx = 0;
    g->draw.offset_y_fx = 0;
    g->draw.user = g;

    QL_LOGI("gl: ready %dx%d%s", (int)width, (int)height, g->offscreen ? " (offscreen)" : "");
    return g;

fail:
    ql_gfx_gl_destroy(g);
    return NULL;
}

void ql_gfx_gl_set_scale(QLGfxGL *gfx, ql_coord_t scale_fx)
{
    if (gfx == NULL)
        return;
    gfx->scale_fx = (scale_fx > 0) ? scale_fx : QL_SCALE_ONE;
    gfx->draw.scale_fx = gfx->scale_fx;
}

QLDraw2D *ql_gfx_gl_draw(QLGfxGL *gfx)
{
    return (gfx != NULL) ? &gfx->draw : NULL;
}

void ql_gfx_gl_finish(QLGfxGL *gfx)
{
    if (gfx != NULL && gfx->ctx != EGL_NO_CONTEXT)
        glFinish();
}

void ql_gfx_gl_set_vsync(QLGfxGL *gfx, int on)
{
    if (gfx == NULL || gfx->egdpy == EGL_NO_DISPLAY)
        return;
    gfx->vsync_user = on ? 1 : 0;
    gfx->vsync_eff = gfx->vsync_user;
    eglSwapInterval(gfx->egdpy, gfx->vsync_eff);
}

/* ------------------------- 保留式脏区（FBO 画布） ------------------------- */

static void fbo_release(QLGfxGL *g)
{
    if (g->fbo != 0) {
        glDeleteFramebuffers(1, &g->fbo);
        g->fbo = 0;
    }
    if (g->fbo_tex != 0) {
        glDeleteTextures(1, &g->fbo_tex);
        g->fbo_tex = 0;
    }
    g->fbo_valid = 0;
    g->fbo_fresh = 0;
    g->fbo_w = 0;
    g->fbo_h = 0;
    g->fbo_cap_w = 0;
    g->fbo_cap_h = 0;
}

/* 按当前尺寸建（或复用）FBO。窗口尺寸变了就重建 */
static int fbo_ensure(QLGfxGL *g)
{
    if (!g->retained)
        return 0;

    /* 已有的纹理装得下就直接用（窗口缩小/小幅拖动都不重新分配） */
    if (g->fbo_valid && g->fbo_w == g->width && g->fbo_h == g->height)
        return 1;
    if (g->fbo_valid && g->width <= g->fbo_cap_w && g->height <= g->fbo_cap_h) {
        g->fbo_w = g->width;
        g->fbo_h = g->height;
        g->fbo_fresh = 1;
        return 1;
    }

    /*
     * 需要更大：多留 25% + 至少 256px 的余量，再按 256 对齐。
     *
     * 只按"刚好装下"分配的话，拖边框这种连续小幅增长会一步步触发重建
     * （每步一次 glTexImage2D + 整屏 clear，几毫秒），看起来就是"缩放掉帧"。
     * 一次性多要一点，整个拖拽过程通常只需重建一次。
     */
    const ql_coord_t want_w = g->width + (g->width / 4) + 256;
    const ql_coord_t want_h = g->height + (g->height / 4) + 256;
    const ql_coord_t cap_w = (want_w + 255) & ~(ql_coord_t)255;
    const ql_coord_t cap_h = (want_h + 255) & ~(ql_coord_t)255;
    fbo_release(g);

    glGenTextures(1, &g->fbo_tex);
    glBindTexture(GL_TEXTURE_2D, g->fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)cap_w, (GLsizei)cap_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g->fbo_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (st != GL_FRAMEBUFFER_COMPLETE) {
        QL_LOGW("gl: retained FBO incomplete (0x%x), falling back to full repaint",
                (unsigned)st);
        fbo_release(g);
        return 0;
    }
    g->fbo_valid = 1;
    g->fbo_w = g->width;
    g->fbo_h = g->height;
    g->fbo_cap_w = cap_w;
    g->fbo_cap_h = cap_h;

    /*
     * 新建（或窗口改尺寸后的）FBO 内容是未定义的。必须让下一帧整屏重画——
     * 否则"只清脏区"会把其余部分留在垃圾上（表现为大片纯黑/花屏）。
     */
    g->fbo_fresh = 1;

    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

ql_err_t ql_gfx_gl_set_retained(QLGfxGL *gfx, int on)
{
    if (gfx == NULL)
        return QL_ERR_INVAL;
    gfx->retained = on ? 1 : 0;
    if (!gfx->retained) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fbo_release(gfx);
        return QL_OK;
    }
    /* 立即试建一次：失败就在调用处当场知道，而不是等到第一帧 */
    return fbo_ensure(gfx) ? QL_OK : QL_ERR_NOMEM;
}

/*
 * 把窗口的 X11 背景色设成"这一帧的清除色"。
 *
 * 窗口被拉大时，新增的那块区域由 X 服务器立刻用**窗口背景像素**填满，
 * 我们的下一帧通常要等一次 swap 才到 —— 中间那一小段就是用户看到的黑边。
 * 背景像素跟着清除色走，这一段就变成页面底色，视觉上无感。
 *
 * 只在颜色变化时才发请求（XChangeWindowAttributes 是个往返）。
 */
static void sync_window_background(QLGfxGL *g, ql_color_t clear_color)
{
    if (g->offscreen || g->win == 0 || g->dpy == NULL)
        return;
    const unsigned long px = ((unsigned long)QL_COLOR_GET_R(clear_color) << 16) |
                             ((unsigned long)QL_COLOR_GET_G(clear_color) << 8) |
                             (unsigned long)QL_COLOR_GET_B(clear_color);
    if (g->bg_pixel_set && g->bg_pixel == px)
        return;
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = px;
    XChangeWindowAttributes(g->dpy, g->win, CWBackPixel, &swa);
    g->bg_pixel = px;
    g->bg_pixel_set = 1;
}

/*
 * 窗口表面尺寸跟窗口对不上就重建。
 *
 * EGL 的 X11 窗口表面尺寸是**创建时定死的**（eglCreateWindowSurface 按当时
 * 窗口几何建）。窗口管理器把窗口改大/改小之后，表面不会自己跟着变，于是
 * 我们把 viewport 设成新尺寸、却画进一块老尺寸的缓冲：内容是有的，但**贴着
 * 底边**，上面留一条黑。用户看到的就是"刚打开时上面黑一块，拖一下窗口才好"。
 *
 * 重建要重新 makeCurrent（旧表面被销毁了，当前上下文就悬空了）。
 */
static int surface_sync(QLGfxGL *g)
{
    if (g->offscreen || g->egdpy == EGL_NO_DISPLAY || g->win == 0)
        return 1;
    if (g->width <= 0 || g->height <= 0)
        return 1;                 /* 还没拿到真实尺寸，等下一帧 */
    if (g->surface != EGL_NO_SURFACE && g->surface_w == g->width &&
        g->surface_h == g->height)
        return 1;

    eglMakeCurrent(g->egdpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g->surface != EGL_NO_SURFACE) {
        eglDestroySurface(g->egdpy, g->surface);
        g->surface = EGL_NO_SURFACE;
    }
    g->surface = eglCreateWindowSurface(g->egdpy, g->cfg, (EGLNativeWindowType)g->win,
                                        NULL);
    if (g->surface == EGL_NO_SURFACE) {
        QL_LOGE("gl: 重建窗口表面失败 (0x%x)", (unsigned)eglGetError());
        return 0;
    }
    if (!eglMakeCurrent(g->egdpy, g->surface, g->surface, g->ctx)) {
        QL_LOGE("gl: 重建表面后 eglMakeCurrent 失败 (0x%x)", (unsigned)eglGetError());
        return 0;
    }
    if (g->vsync_eff >= 0 && !g->offscreen)
        eglSwapInterval(g->egdpy, g->vsync_eff);
    g->surface_w = g->width;
    g->surface_h = g->height;
    return 1;
}

/* 绑定目标帧缓冲并设置视口（保留模式绑 FBO，否则绑默认） */
static void begin_common(QLGfxGL *g, ql_color_t clear_color, int bind_fbo)
{
    (void)surface_sync(g);
    sync_window_background(g, clear_color);

    /*
     * 拖动期间关了 vsync（见 ql_gfx_gl_end），这里按**显示节奏**补上等待。
     *
     * 例外：刚收到 ConfigureNotify 的那一帧（`present_now`）不睡 —— 窗口
     * 已经被拉大了，新露出来的那块还等着我们填。这一帧立刻画、立刻推上去，
     * 空白/黑边就只存在"渲染 + swap"这一小段（亚毫秒）；若按节奏先睡 7ms，
     * 就会发现"缩放时闪一下空白"，而且帧越等越旧（跟手变差）。
     * 下一帧（没有新事件时）照旧按 pace_us 补齐，所以帧率仍然是稳的。
     *
     * 放在 begin 且 t0 在睡之前抓：`frame_us` 是 begin→end，等待算进去之后
     * "--stats 的帧时间 / fps" 与跟 vsync 时口径一致（都是真实节奏）。
     * 若放在 end 之后才睡，fps 会虚高到几百上千，看起来像坏了。
     */
    now_us(&g->t0);
    if (!g->offscreen && g->vsync_eff == 0 && g->vsync_user && g->last_end_us != 0 &&
        !g->present_now) {
        const uint64_t due = g->last_end_us + (uint64_t)g->pace_us;
        while (now_us_mono() < due)
            usleep(200);
    }
    g->present_now = 0;
    g->vert_count = 0;
    g->vertex_count = 0;
    g->draw_calls = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, bind_fbo ? g->fbo : 0);
    glViewport(0, 0, (GLsizei)g->width, (GLsizei)g->height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((GLfloat)QL_COLOR_GET_R(clear_color) / 255.0f,
                 (GLfloat)QL_COLOR_GET_G(clear_color) / 255.0f,
                 (GLfloat)QL_COLOR_GET_B(clear_color) / 255.0f, 1.0f);

    g->clip = ql_rect_make(0, 0, g->width, g->height);
    g->clip_active = 0;
    g->mask_on = 0;
    g->mask_rx = -1.0f;
    g->mask_ry = -1.0f;
    g->poly_n = 0;
    g->cm_on = 0;
    g->draw.mask_r = -1;
    g->draw.mask_ry = -1;
    g->draw.mask_pn = 0;
    g->draw.elem_x_fx = QL_SCALE_ONE;
    g->ime_x = -1;
    g->ime_y = -1;
    g->ime_h = -1;
    g->draw.elem_y_fx = QL_SCALE_ONE;
}

int ql_gfx_gl_begin_rects(QLGfxGL *g, ql_color_t clear_color, const QLRect *rects,
                          int count)
{
    if (g == NULL)
        return 1;

    const int retained = fbo_ensure(g);
    begin_common(g, clear_color, retained);

    if (!retained || rects == NULL || count <= 0 || g->fbo_fresh) {
        /*
         * 整屏清的情形：没开保留模式 / 没有脏区 / FBO 刚建（内容未定义，
         * 调用方这一帧必须整屏重画，不能只画脏区）。
         */
        g->fbo_fresh = 0;
        glClear(GL_COLOR_BUFFER_BIT);
        return 1;
    }

    /*
     * 只清脏区。rects 与 ql_ui_consume_dirty 的输出同单位（CSS px），
     * 这里要乘 scale 换成设备像素——与 gl_set_clip 的做法一致。
     */
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < count; i++) {
        QLRect r = gsx(g, rects[i]);
        r = ql_rect_intersect(r, ql_rect_make(0, 0, g->width, g->height));
        if (ql_rect_is_empty(r))
            continue;
        int x = (int)r.x;
        int y = (int)(g->height - ql_rect_bottom(r));
        glScissor(x, y, (GLsizei)r.w, (GLsizei)r.h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    return 0;
}

void ql_gfx_gl_begin(QLGfxGL *g, ql_color_t clear_color)
{
    if (g == NULL)
        return;

    const int retained = fbo_ensure(g);
    begin_common(g, clear_color, retained);
    glClear(GL_COLOR_BUFFER_BIT);
    /* 整屏清过了，FBO 内容就是这一帧的完整结果，"刚建"不再成立 */
    if (retained)
        g->fbo_fresh = 0;
}



/*
 * 提交本批次累积的顶点。
 *
 * 正常情况下整帧只有一次（单批次是性能设计的核心）；但**毛玻璃**必须中途提交一次：
 * 它要读"已经画好的内容"，所以调用方先 flush，再模糊，再继续往缓冲里摞顶点。
 */
static void flush_batch(QLGfxGL *g)
{
    if (g->vert_count > 0) {
        glUseProgram(g->program);
        glUniform2f(g->u_viewport, (GLfloat)g->width, (GLfloat)g->height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
        glUniform1i(g->u_atlas, 0);

        /*
         * u_bg 是毛玻璃的结果（mode 4 才真的采样）。没有毛玻璃时绑 1x1 占位纹理：
         * 采样器**必须**绑东西，否则采样结果是未定义值。
         */
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (g->blur_tex[0] != 0) ? g->blur_tex[0]
                                                          : g->bg_dummy_tex);
        glUniform1i(g->u_bg, 1);

        /*
         * u_image 同理：采样器必须绑东西。没有图片的批次绑 1x1 占位纹理，
         * 免得采样结果是未定义值。
         */
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (g->cur_img_tex != 0) ? g->cur_img_tex
                                                           : g->bg_dummy_tex);
        glUniform1i(g->u_image, 2);
        glActiveTexture(GL_TEXTURE0);

        /* 颜色矩阵：每批次上传一次（只在 cm 变化时才会 flush，见 gl_set_cm） */
        if (g->u_cm >= 0) {
            GLfloat cmf[12];
            for (int i = 0; i < 12; i++) {
                /* 线性部分是 16.16 无量纲；偏移是"色值 * 65536"（还要 /255 归一到 0..1） */
                cmf[i] = (i % 4 == 3) ? (GLfloat)g->cm[i] / 65536.0f / 255.0f
                                      : (GLfloat)g->cm[i] / 65536.0f;
            }
            glUniform4fv(g->u_cm, 3, cmf);
            glUniform1i(g->u_cm_on, g->cm_on);
        }
        if (g->u_pn >= 0) {
            glUniform1i(g->u_pn, g->poly_n);
            if (g->poly_n >= 3)
                glUniform2fv(g->u_p, g->poly_n, g->poly_xy);
        }

        glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(g->vert_count * sizeof(QLGLVertex)),
                     g->verts, GL_STREAM_DRAW);

        const GLsizei stride = (GLsizei)sizeof(QLGLVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(QLGLVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(QLGLVertex, lx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(QLGLVertex, hx));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                              (void *)offsetof(QLGLVertex, radius));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(QLGLVertex, u));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(QLGLVertex, r));
        /*
         * 圆遮罩属性必须真的绑上：不绑的话它的默认值是 (0,0,0,1)，
         * 片元里"mr >= 0 就启用遮罩"会把半径当成 0 → 整屏被裁掉（全黑）。
         */
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride,
                              (void *)offsetof(QLGLVertex, mcx));
        /* 四角圆角半径也必须绑：没绑时默认 (0,0,0,0) ⇒ 所有圆角归零 */
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride,
                              (void *)offsetof(QLGLVertex, r_tl));

        glEnable(GL_SCISSOR_TEST);      /* 顶点已按最终 clip 画好，这里只需要全屏 scope */
        glScissor(0, 0, (GLsizei)g->width, (GLsizei)g->height);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g->vert_count);

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);
        glDisableVertexAttribArray(4);
        glDisableVertexAttribArray(5);
        glDisableVertexAttribArray(6);
        glDisableVertexAttribArray(7);

        g->draw_calls++;
        g->vertex_count = g->vert_count;
        g->vert_count = 0;
    }
}

void ql_gfx_gl_end(QLGfxGL *g)
{
    if (g == NULL)
        return;

    flush_batch(g);

    /* 保留模式：FBO 是真正的画布，整块 blit 到窗口再交换 */
    if (g->fbo_valid) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, (GLint)g->width, (GLint)g->height,
                          0, 0, (GLint)g->width, (GLint)g->height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /*
     * 拖动边框时**不跟 vblank**。
     *
     * 原因：合成器在连续重配窗口时不会及时送 frame callback，
     * Mesa 的 Present/XCB 路径会一直等（实测一帧卡 1000ms，肉眼就是"卡死一下"）。
     * 不跟 vsync 的话 swap 立刻返回，界面始终跟手；拖动结束后 200ms 自动恢复。
     * 这也是 GTK/Qt 在互动缩放期间的常见做法。
     */
    if (g->egdpy != EGL_NO_DISPLAY && !g->offscreen) {
        const uint32_t now32 = (uint32_t)(now_us_mono() / 1000);
        const int dragging = (int32_t)(now32 - g->resize_until_ms) < 0;
        const int want = (g->vsync_user && (!dragging || g->keep_vsync_on_resize)) ? 1 : 0;
        if (want != g->vsync_eff) {
            g->vsync_eff = want;
            eglSwapInterval(g->egdpy, want);
        }
    }

    struct timespec ts0, ts1;
    now_us(&ts0);
    eglSwapBuffers(g->egdpy, g->surface);
    now_us(&ts1);
    g->swap_us = (uint32_t)((ts1.tv_sec - ts0.tv_sec) * 1000000 +
                            (ts1.tv_nsec - ts0.tv_nsec) / 1000);

    if (g->lag_debug && g->cfg_us != 0) {
        fprintf(stderr, "[lag] cfg_to_present=%.3fms eff=%d\n",
                (double)(now_us_mono() - g->cfg_us) / 1000.0, g->vsync_eff);
        g->cfg_us = 0;
    }

#ifdef QLGL_HAVE_XSYNC
    /*
     * 回执放在 swap 之后：此时这一帧已经推上屏，WM 可以放心走下一步缩放。
     * 先 XSync 把我们这一帧的请求处理完，再设计数器。
     */
    if (g->sync_pending) {
        g->sync_pending = 0;
        XSync(g->dpy, False);
        XSyncSetCounter(g->dpy, g->sync_counter, g->sync_value);
    }
#endif

    struct timespec t1;
    now_us(&t1);
    g->frame_us = (uint32_t)((t1.tv_sec - g->t0.tv_sec) * 1000000 +
                             (t1.tv_nsec - g->t0.tv_nsec) / 1000);
    g->frame_count++;

    if (g->lag_debug) {
        const uint64_t n2 = now_us_mono();
        fprintf(stderr, "[frame] dt=%.2f work=%.2f swap=%.2f eff=%d\n",
                g->last_end_us ? (double)(n2 - g->last_end_us) / 1000.0 : 0.0,
                (double)g->frame_us / 1000.0 - (double)g->swap_us / 1000.0,
                (double)g->swap_us / 1000.0, g->vsync_eff);
    }

    /*
     * 帧率节流：
     *   - 跟 vsync 时，把实测间隔记进 pace_us（滑动平均，给下面用）；
     *   - 拖动期间关了 vsync，下一帧开头（begin_common）按 pace_us 补齐，
     *     否则"不跟 vblank"会让 fps 直接飙到上万（看起来像坏了，也白烧 CPU）。
     */
    if (!g->offscreen) {
        const uint64_t now = now_us_mono();
        if (g->vsync_eff == 1) {
            if (g->last_end_us != 0) {
                const uint64_t dt = now - g->last_end_us;
                if (dt > 1000 && dt < 100000)
                    g->pace_us = (uint32_t)((g->pace_us * 7u + (uint32_t)dt) / 8u);
            }
        }
        g->last_end_us = now;
    }
}

int ql_gfx_gl_wait(QLGfxGL *g, int timeout_ms)
{
    if (g == NULL || g->closed)
        return 0;
    if (g->offscreen || g->dpy == NULL) {
        /* 离屏模式没有事件源：睡一会儿就当"等到了" */
        if (timeout_ms > 0)
            usleep((useconds_t)timeout_ms * 1000u);
        return 1;
    }
#ifdef QLGL_HAVE_XSYNC
    /*
     * 窗口管理器在等我们的回执，但这一轮没有要画的东西（没有脏区、没有改尺寸）：
     * 直接回执"内容就是现在这样"，别让它干等。移动窗口（尺寸没变）会走这条路。
     */
    if (g->sync_pending) {
        g->sync_pending = 0;
        XSync(g->dpy, False);
        XSyncSetCounter(g->dpy, g->sync_counter, g->sync_value);
    }
#endif

    /*
     * 先看 Xlib 自己的队列：XPending 已经把数据读进缓冲时，socket 上可能
     * 一个字节都没有，只 poll 会白等到超时（表现为"偶尔卡 32ms 再补帧"）。
     */
    if (XPending(g->dpy) > 0)
        return 1;

    struct pollfd pfd;
    pfd.fd = ConnectionNumber(g->dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;
    (void)poll(&pfd, 1, timeout_ms);
    return g->closed ? 0 : 1;
}

int ql_gfx_gl_poll(QLGfxGL *g)
{
    if (g == NULL || g->closed || g->offscreen || g->dpy == NULL)
        return (g != NULL && !g->closed) ? 1 : 0;

    while (XPending(g->dpy) > 0) {
        XEvent ev;
        XNextEvent(g->dpy, &ev);

        /*
         * 输入法必须**先过一遍**：正在组字时（拼音还没选词）按键要交给 IME，
         * 不能让界面层看到。返回 True 表示这条事件被吃掉。
         */
        {
            const Bool im_took = (g->xic != NULL) ? XFilterEvent(&ev, None) : False;
            /*
             * 输入法排障：`QLGL_TRACE_IM=1` 时把"哪些事件被输入法吃掉"打出来。
             * 打不出中文时先看这里 —— 按键全都没被吃掉说明 IC 没拿到焦点
             * （或者根本没连上输入法），与界面层无关。
             */
            if (g->trace_im && (ev.type == KeyPress || ev.type == KeyRelease))
                fprintf(stderr, "[IM] type=%d filtered=%d\n", ev.type, (int)im_took);
            if (im_took)
                continue;
        }

        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == g->wm_delete) {
                g->closed = 1;
            }
#ifdef QLGL_HAVE_XSYNC
            else if (ev.xclient.message_type == g->wm_protocols &&
                     (Atom)ev.xclient.data.l[0] == g->net_wm_sync_request) {
                /* WM：画完这一帧（新尺寸）之后把计数器设成这个值 */
                XSyncIntsToValue(&g->sync_value, (unsigned int)ev.xclient.data.l[2],
                                 (int)ev.xclient.data.l[3]);
                g->sync_pending = 1;
            }
#endif
            break;
        case DestroyNotify:
            g->closed = 1;
            break;
        case ConfigureNotify: {
            /*
             * 窗口被拖动缩放。这里只更新"画布多大"，
             * 真正的重排交给调用方（它知道界面该按哪个逻辑尺寸布局）。
             */
            const int nw = ev.xconfigure.width;
            const int nh = ev.xconfigure.height;
            if (nw > 0 && nh > 0 && (nw != g->width || nh != g->height)) {
                g->width = (ql_coord_t)nw;
                g->height = (ql_coord_t)nh;
                g->fbo_fresh = 1;      /* 保留模式的旧内容已经作废 */
                g->ime_x = -1;         /* 窗口挪了/改尺寸了：光标在根窗口的位置也变了 */
                g->resize_pending = 1;
                /* 未来 200ms 内不跟 vblank（见 ql_gfx_gl_end 里的说明） */
                g->resize_until_ms = (uint32_t)(now_us_mono() / 1000) + 200u;
                g->present_now = 1;
                g->cfg_us = now_us_mono();
            }
            break;
        }
        case FocusIn:
            if (g->xic != NULL)
                XSetICFocus(g->xic);
            break;
        case FocusOut:
            if (g->xic != NULL)
                XUnsetICFocus(g->xic);
            break;
        default:
            if (ql_x11_clip_handle_event(&ev))
                break;                 /* 剪贴板协议事件 */
            ql_x11_push_input_scaled(&ev, &g->buttons, g->scale_fx, g->xic);
            break;
        }
    }
    return g->closed ? 0 : 1;
}

/* --- 剪贴板：把 X11 实现接到界面层（QLUIClipboard 接口） --- */
static void gl_clip_set(const char *utf8, void *user)
{
    (void)user;
    ql_x11_clip_set_text(utf8);
}

static int gl_clip_get(char *buf, size_t cap, void *user)
{
    (void)user;
    return ql_x11_clip_get_text(buf, cap);
}

static const QLUIClipboard s_gl_clip = { gl_clip_set, gl_clip_get };

void ql_gfx_gl_attach_clipboard(QLGfxGL *g, QLUIRoot *ui)
{
    if (g == NULL || g->dpy == NULL || g->win == 0 || ui == NULL)
        return;
    ql_x11_clip_attach(g->dpy, g->win);
    ql_ui_set_clipboard(ui, &s_gl_clip, NULL);
}

void ql_gfx_gl_set_ime_spot(QLGfxGL *g, ql_coord_t x, ql_coord_t y, ql_coord_t h)
{
    if (g == NULL || g->xic == NULL || g->dpy == NULL)
        return;
    if (x == g->ime_x && y == g->ime_y && h == g->ime_h)
        return;                     /* 没动就不发 X 请求 */
    g->ime_x = x;
    g->ime_y = y;
    g->ime_h = h;

    /*
     * XIM 的 spot 用**根窗口坐标**，而且是光标的**左下角**（baseline 位置）。
     * 我们的坐标是"视口内的 CSS px"，所以要：乘 DPR → 平移到根窗口 → y 加高度。
     */
    XPoint spot;
    spot.x = (short)gsl(g, x);
    spot.y = (short)gsl(g, y + h);
    {
        Window child = None;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(g->dpy, g->win, DefaultRootWindow(g->dpy),
                                  (int)gsl(g, x), (int)gsl(g, y + h), &rx, &ry, &child)) {
            spot.x = (short)rx;
            spot.y = (short)ry;
        }
    }
    /*
     * 送两遍：
     *   1. **preedit attributes 里的 XNSpotLocation** —— GTK/Qt 都是这么送的，
     *      ibus 的候选窗位置就是从这条路径上取的；
     *   2. 顶层 `XNSpotLocation` —— `XIMPreeditNothing` 风格下有些实现只认这条。
     * 只送一条时踩过坑：候选窗出现在窗口下方/屏幕外，等于输入法根本不知道
     * 光标在哪。
     */
    {
        char *bad = NULL;
        XVaNestedList preedit = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
        if (preedit != NULL) {
            bad = XSetICValues(g->xic, XNPreeditAttributes, preedit, NULL);
            XFree(preedit);
        }
        if (bad != NULL) {
            /* 这个输入风格没有 preedit attributes：释放并退回顶层那条 */
            XFree(bad);
        }
    }
    {
        char *bad = XSetICValues(g->xic, XNSpotLocation, &spot, NULL);
        if (bad != NULL)
            XFree(bad);
        if (g->trace_im)
            fprintf(stderr, "[IM] spot root=(%d,%d)\n", spot.x, spot.y);
    }
}

int ql_gfx_gl_resize_pending(QLGfxGL *g)
{
    if (g == NULL)
        return 0;
    const int p = g->resize_pending;
    g->resize_pending = 0;
    return p;
}

void ql_gfx_gl_size(const QLGfxGL *g, ql_coord_t *out_w, ql_coord_t *out_h)
{
    ql_coord_t w = 0, h = 0;
    if (g != NULL)
    {
        w = (g->scale_fx > 0 && g->scale_fx != QL_SCALE_ONE)
                ? ql_unscale_len(g->width, g->scale_fx)
                : g->width;
        h = (g->scale_fx > 0 && g->scale_fx != QL_SCALE_ONE)
                ? ql_unscale_len(g->height, g->scale_fx)
                : g->height;
    }
    if (out_w != NULL)
        *out_w = w;
    if (out_h != NULL)
        *out_h = h;
}

void ql_gfx_gl_close(QLGfxGL *g)
{
    if (g != NULL)
        g->closed = 1;
}

void ql_gfx_gl_destroy(QLGfxGL *g)
{
    if (g == NULL)
        return;

    ql_x11_clip_detach();
    if (g->xic != NULL) {
        XDestroyIC(g->xic);
        g->xic = NULL;
    }
    if (g->xim != NULL) {
        XCloseIM(g->xim);
        g->xim = NULL;
    }

    if (g->egdpy != EGL_NO_DISPLAY && g->egdpy != NULL) {
        eglMakeCurrent(g->egdpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->atlas_tex)
            glDeleteTextures(1, &g->atlas_tex);
        if (g->vbo)
            glDeleteBuffers(1, &g->vbo);
        if (g->blur_vbo)
            glDeleteBuffers(1, &g->blur_vbo);
        for (int i = 0; i < 2; i++) {
            if (g->blur_fbo[i])
                glDeleteFramebuffers(1, &g->blur_fbo[i]);
            if (g->blur_tex[i])
                glDeleteTextures(1, &g->blur_tex[i]);
        }
        for (int i = 0; i < g->img_count; i++)
            if (g->img_tex[i])
                glDeleteTextures(1, &g->img_tex[i]);
        if (g->bg_dummy_tex)
            glDeleteTextures(1, &g->bg_dummy_tex);
        if (g->blur_prog)
            glDeleteProgram(g->blur_prog);
#if QL_FEAT_CSS_FILTER_LAYER
        if (g->layer_vbo)
            glDeleteBuffers(1, &g->layer_vbo);
        for (int i = 0; i < QL_CFG_UI_LAYER_MAX_DEPTH; i++) {
            if (g->layer[i].fbo)
                glDeleteFramebuffers(1, &g->layer[i].fbo);
            if (g->layer[i].tex)
                glDeleteTextures(1, &g->layer[i].tex);
            if (g->layer[i].mask_tex)
                glDeleteTextures(1, &g->layer[i].mask_tex);
        }
        if (g->layer_bd_fbo)
            glDeleteFramebuffers(1, &g->layer_bd_fbo);
        if (g->layer_bd_tex)
            glDeleteTextures(1, &g->layer_bd_tex);
        if (g->layer_prog)
            glDeleteProgram(g->layer_prog);
        if (g->blur_rgba_prog)
            glDeleteProgram(g->blur_rgba_prog);
#endif
        if (g->program)
            glDeleteProgram(g->program);
        if (g->ctx != EGL_NO_CONTEXT)
            eglDestroyContext(g->egdpy, g->ctx);
        if (g->surface != EGL_NO_SURFACE)
            eglDestroySurface(g->egdpy, g->surface);
        eglTerminate(g->egdpy);
    }
    if (g->dpy != NULL) {
#ifdef QLGL_HAVE_XSYNC
        if (g->sync_counter != None)
            XSyncDestroyCounter(g->dpy, g->sync_counter);
#endif
        if (g->win != 0)
            XDestroyWindow(g->dpy, g->win);
        XCloseDisplay(g->dpy);
    }
    ql_mem_free(g->verts);
    ql_mem_free(g->uv);
    ql_mem_free(g);
}

uint32_t ql_gfx_gl_frame_count(QLGfxGL *g)  { return g ? g->frame_count : 0; }
uint32_t ql_gfx_gl_draw_calls(QLGfxGL *g)   { return g ? g->draw_calls : 0; }
uint32_t ql_gfx_gl_vertex_count(QLGfxGL *g) { return g ? g->vertex_count : 0; }
uint32_t ql_gfx_gl_frame_us(QLGfxGL *g)     { return g ? g->frame_us : 0; }
uint32_t ql_gfx_gl_swap_us(QLGfxGL *g)      { return g ? g->swap_us : 0; }
double   ql_gfx_gl_gpu_mhz(QLGfxGL *g)      { QL_UNUSED(g); return 0; }

ql_err_t ql_gfx_gl_read_pixels(QLGfxGL *g, uint8_t *rgba, uint32_t stride_bytes)
{
    if (g == NULL || rgba == NULL)
        return QL_ERR_INVAL;

    uint32_t row = (uint32_t)g->width * 4u;
    if (stride_bytes < row)
        return QL_ERR_INVAL;

    uint8_t *tmp = (uint8_t *)ql_mem_calloc(1, (size_t)row * (size_t)g->height);
    if (tmp == NULL)
        return QL_ERR_NOMEM;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)g->width, (GLsizei)g->height,
                 GL_RGBA, GL_UNSIGNED_BYTE, tmp);

    /* GL 原点在左下，翻转成自上而下 */
    for (ql_coord_t y = 0; y < g->height; y++) {
        const uint8_t *src = tmp + (size_t)(g->height - 1 - y) * row;
        memcpy(rgba + (size_t)y * stride_bytes, src, row);
    }

    ql_mem_free(tmp);
    return QL_OK;
}

#endif /* QLGL_ENABLE_GL */
