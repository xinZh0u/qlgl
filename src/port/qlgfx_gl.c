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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QL_GL_MAX_VERTS     65536
#define QL_GL_ATLAS_SIZE    512
#define QL_GL_FONT_SLOTS    2

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
} QLGLVertex;

typedef struct {
    float u0, v0, u1, v1;
    uint8_t valid;
} QLGLUV;

struct QLGfxGL {
    Display         *dpy;
    Window           win;
    Atom             wm_delete;
    EGLDisplay       egdpy;
    EGLSurface       surface;
    EGLContext       ctx;
    int              offscreen;
    int              closed;

    ql_coord_t       width, height;

    GLuint           program;
    GLuint           vbo;
    GLint            u_viewport;
    GLint            u_atlas;
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

    QLGLUV           uv[QL_GL_FONT_SLOTS][256];   /* 每字体最多 256 字形 */
    uint16_t         uv_count[QL_GL_FONT_SLOTS];
};

/* ------------------------------ 着色器 ------------------------------ */

static const char *VS_SRC =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_local;\n"
    "layout(location=2) in vec2 a_half;\n"
    "layout(location=3) in vec4 a_params;\n"   /* radius, border, use_tex, - */
    "layout(location=4) in vec2 a_uv;\n"
    "layout(location=5) in vec4 a_color;\n"
    "uniform vec2 u_viewport;\n"
    "out vec2 v_local; out vec2 v_half; out vec4 v_params; out vec2 v_uv; out vec4 v_color;\n"
    "void main(){\n"
    "  vec2 ndc = (a_pos / u_viewport) * 2.0 - 1.0;\n"
    "  ndc.y = -ndc.y;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_local = a_local; v_half = a_half; v_params = a_params;\n"
    "  v_uv = a_uv; v_color = a_color;\n"
    "}\n";

static const char *FS_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_local; in vec2 v_half; in vec4 v_params; in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 frag;\n"
    "float sdRoundBox(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
    "}\n"
    "void main(){\n"
    "  float radius = v_params.x;\n"
    "  float border = v_params.y;\n"
    "  float use_tex = v_params.z;\n"
    "  float cov;\n"
    "  if (use_tex > 0.5) {\n"
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
    "  frag = vec4(v_color.rgb, v_color.a * cov);\n"
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
    return 1;
}

/* ------------------------------ 字形图集 ------------------------------ */

static void atlas_upload(QLGfxGL *g, const uint8_t *data)
{
    glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE, 0,
                 GL_RED, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static int font_slot(const QLFont *font)
{
    if (font == &ql_font_mono_12) return 0;
    if (font == &ql_font_mono_16) return 1;
    return -1;
}

static int build_atlas(QLGfxGL *g)
{
    uint8_t *pixels = (uint8_t *)ql_mem_calloc(1, QL_GL_ATLAS_SIZE * QL_GL_ATLAS_SIZE);
    if (pixels == NULL)
        return 0;

    static const QLFont *const fonts[QL_GL_FONT_SLOTS] = { &ql_font_mono_12, &ql_font_mono_16 };

    int pen_x = 1, pen_y = 1, row_h = 0;

    for (int slot = 0; slot < QL_GL_FONT_SLOTS; slot++) {
        const QLFont *font = fonts[slot];
        g->uv_count[slot] = font->glyph_count;

        for (uint16_t i = 0; i < font->glyph_count && i < 256; i++) {
            const QLGlyph *glyph = &font->glyphs[i];
            if (glyph->box_w == 0 || glyph->box_h == 0) {
                g->uv[slot][i].valid = 0;
                continue;
            }
            if (pen_x + glyph->box_w + 1 > QL_GL_ATLAS_SIZE) {
                pen_x = 1;
                pen_y += row_h + 1;
                row_h = 0;
            }
            if (pen_y + glyph->box_h + 1 > QL_GL_ATLAS_SIZE) {
                QL_LOGE("gl: glyph atlas full");
                ql_mem_free(pixels);
                return 0;
            }

            for (uint32_t gy = 0; gy < glyph->box_h; gy++) {
                for (uint32_t gx = 0; gx < glyph->box_w; gx++) {
                    uint8_t a = ql_font_glyph_alpha(font, glyph, (int32_t)gx, (int32_t)gy);
                    pixels[(size_t)(pen_y + (int)gy) * QL_GL_ATLAS_SIZE +
                           (size_t)(pen_x + (int)gx)] = a;
                }
            }

            float inv = 1.0f / (float)QL_GL_ATLAS_SIZE;
            g->uv[slot][i].u0 = (float)pen_x * inv;
            g->uv[slot][i].v0 = (float)pen_y * inv;
            g->uv[slot][i].u1 = (float)(pen_x + glyph->box_w) * inv;
            g->uv[slot][i].v1 = (float)(pen_y + glyph->box_h) * inv;
            g->uv[slot][i].valid = 1;

            pen_x += (int)glyph->box_w + 1;
            if ((int)glyph->box_h > row_h)
                row_h = (int)glyph->box_h;
        }
    }

    atlas_upload(g, pixels);

    if (getenv("QLGL_GL_DUMP_ATLAS") != NULL) {
        FILE *f = fopen("/tmp/qlgl_atlas.pgm", "wb");
        if (f != NULL) {
            fprintf(f, "P5\n%d %d\n255\n", QL_GL_ATLAS_SIZE, QL_GL_ATLAS_SIZE);
            fwrite(pixels, 1, (size_t)QL_GL_ATLAS_SIZE * QL_GL_ATLAS_SIZE, f);
            fclose(f);
            QL_LOGI("gl: atlas dumped to /tmp/qlgl_atlas.pgm");
        }
    }

    ql_mem_free(pixels);
    return 1;
}

/* ------------------------------ 顶点发射 ------------------------------ */

static void emit_vertex(QLGfxGL *g, const QLGLVertex *v)
{
    if (g->vert_count >= g->vert_capacity)
        return;      /* 溢出就丢：宁可少画不可崩 */
    g->verts[g->vert_count++] = *v;
}

static void emit_quad(QLGfxGL *g, QLRect r, ql_coord_t radius, ql_coord_t border,
                      ql_color_t color, uint8_t alpha,
                      float u0, float v0, float u1, float v1, int use_tex)
{
    if (ql_rect_is_empty(r) || alpha == 0)
        return;

    /* 外扩 1px 给抗锯齿留余量 */
    float x0 = (float)r.x - 1.0f;
    float y0 = (float)r.y - 1.0f;
    float x1 = (float)ql_rect_right(r) + 1.0f;
    float y1 = (float)ql_rect_bottom(r) + 1.0f;

    float cx = (float)r.x + (float)r.w * 0.5f;
    float cy = (float)r.y + (float)r.h * 0.5f;
    float hx = (float)r.w * 0.5f;
    float hy = (float)r.h * 0.5f;

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

    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    const float us[4] = { u0, u1, u1, u0 };
    const float vs[4] = { v0, v0, v1, v1 };

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
        v.u = us[k];
        v.v = vs[k];
        emit_vertex(g, &v);
    }
}

/* ------------------------------ QLDraw2D 实现 ------------------------------ */

static void gl_set_clip(QLDraw2D *self, QLRect clip)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    g->clip = ql_rect_intersect(clip, ql_rect_make(0, 0, g->width, g->height));
    g->clip_active = 1;

    /* 立即应用 scissor（GL 原点在左下，y 要翻转） */
    int x = (int)g->clip.x;
    int y = (int)(g->height - ql_rect_bottom(g->clip));
    glScissor(x, y, (GLsizei)g->clip.w, (GLsizei)g->clip.h);
}

static void gl_fill_rect(QLDraw2D *self, QLRect rect, ql_color_t color, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    emit_quad(g, rect, 0, -1, color, alpha, 0, 0, 0, 0, 0);
}

static void gl_fill_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                               ql_color_t color, uint8_t alpha)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    emit_quad(g, rect, radius, -1, color, alpha, 0, 0, 0, 0, 0);
}

static void gl_stroke_round_rect(QLDraw2D *self, QLRect rect, ql_coord_t radius,
                                 ql_coord_t thickness, ql_color_t color)
{
    QLGfxGL *g = (QLGfxGL *)self->user;
    if (thickness <= 0)
        return;
    emit_quad(g, rect, radius, thickness, color, 255, 0, 0, 0, 0, 0);
}

static void gl_emit_glyph(QLDraw2D *self, const QLFont *font, const QLGlyph *glyph,
                          ql_coord_t pen_x, ql_coord_t baseline_y, ql_color_t color)
{
    QLGfxGL *g = (QLGfxGL *)self->user;

    int slot = font_slot(font);
    if (slot < 0)
        return;
    uint32_t index = (uint32_t)(glyph - font->glyphs);
    if (index >= 256 || !g->uv[slot][index].valid)
        return;

    QLGLUV uv = g->uv[slot][index];
    QLRect r = ql_rect_make(pen_x + glyph->ofs_x, baseline_y + glyph->ofs_y,
                            glyph->box_w, glyph->box_h);
    emit_quad(g, r, 0, -1, color, 255, uv.u0, uv.v0, uv.u1, uv.v1, 1);
}

static void gl_draw_text(QLDraw2D *self, const QLFont *font, const char *text, int32_t len,
                         QLRect box, ql_color_t color, QLTextAlign align)
{
    ql_draw2d_text_layout(self, font, text, len, box, color, align, gl_emit_glyph);
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
    g->offscreen = (flags & QL_GFX_GL_OFFSCREEN) ? 1 : 0;
    g->vert_capacity = QL_GL_MAX_VERTS;
    g->verts = (QLGLVertex *)ql_mem_calloc(QL_GL_MAX_VERTS, sizeof(QLGLVertex));
    if (g->verts == NULL) {
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
        swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
        g->win = XCreateWindow(g->dpy, RootWindow(g->dpy, screen), 0, 0,
                               (unsigned)width, (unsigned)height, 0, vinfo.depth,
                               InputOutput, vinfo.visual,
                               CWBorderPixel | CWColormap | CWEventMask, &swa);
        if (g->win == 0) {
            QL_LOGE("gl: XCreateWindow failed");
            goto fail;
        }
        XStoreName(g->dpy, g->win, (title != NULL) ? title : "QLGL GL");
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
    if (!build_atlas(g))
        goto fail;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);

    g->clip = ql_rect_make(0, 0, width, height);

    g->draw.set_clip = gl_set_clip;
    g->draw.fill_rect = gl_fill_rect;
    g->draw.fill_round_rect = gl_fill_round_rect;
    g->draw.stroke_round_rect = gl_stroke_round_rect;
    g->draw.draw_text = gl_draw_text;
    g->draw.flush = NULL;
    g->draw.user = g;

    QL_LOGI("gl: ready %dx%d%s", (int)width, (int)height, g->offscreen ? " (offscreen)" : "");
    return g;

fail:
    ql_gfx_gl_destroy(g);
    return NULL;
}

QLDraw2D *ql_gfx_gl_draw(QLGfxGL *gfx)
{
    return (gfx != NULL) ? &gfx->draw : NULL;
}

void ql_gfx_gl_set_vsync(QLGfxGL *gfx, int on)
{
    if (gfx == NULL || gfx->egdpy == EGL_NO_DISPLAY)
        return;
    eglSwapInterval(gfx->egdpy, on ? 1 : 0);
}

void ql_gfx_gl_begin(QLGfxGL *g, ql_color_t clear_color)
{
    if (g == NULL)
        return;

    now_us(&g->t0);
    g->vert_count = 0;
    g->vertex_count = 0;
    g->draw_calls = 0;

    glViewport(0, 0, (GLsizei)g->width, (GLsizei)g->height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((GLfloat)QL_COLOR_GET_R(clear_color) / 255.0f,
                 (GLfloat)QL_COLOR_GET_G(clear_color) / 255.0f,
                 (GLfloat)QL_COLOR_GET_B(clear_color) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    g->clip = ql_rect_make(0, 0, g->width, g->height);
    g->clip_active = 0;
}

void ql_gfx_gl_end(QLGfxGL *g)
{
    if (g == NULL)
        return;

    if (g->vert_count > 0) {
        glUseProgram(g->program);
        glUniform2f(g->u_viewport, (GLfloat)g->width, (GLfloat)g->height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->atlas_tex);
        glUniform1i(g->u_atlas, 0);

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

        glEnable(GL_SCISSOR_TEST);      /* 顶点已按最终 clip 画好，这里只需要全屏 scope */
        glScissor(0, 0, (GLsizei)g->width, (GLsizei)g->height);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g->vert_count);

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);
        glDisableVertexAttribArray(4);
        glDisableVertexAttribArray(5);

        g->draw_calls = 1;
        g->vertex_count = g->vert_count;
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
        case KeyPress: {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            if (sym == XK_Escape || sym == XK_q)
                g->closed = 1;
            break;
        }
        case DestroyNotify:
            g->closed = 1;
            break;
        default:
            break;
        }
    }
    return g->closed ? 0 : 1;
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
