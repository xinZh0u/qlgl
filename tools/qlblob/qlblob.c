/**
 * @file    qlblob.c
 * @brief   把构建期生成的 QLUIDoc 导出成 .qlui blob（宿主工具，不进目标机）。
 *
 * 关键设计：**blob 的格式只在 C 侧实现一次**（src/ui/qlui_blob.c）。
 * 这个工具只是"链上生成的 ui_xxx.c，调一次 ql_ui_doc_save()"，
 * 所以不存在 Python 与 C 两份序列化代码互相漂移的可能。
 *
 * 用法：
 *     qlblob <out.qlui>
 *
 * 需要在链接时提供 qlui_blob_doc()（由 tools/qlui/qlui.py 生成的
 * ui_<name>_blob.c 提供，返回 &ui_<name>_doc）。
 */
#include <stdio.h>

#include <qlgl/qlgl.h>

extern const QLUIDoc *qlui_blob_doc(void);

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out.qlui>\n", argv[0]);
        return 1;
    }

    const QLUIDoc *doc = qlui_blob_doc();
    if (doc == NULL) {
        fprintf(stderr, "qlblob: qlui_blob_doc() returned NULL\n");
        return 1;
    }

    size_t size = ql_ui_doc_encode(doc, NULL, 0);
    if (size == 0) {
        fprintf(stderr, "qlblob: doc is invalid (encode returned 0)\n");
        return 1;
    }

    if (ql_ui_doc_save(doc, argv[1]) != QL_OK) {
        fprintf(stderr, "qlblob: cannot write \"%s\"\n", argv[1]);
        return 1;
    }

    printf("qlblob: %s (%zu bytes, %u nodes, %u styles)\n", argv[1], size,
           (unsigned)doc->node_count, (unsigned)doc->style_count);
    return 0;
}
