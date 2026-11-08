/**
 * @file    qlgfx_gl.c
 * @brief   OpenGL ES 3.0 / EGL 后端：SDF 圆角矩形 + 字形图集，单管线单批次。
 */
/* clock_gettime 在 -std=c11 下需要 POSIX 特性宏 */
#define _POSIX_C_SOURCE 200809L

#include <qlgl/qlgl.h>

#ifdef QLGL_ENABLE_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "qlinput_x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    EGLDisplay       egdpy;
    EGLSurface       surface;
    EGLContext       ctx;
    int              offscreen;
    int              closed;
    uint8_t          buttons;      /* 当前按住的鼠标按钮位图 */

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
    struct timespec  t0;

    /* 保留式脏区：和窗口同尺寸的 FBO，帧末 blit 到窗口 */
    GLuint           fbo;
    GLuint           fbo_tex;
    int              retained;     /* 调用方要求开启 */
    int              fbo_valid;    /* FBO 已建好并可用 */
    int              fbo_fresh;    /* 刚建/刚改尺寸：内容未定义，下一帧必须整屏画 */
    int              fbo_w, fbo_h;

    QLGLUVEntry     *uv;          /* 哈希表，容量 QL_GL_UV_SLOTS */
    uint32_t         uv_used;
    /* 图集货架分配器状态 */
    /* 圆遮罩状态（设备像素） */
    float            mask_cx, mask_cy, mask_r;
    int              mask_on;

    int              atlas_x, atlas_y, atlas_row_h;
    int              atlas_full_warned;

    /*
     * 毛玻璃（`backdrop-filter: blur()`）。
     * 半分辨率的 ping-pong：目标帧缓冲 →（blit 降采样）tex[0] →(H) fbo[1] →(V) fbo[0]，
     * 最后把 tex[0] 当成一张贴图画回元素矩形里。
     */
    GLuint           blur_prog;
    GLuint           blur_vbo;
    GLint            blur_u_tex, blur_u_texel, blur_u_step;
    GLuint           blur_tex[2];
    GLuint           blur_fbo[2];
    int              blur_w, blur_h;
    GLuint           bg_dummy_tex;    /* u_bg 的占位：没有毛玻璃时也必须绑着 */
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
    "uniform vec2 u_viewport;\n"
    "out vec2 v_local; out vec2 v_half; out vec4 v_params; out vec2 v_uv; out vec4 v_color;\n"
    "out vec4 v_mask;\n"
    "void main(){\n"
    "  vec2 ndc = (a_pos / u_viewport) * 2.0 - 1.0;\n"
    "  ndc.y = -ndc.y;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_local = a_local; v_half = a_half; v_params = a_params;\n"
    "  v_uv = a_uv; v_color = a_color; v_mask = a_mask;\n"
    "}\n";

/*
 * v_params = (radius, border_or_blur, mode, -)
 *   mode 0 = 形状（填充/描边），1 = 字形（采样图集），2 = 软阴影
 *   mode 2 时 border 存的是 blur 半径，距离衰减与 CPU 的 shadow_coverage 同一公式，
 *          保证两条后端视觉一致（golden 测试容忍 40/通道）。
 */
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
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_bg;\n"
    "uniform vec2 u_viewport;\n"
    "out vec4 frag;\n"
    "float sdRoundBox(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
    "}\n"
    "void main(){\n"
    "  float radius = v_params.x;\n"
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
    "    float md = length(fp - v_mask.xy) - v_mask.z;\n"
    "    m = 1.0 - smoothstep(-0.5, 0.5, md);\n"
    "  }\n"
    /* 形状判定必须按 mode **从大到小**排，别再写成区间夹逼（容易漏档） */
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
    "    frag = vec4(texture(u_bg, v_uv).rgb, v_color.a * cov * m);\n"
    "    return;\n"
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
    "    frag = vec4(mix(v_color.rgb, c2, t), v_color.a * cov * m);\n"
    "    return;\n"
    "  }\n"
    "  frag = vec4(v_color.rgb, v_color.a * cov * m);\n"
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
    out.mr = (g->mask_on) ? g->mask_r : -1.0f;
    out.mpad = 0.0f;
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

static void emit_quad(QLGfxGL *g, QLRect r, ql_coord_t radius, ql_coord_t border,
                      ql_color_t color, uint8_t alpha,
                      float u0, float v0, float u1, float v1, int use_tex)
{
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

    /* 绘制期平移：形状吃全偏移（亚像素），字形只吃整数部分（保持清晰） */
    const float ox = (use_tex == 1) ? round_to_int(off_x_of(g)) : off_x_of(g);
    const float oy = (use_tex == 1) ? round_to_int(off_y_of(g)) : off_y_of(g);
    x0 += ox; y0 += oy; x1 += ox; y1 += oy;
    cx += ox; cy += oy;

    const float xs[4] = { x0, x1, x1, x0 };
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
    return (g->draw.elem_x_fx < g->draw.elem_y_fx) ? g->draw.elem_x_fx
                                                   : g->draw.elem_y_fx;
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
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  const float w[5] = float[5](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);\n"
    "  vec3 acc = texture(u_tex, v_uv).rgb * w[0];\n"
    "  for (int i = 1; i < 5; i++) {\n"
    "    vec2 o = u_step * float(i);\n"
    "    acc += texture(u_tex, v_uv + o).rgb * w[i];\n"
    "    acc += texture(u_tex, v_uv - o).rgb * w[i];\n"
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

/* 一趟模糊：从 tex[src] 画到 fbo[dst]，dx/dy 是方向（纹素为单位的抽头间距） */
static void blur_pass(QLGfxGL *g, int src, int dst, float dx, float dy)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->blur_fbo[dst]);
    glViewport(0, 0, g->blur_w, g->blur_h);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g->blur_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->blur_tex[src]);
    glUniform1i(g->blur_u_tex, 0);
    glUniform2f(g->blur_u_texel, 1.0f / (GLfloat)g->blur_w, 1.0f / (GLfloat)g->blur_h);
    glUniform2f(g->blur_u_step, dx, dy);

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
static void gl_backdrop_blur(QLDraw2D *self, QLRect rect, int sigma)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (g == NULL || g->blur_prog == 0)
        return;

    QLRect dev = gsx(g, elem_rect_gl(g, rect));
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
    blur_pass(g, 0, 1, span / (float)g->blur_w, 0.0f);
    blur_pass(g, 1, 0, 0.0f, span / (float)g->blur_h);

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
        v.u = (xs[k] - (float)dev.x) / (float)(dev.w > 0 ? dev.w : 1);
        v.v = 1.0f - (ys[k] - (float)dev.y) / (float)(dev.h > 0 ? dev.h : 1);
        emit_vertex(g, &v);
    }
}

static void gl_set_mask(QLDraw2D *self, ql_coord_t cx, ql_coord_t cy, ql_coord_t r)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (r < 0) {
        g->mask_on = 0;
        g->mask_r = -1.0f;
        return;
    }
    QLRect pt = gsx(g, ql_rect_make(cx, cy, 0, 0));
    g->mask_cx = (float)pt.x;
    g->mask_cy = (float)pt.y;
    g->mask_r = (float)gsl(g, r);
    g->mask_on = 1;
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
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color)
{
    QLGfxGL *g = (QLGfxGL *)self->user;

    QLGLUV uv;
    if (!ensure_glyph_uv(g, font, glyph, &uv))
        return;
    QLRect r = ql_rect_make(pen_x + glyph->ofs_x, baseline_y + glyph->ofs_y,
                            glyph->box_w, glyph->box_h);
    emit_quad(g, r, 0, -1, color, 255, uv.u0, uv.v0, uv.u1, uv.v1, 1);
}

static void gl_draw_text(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                         QLRect box, ql_color_t color, QLTextAlign align,
                         ql_coord_t line_h)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (elem_len_fx_gl(g) == 0)
        return;         /* 压到零宽：同 CPU，字一个都不画 */
    const ql_coord_t font_fx =
        (ql_coord_t)((int64_t)g->scale_fx * elem_len_fx_gl(g) / QL_SCALE_ONE);
    const QLFont *dev = ql_draw2d_device_font(font, font_fx);
    /* 排版在设备像素里进行：字形位图与 quad 是一一对应的，不能再缩放 */
    /* 行高是 CSS px；排版在设备像素里做，所以按同样的比例换算 */
    const ql_coord_t dev_line_h =
        (line_h > 0) ? (ql_coord_t)((int64_t)line_h * font_fx / QL_SCALE_ONE) : 0;
    ql_draw2d_text_layout(self, dev, text, len, gsx(g, box), color, align, dev_line_h,
                          gl_emit_glyph);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void now_us(struct timespec *t)
{
    clock_gettime(CLOCK_MONOTONIC, t);
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
    g->mask_r = -1.0f;
    g->draw.mask_r = -1;
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
        swa.event_mask = ExposureMask | StructureNotifyMask |
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
        XSetWMProtocols(g->dpy, g->win, &g->wm_delete, 1);
        XMapWindow(g->dpy, g->win);

        g->surface = eglCreateWindowSurface(g->egdpy, cfg, (EGLNativeWindowType)g->win, NULL);
    }

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
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);

    g->clip = ql_rect_make(0, 0, width, height);

    g->draw.set_clip = gl_set_clip;
    g->draw.set_mask = gl_set_mask;
    g->draw.backdrop_blur = gl_backdrop_blur;
    g->draw.fill_rect = gl_fill_rect;
    g->draw.fill_round_rect = gl_fill_round_rect;
    g->draw.fill_round_rect_gradient = gl_fill_round_rect_gradient;
    g->draw.fill_shadow = gl_fill_shadow;
    g->draw.stroke_round_rect = gl_stroke_round_rect;
    g->draw.draw_line = ql_draw2d_draw_line;
    g->draw.draw_text = gl_draw_text;
    g->draw.flush = NULL;
    g->draw.scale_fx = g->scale_fx;
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
    eglSwapInterval(gfx->egdpy, on ? 1 : 0);
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
}

/* 按当前尺寸建（或复用）FBO。窗口尺寸变了就重建 */
static int fbo_ensure(QLGfxGL *g)
{
    if (!g->retained)
        return 0;
    if (g->fbo_valid && g->fbo_w == g->width && g->fbo_h == g->height)
        return 1;

    fbo_release(g);

    glGenTextures(1, &g->fbo_tex);
    glBindTexture(GL_TEXTURE_2D, g->fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)g->width, (GLsizei)g->height, 0,
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

/* 绑定目标帧缓冲并设置视口（保留模式绑 FBO，否则绑默认） */
static void begin_common(QLGfxGL *g, ql_color_t clear_color, int bind_fbo)
{
    now_us(&g->t0);
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
    g->mask_r = -1.0f;
    g->draw.mask_r = -1;
    g->draw.elem_x_fx = QL_SCALE_ONE;
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
        glActiveTexture(GL_TEXTURE0);

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

    eglSwapBuffers(g->egdpy, g->surface);

    struct timespec t1;
    now_us(&t1);
    g->frame_us = (uint32_t)((t1.tv_sec - g->t0.tv_sec) * 1000000 +
                             (t1.tv_nsec - g->t0.tv_nsec) / 1000);
    g->frame_count++;
}

int ql_gfx_gl_poll(QLGfxGL *g)
{
    if (g == NULL || g->closed || g->offscreen || g->dpy == NULL)
        return (g != NULL && !g->closed) ? 1 : 0;

    while (XPending(g->dpy) > 0) {
        XEvent ev;
        XNextEvent(g->dpy, &ev);
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == g->wm_delete)
                g->closed = 1;
            break;
        case DestroyNotify:
            g->closed = 1;
            break;
        default:
            ql_x11_push_input_scaled(&ev, &g->buttons, g->scale_fx);
            break;
        }
    }
    return g->closed ? 0 : 1;
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
        if (g->bg_dummy_tex)
            glDeleteTextures(1, &g->bg_dummy_tex);
        if (g->blur_prog)
            glDeleteProgram(g->blur_prog);
        if (g->program)
            glDeleteProgram(g->program);
        if (g->ctx != EGL_NO_CONTEXT)
            eglDestroyContext(g->egdpy, g->ctx);
        if (g->surface != EGL_NO_SURFACE)
            eglDestroySurface(g->egdpy, g->surface);
        eglTerminate(g->egdpy);
    }
    if (g->dpy != NULL) {
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
