/**
 * @file    test_ui_blob.c
 * @brief   .qlui blob：编码/解码往返、与构建产物的逐字节一致性、损坏检测。
 *
 * 这是"两种形态共存"的护栏：
 *   ① 构建期 C 表 ──ql_ui_doc_encode──▶ blob ──ql_ui_doc_load_mem──▶ doc'，
 *      渲染结果必须与 ① 完全一致；
 *   ② 构建工具 qlblob 产出的 .qlui 必须与本测试现场编码的结果**逐字节相同**，
 *      否则说明"Python 路径"和"C 路径"在格式上漂移了。
 *
 * 找不到构建产物（例如只编了库没编示例）时，② 会跳过但 ① 仍然执行。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"
#include "ui_demo.h"

#if !defined(QL_UI_BLOB_PATH)
#define QL_UI_BLOB_PATH "ui_demo.qlui"
#endif

#define W 320
#define H 320

/* 读整个文件；失败返回 NULL */
static uint8_t *read_all(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)ql_mem_alloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        ql_mem_free(buf);
        return NULL;
    }
    *out_size = (size_t)size;
    return buf;
}

/* 用一份 doc 渲染到内存显示，返回像素缓冲（调用方 ql_disp_mem_destroy） */
static QLDispDriver *render_doc(const QLUIDoc *doc)
{
    if (doc == NULL)
        return NULL;

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL)
        return NULL;

    QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_ARGB8888);
    if (disp == NULL) {
        ql_ui_free(ui);
        return NULL;
    }

    QLCanvas canvas;
    if (ql_canvas_from_disp(&canvas, disp) != QL_OK) {
        ql_disp_mem_destroy(disp);
        ql_ui_free(ui);
        return NULL;
    }

    ql_canvas_clear(&canvas, QL_COLOR_RGB(0x10, 0x14, 0x1C));
    ql_ui_layout(ui, W, H);
    ql_ui_render_canvas(ui, &canvas);

    ql_ui_free(ui);
    return disp;
}

QL_TEST_BEGIN("test_ui_blob")

    /* ---------- 1. 头结构与 ABI 自检 ---------- */
    QL_CHECK_EQ(sizeof(QLUIDocBlobHeader), 144);  /* v28：+ Grid 三段池 */
    QL_CHECK_EQ(sizeof(QLUINode), 28);   /* v27：+ disabled/checked/placeholder 变体与文案 */
    /* v16：+ min/max-width/height（4 × QLUILength = 16 字节） */
    QL_CHECK_EQ(sizeof(QLUIStyle), 312);   /* v28：+ Grid 与 border-image */

    /* ---------- 2. 静态 doc 没有 arena，释放是空操作 ---------- */
    QL_CHECK_EQ(ui_demo_doc.arena, NULL);
    QL_CHECK_EQ(ql_ui_doc_is_owned(&ui_demo_doc), 0);
    ql_ui_doc_free((QLUIDoc *)(uintptr_t)&ui_demo_doc);   /* 不应崩、不应 free */
    QL_CHECK_EQ(ui_demo_doc.node_count, 26);   /* 24 个元素 + 2 个行内间隔 span */              /* 仍然有效 */

    /* ---------- 3. 编码：容量查询与写入 ---------- */
    size_t need = ql_ui_doc_encode(&ui_demo_doc, NULL, 0);
    QL_CHECK(need > sizeof(QLUIDocBlobHeader));
    /* 段按 8 字节对齐 */
    QL_CHECK_EQ(need % 8u, 0u);

    uint8_t *blob = (uint8_t *)ql_mem_alloc(need);
    QL_CHECK(blob != NULL);
    QL_CHECK_EQ(ql_ui_doc_encode(&ui_demo_doc, blob, need), need);
    /* 容量不足时不写、只返回需求 */
    QL_CHECK_EQ(ql_ui_doc_encode(&ui_demo_doc, blob, need - 1), need);

    QLUIDocBlobHeader head;
    memcpy(&head, blob, sizeof(head));
    QL_CHECK_EQ(head.magic, QL_UI_BLOB_MAGIC);
    QL_CHECK_EQ(head.version, QL_UI_BLOB_VERSION);
    QL_CHECK_EQ(head.header_size, sizeof(QLUIDocBlobHeader));
    QL_CHECK_EQ(head.node_count, ui_demo_doc.node_count);
    QL_CHECK_EQ(head.style_count, ui_demo_doc.style_count);
    QL_CHECK_EQ(head.text_count, ui_demo_doc.text_count);
    QL_CHECK_EQ(head.font_size_count, ui_demo_doc.font_size_count);
    QL_CHECK_EQ(head.total_size, need);

    /* ---------- 4. 解码：字段与内容一一对应 ---------- */
    QLUIDoc *loaded = ql_ui_doc_load_mem(blob, need);
    QL_CHECK(loaded != NULL);
    if (loaded == NULL) {
        printf("  加载失败原因：%s\n", ql_ui_doc_last_error());
        ql_mem_free(blob);
        qlgl_shutdown();
        return 1;
    }
    QL_CHECK_EQ(ql_ui_doc_is_owned(loaded), 1);
    QL_CHECK(loaded->arena != NULL);
    QL_CHECK_STR(loaded->name, ui_demo_doc.name);
    QL_CHECK_EQ(loaded->node_count, ui_demo_doc.node_count);
    QL_CHECK_EQ(loaded->style_count, ui_demo_doc.style_count);
    QL_CHECK_EQ(loaded->text_count, ui_demo_doc.text_count);
    QL_CHECK_EQ(loaded->font_size_count, ui_demo_doc.font_size_count);
    QL_CHECK_EQ(memcmp(loaded->nodes, ui_demo_doc.nodes,
                       (size_t)ui_demo_doc.node_count * sizeof(QLUINode)), 0);
    QL_CHECK_EQ(memcmp(loaded->styles, ui_demo_doc.styles,
                       (size_t)ui_demo_doc.style_count * sizeof(QLUIStyle)), 0);
    QL_CHECK_EQ(memcmp(loaded->text_offsets, ui_demo_doc.text_offsets,
                       (size_t)ui_demo_doc.text_count * sizeof(uint16_t)), 0);
    QL_CHECK_EQ(memcmp(loaded->font_sizes, ui_demo_doc.font_sizes,
                       (size_t)ui_demo_doc.font_size_count * sizeof(uint16_t)), 0);

    /* 文本池逐字节相同 */
    size_t pool = ql_ui_doc_text_pool_size(&ui_demo_doc);
    QL_CHECK(pool > 0);
    QL_CHECK_EQ(ql_ui_doc_text_pool_size(loaded), pool);
    QL_CHECK_EQ(memcmp(loaded->text_pool, ui_demo_doc.text_pool, pool), 0);

    /* 中文文本仍然可读（UTF-8 没有被截断） */
    QL_CHECK_STR(loaded->text_pool + loaded->text_offsets[2], "中文字体");

    /* ---------- 5. 重新编码是幂等的（字节层面完全一致） ---------- */
    uint8_t *blob2 = (uint8_t *)ql_mem_alloc(need);
    QL_CHECK(blob2 != NULL);
    QL_CHECK_EQ(ql_ui_doc_encode(loaded, blob2, need), need);
    QL_CHECK_EQ(memcmp(blob, blob2, need), 0);
    ql_mem_free(blob2);

    /* ---------- 6. 加载后的 doc 能正常布局与绘制 ---------- */
    QLDispDriver *ref = render_doc(&ui_demo_doc);
    QLDispDriver *got = render_doc(loaded);
    QL_CHECK(ref != NULL && got != NULL);
    if (ref != NULL && got != NULL) {
        long diff = 0;
        for (ql_coord_t y = 0; y < H; y++)
            for (ql_coord_t x = 0; x < W; x++)
                if (ql_disp_mem_pixel(ref, x, y) != ql_disp_mem_pixel(got, x, y))
                    diff++;
        printf("  渲染差异像素：%ld/%d\n", diff, W * H);
        QL_CHECK_EQ(diff, 0);          /* 同一份数据，必须逐像素相同 */
        ql_disp_mem_destroy(got);
    }
    if (ref != NULL)
        ql_disp_mem_destroy(ref);

    ql_ui_doc_free(loaded);

    /* ---------- 7. 损坏检测：每种失败都要明确拒绝，不能画半个界面 ---------- */
    uint8_t *bad = (uint8_t *)ql_mem_alloc(need);

    /* 7.1 魔数 */
    memcpy(bad, blob, need);
    bad[0] ^= 0xFF;
    QL_CHECK(ql_ui_doc_load_mem(bad, need) == NULL);
    QL_CHECK(strstr(ql_ui_doc_last_error(), "magic") != NULL);

    /* 7.2 版本 */
    memcpy(bad, blob, need);
    bad[4] = 0x7F;
    QL_CHECK(ql_ui_doc_load_mem(bad, need) == NULL);
    QL_CHECK(strstr(ql_ui_doc_last_error(), "version") != NULL);

    /* 7.3 截断（长度与头不符） */
    QL_CHECK(ql_ui_doc_load_mem(blob, need - 1) == NULL);
    QL_CHECK(strstr(ql_ui_doc_last_error(), "size") != NULL);

    /* 7.4 校验和：改 payload 的一个字节 */
    memcpy(bad, blob, need);
    bad[need - 1] ^= 0x01;
    QL_CHECK(ql_ui_doc_load_mem(bad, need) == NULL);
    QL_CHECK(strstr(ql_ui_doc_last_error(), "checksum") != NULL);

    /* 7.5 段偏移被篡改（把 off_nodes 挪 8 字节，校验和会先挡住） */
    memcpy(bad, blob, need);
    QLUIDocBlobHeader h2;
    memcpy(&h2, bad, sizeof(h2));
    h2.off_nodes += 8;
    memcpy(bad, &h2, sizeof(h2));
    QL_CHECK(ql_ui_doc_load_mem(bad, need) == NULL);   /* 校验和或偏移校验挡住 */

    /* 7.6 空/过小输入 */
    QL_CHECK(ql_ui_doc_load_mem(blob, 4) == NULL);
    QL_CHECK(ql_ui_doc_load_mem(NULL, need) == NULL);

    ql_mem_free(bad);
    ql_mem_free(blob);

    /* ---------- 8. 与构建工具 qlblob 的产物逐字节一致 ---------- */
    size_t file_size = 0;
    uint8_t *file_blob = read_all(QL_UI_BLOB_PATH, &file_size);
    if (file_blob == NULL) {
        printf("  [SKIP] 未找到 %s（应先构建 qlui_blob 目标）\n", QL_UI_BLOB_PATH);
    } else {
        size_t ref_size = ql_ui_doc_encode(&ui_demo_doc, NULL, 0);
        QL_CHECK_EQ(file_size, ref_size);
        if (file_size == ref_size) {
            uint8_t *ref_blob = (uint8_t *)ql_mem_alloc(ref_size);
            QL_CHECK(ref_blob != NULL);
            if (ref_blob != NULL) {
                ql_ui_doc_encode(&ui_demo_doc, ref_blob, ref_size);
                QL_CHECK_EQ(memcmp(ref_blob, file_blob, ref_size), 0);
                ql_mem_free(ref_blob);
            }
        }
        /* 文件也能直接 build 并布局 */
        QLUIDoc *from_file = ql_ui_doc_load_mem(file_blob, file_size);
        QL_CHECK(from_file != NULL);
        if (from_file != NULL) {
            QLUIRoot *ui = ql_ui_build(from_file);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QL_CHECK(ql_ui_node_rect(ql_ui_root_node(ui)).w == W);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(from_file);
        }
        ql_mem_free(file_blob);
    }

    /* ---------- 9. 文件路径 API：ql_ui_load_file 自己管 doc 生命周期 ---------- */
    QLUIRoot *via_file = ql_ui_load_file(QL_UI_BLOB_PATH);
    if (via_file == NULL) {
        printf("  [SKIP] ql_ui_load_file 找不到文件\n");
    } else {
        QL_CHECK(ql_ui_doc_is_owned(via_file->doc));
        ql_ui_layout(via_file, W, H);
        QL_CHECK_EQ(ql_ui_node_rect(ql_ui_root_node(via_file)).w, W);
        ql_ui_free(via_file);         /* doc 应随 root 一起被释放 */
    }

    /* 不存在的文件：返回 NULL 且不泄漏 */
    QL_CHECK(ql_ui_load_file("/nonexistent/x.qlui") == NULL);
    QL_CHECK(ql_ui_doc_load("/nonexistent/x.qlui") == NULL);
    QL_CHECK(ql_ui_doc_load(NULL) == NULL);

QL_TEST_END()
