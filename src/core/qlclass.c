/**
 * @file    qlclass.c
 * @brief   类表与注册。父类必须先于子类注册，注册时算好私有区布局。
 */
#include <qlgl/qlgl.h>

static QLObjectClass *s_class_table[QL_CFG_MAX_CLASS];
static uint16_t s_class_count;

/*
 * 私有区按层紧凑排布，但每层的起点必须满足 QLObject 的对齐要求，
 * 否则 self = mem + priv_total 可能落在非对齐地址上（UBSan 会报
 * "member access within misaligned address"）。
 */
#define QL_PRIV_ALIGN       ((uint32_t)_Alignof(max_align_t))
#define QL_PRIV_ROUND(v)    (((v) + QL_PRIV_ALIGN - 1u) & ~(QL_PRIV_ALIGN - 1u))

QLObjectClass *ql_class_get(QLType type)
{
    if (type == QL_TYPE_NONE || type >= s_class_count)
        return NULL;
    return s_class_table[type];
}

int ql_class_is_a(QLObjectClass *klass, QLType type)
{
    for (QLObjectClass *k = klass; k != NULL; k = k->parent) {
        if (k->type == type)
            return 1;
    }
    return 0;
}

uint16_t ql_class_count(void)
{
    return s_class_count;
}

const char *ql_class_name(QLType type)
{
    QLObjectClass *klass = ql_class_get(type);
    return (klass != NULL && klass->name != NULL) ? klass->name : "";
}

void ql_class_reset(void);
void ql_class_reset(void)
{
    s_class_count = 0;
}

QLType ql_class_register(QLObjectClass *klass, QLType parent_type,
                         void (*setup)(QLObjectClass *klass))
{
    if (klass == NULL)
        return QL_TYPE_NONE;

    if (klass->instance_size == 0) {
        if (setup == NULL) {
            QL_LOGE("ql_class_register: %s has no setup function",
                    klass->name ? klass->name : "(anonymous)");
            return QL_TYPE_NONE;
        }
        setup(klass);
    }

    if (s_class_count >= QL_CFG_MAX_CLASS) {
        QL_LOGE("ql_class_register: class table full (max %d)", QL_CFG_MAX_CLASS);
        return QL_TYPE_NONE;
    }

    QLObjectClass *parent = NULL;
    if (parent_type != QL_TYPE_NONE) {
        parent = ql_class_get(parent_type);
        if (parent == NULL) {
            QL_LOGE("ql_class_register: parent type %u not registered", (unsigned)parent_type);
            return QL_TYPE_NONE;
        }
    }

    /* 深度检查：本类算一层 */
    uint32_t depth = 1;
    for (QLObjectClass *p = parent; p != NULL; p = p->parent) {
        if (++depth > QL_CFG_MAX_INHERIT_DEPTH) {
            QL_LOGE("ql_class_register: inheritance depth exceeds %d", QL_CFG_MAX_INHERIT_DEPTH);
            return QL_TYPE_NONE;
        }
    }

    /* 本类私有区向上对齐；父类注册时已经对齐过，所以累加仍是对齐的 */
    uint32_t own = QL_PRIV_ROUND(klass->own_private_size);

    /* 私有区按"根 → 派生"顺序紧凑排布，偏移与派生类无关 */
    uint32_t priv_offset = (parent != NULL)
        ? (uint32_t)parent->priv_offset + parent->own_private_size
        : 0u;
    uint32_t priv_total = (parent != NULL ? parent->priv_total : 0u) + own;

    if (priv_offset > 0xFFFFu || priv_total > 0xFFFFu) {
        QL_LOGE("ql_class_register: private area exceeds 64KB");
        return QL_TYPE_NONE;
    }

    klass->type = s_class_count;
    klass->parent = parent;
    klass->own_private_size = (uint16_t)own;
    klass->priv_offset = (uint16_t)priv_offset;
    klass->priv_total = (uint16_t)priv_total;

    s_class_table[s_class_count++] = klass;

    if (klass->class_init != NULL)
        klass->class_init(klass);

    QL_LOGD("class registered: %s (type=%u, instance=%u, private=%u, total=%u)",
            klass->name ? klass->name : "?",
            (unsigned)klass->type,
            (unsigned)klass->instance_size,
            (unsigned)klass->own_private_size,
            (unsigned)klass->priv_total);

    return klass->type;
}
