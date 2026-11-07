/* 本文件由 tools/qlgen/qlgen.py 生成，请勿手工修改。 */
#include <qlgl/qlgl.h>

extern QLObjectClass ql_object_class;
extern QLWidgetClass ql_widget_class;
extern QLLabelClass ql_label_class;

const QLBuiltinClass ql_builtin_class_table[] = {
    { QL_TYPE_OBJECT, QL_TYPE_NONE, "QLObject", (QLObjectClass *)(void *)&ql_object_class, ql_object_class_setup },
    { QL_TYPE_WIDGET, QL_TYPE_OBJECT, "QLWidget", (QLObjectClass *)(void *)&ql_widget_class, ql_widget_class_setup },
    { QL_TYPE_LABEL, QL_TYPE_WIDGET, "QLLabel", (QLObjectClass *)(void *)&ql_label_class, ql_label_class_setup },
};

const uint16_t ql_builtin_class_count =
    (uint16_t)QL_ARRAY_SIZE(ql_builtin_class_table);
