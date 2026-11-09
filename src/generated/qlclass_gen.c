/* 本文件由 tools/qlgen/qlgen.py 生成，请勿手工修改。 */
#include <qlgl/qlgl.h>

extern QLObjectClass ql_object_class;

const QLBuiltinClass ql_builtin_class_table[] = {
    { QL_TYPE_OBJECT, QL_TYPE_NONE, "QLObject", (QLObjectClass *)(void *)&ql_object_class, ql_object_class_setup },
};

const uint16_t ql_builtin_class_count =
    (uint16_t)QL_ARRAY_SIZE(ql_builtin_class_table);
