#ifndef QLLABEL_H
#define QLLABEL_H

#include <qlwidget.h>

QL_PROPERTY_ENUM_START(QL_LABEL,QLLabel,QL_WIDGET)
    QL_LABEL_PROPERTY_NAME,
    QL_LABEL_PROPERTY_END,
QL_PROPERTY_ENUM_END(QL_LABEL,100)

QL_DECLARE(QLLabel,ql_label)
#define QL_LABEL_TYPE ql_label_get_type()

struct QLLabel{
    QLWidget parent;
};

struct QLLabelClass{
    QLWidgetClass parent;
};

QLLabel *ql_label_new(void);

#endif // QLLABEL_H
