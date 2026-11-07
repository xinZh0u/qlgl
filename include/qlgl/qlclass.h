/**
 * @file    qlclass.h
 * @brief   类/类型系统：类描述结构、注册、以及定义新类的宏。
 *
 * 设计要点：
 *  1. QLType 固定 32 位，id 由 tools/qlgen 在构建期生成（见 qltype_gen.h）；
 *  2. 类注册必须"父类先于子类"，注册时一次性算好私有区偏移与总大小；
 *  3. 不使用 __attribute__((constructor))，裸机也能按表注册。
 */
#ifndef QLGL_CLASS_H
#define QLGL_CLASS_H

#include "qlgl.h"

typedef struct QLObject QLObject;
typedef struct QLObjectClass QLObjectClass;

/* 32 位，禁止改成 unsigned long（32/64 位不等长） */
typedef uint32_t QLType;

struct QLObjectClass {
    QLType        type;             /* 构建期 id，运行期会校验一致 */
    uint16_t      instance_size;    /* 本类结构体大小（含父类字段） */
    uint16_t      own_private_size; /* 本类私有区大小 */
    uint16_t      priv_offset;      /* 私有区相对分配块首的偏移（注册时算好） */
    uint16_t      priv_total;       /* 整条继承链私有区总和（注册时算好） */
    uint16_t      reserved16;

    const char   *name;

    ql_property_t property_start;   /* 含 */
    ql_property_t property_end;     /* 含 */

    const QLPropertyDesc *props;
    uint16_t      prop_count;

    const QLEventDesc *events;
    uint16_t      event_count;

    struct QLObjectClass *parent;

    /* 虚函数：一律接收 QLObject*，由宏生成 trampoline 完成向下转型 */
    void     (*init)(QLObject *self);
    void     (*deinit)(QLObject *self);
    void     (*class_init)(struct QLObjectClass *klass);
    ql_err_t (*set_property)(QLObject *self, ql_property_t property, QLValue value);
    QLValue  (*get_property)(QLObject *self, ql_property_t property);
    ql_err_t (*event)(QLObject *self, QLEvent *event);
};

/* 构建期生成的类表项 */
typedef struct {
    QLType         type;
    QLType         parent_type;   /* QL_TYPE_NONE 表示根类 */
    const char    *name;
    QLObjectClass *klass;
    /* 注册时先调用它填充 sizeof/属性区间等编译期常量，避免嵌套聚合初始化 */
    void         (*setup)(QLObjectClass *klass);
} QLBuiltinClass;

/*
 * 注册一个类，返回它的运行期 id。
 * parent_type 必须是已注册的类；失败返回 QL_TYPE_NONE。
 * 内置类由 qlgl_init() 按生成表注册；测试/插件可以自行调用。
 */
QLType ql_class_register(QLObjectClass *klass, QLType parent_type,
                         void (*setup)(QLObjectClass *klass));

QLObjectClass *ql_class_get(QLType type);       /* 越界返回 NULL */
/* klass 是否是 type 本身或它的派生类 */
int            ql_class_is_a(QLObjectClass *klass, QLType type);
uint16_t       ql_class_count(void);
const char    *ql_class_name(QLType type);

/* ----- 定义新类的宏 ----- */

/*
 * QL_DECLARE(QLWidget, ql_widget)
 *   放在头文件里：声明 QLWidget / QLWidgetClass / struct QLWidgetPrivate，
 *   以及 get_type / private_size / get_private 三个函数。
 */
#define QL_DECLARE(Name, name)                                               \
    typedef struct Name Name;                                                \
    typedef struct Name##Class Name##Class;                                  \
    struct Name##Private;                                                    \
    extern Name##Class name##_class;                                         \
    QLType name##_get_type(void);                                            \
    uint32_t name##_private_size(void);                                      \
    void name##_class_setup(QLObjectClass *klass);                           \
    struct Name##Private *name##_get_private(Name *self);

/*
 * QL_DEFINE_WITH_PRIVATE(QLWidget, QLWidget, ql_widget, ql_object)
 *   第 4 个参数是父类的"函数前缀"，tools/qlgen 靠它推导继承关系，
 *   即使这里没有直接用到，也不要删。
 *
 * 使用要求：
 *   - 之前必须定义 struct Name##Private；
 *   - 之前必须定义属性枚举（NAME##_PROPERTY_START / NAME##_PROPERTY_END）；
 *   - 之后必须定义三个 static 函数：
 *         static void ql_widget_class_init(QLWidgetClass *klass);
 *         static void ql_widget_init(QLWidget *self);
 *         static void ql_widget_deinit(QLWidget *self);
 *   - 调用处不要再加分号。
 */
#define QL_DEFINE_WITH_PRIVATE(NAME, Name, name, Inherit)                     \
    _Static_assert(sizeof(Name) <= 0xFFFFu, "实例结构体超过 64KB");           \
    _Static_assert(sizeof(struct Name##Private) <= 0xFFFFu,                   \
                   "私有区超过 64KB");                                         \
    static void name##_class_init(Name##Class *klass);                        \
    static void name##_init(Name *self);                                      \
    static void name##_deinit(Name *self);                                    \
    static void name##_init_tramp(QLObject *self)                             \
    {                                                                         \
        name##_init((Name *)self);                                            \
    }                                                                         \
    static void name##_deinit_tramp(QLObject *self)                           \
    {                                                                         \
        name##_deinit((Name *)self);                                          \
    }                                                                         \
    static void name##_class_init_tramp(QLObjectClass *klass)                 \
    {                                                                         \
        name##_class_init((Name##Class *)klass);                              \
    }                                                                         \
    QLType name##_get_type(void)                                              \
    {                                                                         \
        return ((QLObjectClass *)(void *)&name##_class)->type;                 \
    }                                                                         \
    uint32_t name##_private_size(void)                                        \
    {                                                                         \
        return (uint32_t)sizeof(struct Name##Private);                        \
    }                                                                         \
    struct Name##Private *name##_get_private(Name *self)                      \
    {                                                                         \
        const QLObject *_obj = (const QLObject *)(const void *)self;           \
        const QLObjectClass *_own = (const QLObjectClass *)(const void *)&name##_class; \
        return (struct Name##Private *)((char *)self                          \
                - _obj->klass->priv_total                                     \
                + _own->priv_offset);                                         \
    }                                                                         \
    Name##Class name##_class;                                                 \
    void name##_class_setup(QLObjectClass *klass)                             \
    {                                                                         \
        klass->instance_size    = (uint16_t)sizeof(Name);                      \
        klass->own_private_size = (uint16_t)sizeof(struct Name##Private);      \
        QL_CLASS_SET_NAME(klass, #Name);                                       \
        klass->property_start   = NAME##_PROPERTY_START;                       \
        klass->property_end     = NAME##_PROPERTY_END;                         \
        klass->init             = name##_init_tramp;                           \
        klass->deinit           = name##_deinit_tramp;                         \
        klass->class_init       = name##_class_init_tramp;                     \
    }

/*
 * 宏参数 name 会替换掉类描述里的 .name 字段名，所以字段访问走一层间接宏：
 * QL_CLASS_SET_NAME 的形参是 klass/v，其中的 name 不会参与外层参数替换。
 */
#define QL_CLASS_SET_NAME(k, v)  ((k)->name = (v))

/*
 * 类变量的真实类型是 Name##Class（比 QLObjectClass 大，可能带额外虚函数），
 * 传给只认 QLObjectClass* 的 API 时用它转一次。
 */
#define QL_CLASS_OF(name)   ((QLObjectClass *)(void *)&name##_class)

/* 读取类描述（各字段与 QLObjectClass 一致） */
#define QL_CLASS_INFO(name) (*QL_CLASS_OF(name))

/* 注册一个用 QL_DEFINE_WITH_PRIVATE 定义的类 */
#define QL_REGISTER(name, parent_type)                                       \
    ql_class_register(QL_CLASS_OF(name), (parent_type), name##_class_setup)

/* 取私有区（推荐写法，省掉强制转换） */
#define QL_PRIV(Name, name, self)                                             \
    name##_get_private((Name *)(void *)(self))

#endif /* QLGL_CLASS_H */
