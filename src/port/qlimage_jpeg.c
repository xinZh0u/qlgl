/**
 * @file    qlimage_jpeg.c
 * @brief   平台后端的参考实现：libjpeg-turbo（`QL_FEAT_IMG_LIBJPEG`）。
 *
 * 为什么 JPEG 给库而不是自己写：JPEG 是一整套色彩管线（DCT 反变换、色度上采样、
 * YCbCr→RGB、量化表），手写出来的失败模式是**色偏**而不是崩溃 —— 收益/风险完全不划算。
 * 而 libjpeg-turbo 到处都有（Linux/macOS/Android 都能装），且它的 SIMD 路径比
 * 任何自研实现都快一个数量级。
 *
 * 这里只做三件事：把内存当输入流喂进去、要求输出 RGBA8888、把 libjpeg 的
 * `error_exit`（默认会 `exit()`！）改成 `longjmp`。
 *
 * 输出的 `QLImage` 会由 `qlimage.c` 的 `decode_via_backend()` 重新校验并按需要
 * 转成目标像素格式，所以这里**只负责 RGBA8888**。
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jerror.h>
#include <jpeglib.h>

#include <qlgl/qlgl.h>

/* 只有构建期找到 libjpeg 才会编译这个文件（见 CMakeLists） */
#if QL_FEAT_IMG_LIBJPEG

/* ------------------------------ 错误处理 ------------------------------ */

/*
 * libjpeg 的默认 error_exit 是 `fprintf + exit(1)` —— 对一个库来说不可接受：
 * 一张坏图会让整个进程退出。所以换成 longjmp 回到 decode()。
 */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
    char                  msg[JMSG_LENGTH_MAX];
    int                   truncated;   /* 见过 JWRN_JPEG_EOF */
} JpegErr;

static void jpeg_on_error(j_common_ptr cinfo)
{
    JpegErr *e = (JpegErr *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jb, 1);
}

/*
 * 警告默认不中断（有些合法文件会带"文件前后有多余字节"这类无害警告），
 * 但**"数据提前结束"例外**：那会解出下半张全灰的图 —— 正是本库最反对的
 * "半个正确"。所以只把它记下来，读完所有行之后按失败处理。
 */
static void jpeg_on_emit(j_common_ptr cinfo, int level)
{
    JpegErr *e = (JpegErr *)cinfo->err;
    if (level < 0) {
        ++e->pub.num_warnings;
        if (cinfo->err->msg_code == JWRN_JPEG_EOF)
            e->truncated = 1;
    }
}

/* ------------------------------ 内存源 ------------------------------ */

static void jpeg_mem_src_impl(j_decompress_ptr cinfo, const uint8_t *data, size_t len)
{
    jpeg_mem_src(cinfo, data, (unsigned long)len);
}

/* ------------------------------ 解码 ------------------------------ */

static int jpeg_decode(const uint8_t *data, size_t len, uint8_t out_format, QLImage *out,
                       char *err, size_t err_cap)
{
    (void)out_format;                  /* 只产出 RGBA8888，由调用方转换 */

    struct jpeg_decompress_struct cinfo;
    JpegErr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));

    /*
     * 像素缓冲声明在 setjmp **之前**：libjpeg 可能在扫描行中途 error_exit，
     * 那时 px 已经分配了 —— 如果它是在 setjmp 之后才声明的，longjmp 回来时
     * 它的值是不确定的（C 标准规定非 volatile 的局部变量在 longjmp 后失效）。
     */
    uint8_t *volatile px = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    jerr.pub.emit_message = jpeg_on_emit;
    jerr.msg[0] = '\0';

    if (setjmp(jerr.jb) != 0) {
        /* libjpeg 说这条路走不通：把它的文案原样带出去 */
        jpeg_destroy_decompress(&cinfo);
        if (px != NULL)
            ql_mem_free((void *)px);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "%s", jerr.msg);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src_impl(&cinfo, data, len);
    jpeg_read_header(&cinfo, TRUE);

    /* 灰度图也走 RGBA：上屏逻辑不需要知道源是几通道 */
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width == 0u || cinfo.output_height == 0u ||
        cinfo.output_width > 0xFFFFu || cinfo.output_height > 0xFFFFu) {
        jpeg_destroy_decompress(&cinfo);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "尺寸非法（%ux%u）", (unsigned)cinfo.output_width,
                     (unsigned)cinfo.output_height);
        return -1;
    }
    if (cinfo.output_components != 4) {
        jpeg_destroy_decompress(&cinfo);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "不支持的颜色空间（%d 通道）",
                     (int)cinfo.output_components);
        return -1;
    }

    const uint32_t w = cinfo.output_width;
    const uint32_t h = cinfo.output_height;
    /* 上界纪律与自有解码器一致（先除后乘） */
    if (w > QL_CFG_IMAGE_MAX_PIXELS / h) {
        jpeg_destroy_decompress(&cinfo);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "尺寸超过 QL_CFG_IMAGE_MAX_PIXELS（%ux%u）",
                     (unsigned)w, (unsigned)h);
        return -1;
    }

    const uint32_t stride = w * 4u;
    const uint64_t total = (uint64_t)stride * h;
    if (stride > 0xFFFFu || total > QL_CFG_IMAGE_MAX_BYTES * 16u) {
        jpeg_destroy_decompress(&cinfo);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "解码后字节数超过上限（%u 字节）", (unsigned)total);
        return -1;
    }

    px = (uint8_t *)ql_mem_alloc((size_t)total);
    if (px == NULL) {
        jpeg_destroy_decompress(&cinfo);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "out of memory");
        return -1;
    }

    /*
     * 一次读一行。输出缓冲是**由低到高**的行序（第 0 行在最前），而 libjpeg 的
     * scanline 缓冲是我们自己给的，所以直接把 dst 的那一行地址塞给它就行 ——
     * 不做整幅翻转、也不需要额外的一行中转。
     */
    JSAMPROW row = NULL;
    while (cinfo.output_scanline < cinfo.output_height) {
        row = px + (size_t)cinfo.output_scanline * stride;
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1)
            break;                     /* 数据提前结束：下面按"行数不够"处理 */
    }
    const int rows_ok = (cinfo.output_scanline == cinfo.output_height);
    const unsigned got_rows = (unsigned)cinfo.output_scanline;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    /* rows_ok 为假、或解的过程中见过 EOF 警告，都按"残图"处理 */
    if (!rows_ok || jerr.truncated) {
        ql_mem_free(px);
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "数据提前结束（只解出 %u/%u 行，残缺的图不画）",
                     got_rows, (unsigned)h);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->px = px;
    out->len = (uint32_t)total;
    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    out->stride = (uint16_t)stride;
    out->format = QL_IMAGE_RGBA8888;
    out->owned = 1;
    return 1;
}

static const QLImageBackend s_libjpeg = { jpeg_decode, "libjpeg" };

const QLImageBackend *ql_image_backend_libjpeg(void)
{
    return &s_libjpeg;
}

#endif /* QL_FEAT_IMG_LIBJPEG */
