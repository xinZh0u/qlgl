#ifndef QLCLASS_H
#define QLCLASS_H

#include <qlobject.h>

#define QL_DECLARE(Name, name)                                      \
typedef struct Name Name;                                           \
typedef struct Name##Class Name##Class;                             \
                                                                    \
extern Name##Class name##_class;                                    \
                                                                    \
QLType name##_get_type();                                           \
uint32_t name##_private_get_size();                                 \


#define QL_DEFINE_WITH_PRIVATE(NAME,Name, name, Inherit)            \
typedef struct Name##Private Name##Private;                         \
static void name##_class_init(Name##Class *klass);                  \
static void name##_init(Name *self);                                \
static void name##_deinit(Name *self);                              \
                                                                    \
QLType name##_get_type(){                                           \
        return ((QLObjectClass*)&name##_class)->type;               \
}                                                                   \
                                                                    \
uint32_t name##_private_get_size(){                                 \
      return Inherit##_private_get_size() + sizeof(struct Name##Private);           \
}                                                                                   \
                                                                                    \
Name##Private * name##_get_private(Name * self){                                    \
    return (Name##Private *)((char*)self - name##_private_get_size());              \
}                                                                                   \
                                                                                \
                                                                                \
                                                                                \
Name##Class name##_class;                                                       \
__attribute__((constructor))                                                    \
    static void name##_register(void)                                           \
{                                                                               \
        Name##Class *k = &name##_class;                                         \
        QLObjectClass * pclass = (QLObjectClass *)&name##_class;                \
        pclass->instance_size = sizeof(Name);                                   \
        pclass->private_size = name##_private_get_size;                         \
        pclass->init = (void (*)(QLObject *))name##_init;                       \
        pclass->deinit = (void (*)(QLObject *))name##_deinit;                   \
        pclass->class_init = (void (*)(QLObjectClass *))name##_class_init;      \
        pclass->parent_type = Inherit##_get_type;                               \
        pclass->property_start = NAME##_PROPERTY_START;                         \
        pclass->property_end = NAME##_PROPERTY_END;                             \
        ql_class_register((QLObjectClass *)k);                                  \
}                                                                               \


// 类属性宏定义
#define QL_PROPERTY_ENUM_START(NAME,Name,INHERIT)                                \
typedef enum Name##Property Name##Property;                           \
enum Name##Property {                                                    \
    NAME##_PROPERTY_START = INHERIT##_PROPERTY_SPACE_END,                   \

#define QL_PROPERTY_ENUM_END(NAME,capacity)                                 \
    NAME##_PROPERTY_SPACE_END = NAME##_PROPERTY_START+capacity,               \
};                                                                          \
//为了对其ABI,需要进行参数区域拓展，比如WIDGET有10个参数，那么原则上是拓充到100个，具体情况具体分析。总之要有大余量，不可能用完

// #define QL_VALUE_INT(value)    ql_value_int(int value)

#define QL_PROPERTY_SET(Name,name,obj,property)                            \
Name * self = (Name*)obj;                                                 \
Name##Private * private = name##_get_private(self);                          \
switch (property) {                                                         \

#define QL_SET_CASE(value,property,TYPE)                                              \
break;                                                                      \
case property:                                                              \
if(value.type!=TYPE)break;                                                  \

#define QL_PROPERTY_SET_END                 \
break;                                         \
}                                               \

#define QL_PROPERTY_GET(Name,name,obj,property)                             \
Name * self = (Name*)obj;                                                   \
Name##Private * private = name##_get_private(self);                         \
QLValue value = {0};                                                        \
switch (property) {                                                         \

#define QL_GET_CASE(property,TYPE)                                          \
break;                                                                      \
case property:                                                              \
    value.type = TYPE;                                                      \

#define QL_PROPERTY_GET_END                    \
break;                                      \
}                                           \
return value;                               \

                                                                       \

#endif
