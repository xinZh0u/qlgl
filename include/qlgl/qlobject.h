/**
 * @file    qlobject.h
 * @brief   根类 QLObject：生命周期、对象树、私有区访问。
 *
 * 内存布局（一次分配，块首为私有区，末尾是实例）：
 *
 *   [ 根类私有区 ][ ... ][ 本类私有区 ][ QLObject 实例 ]
 *    ^ priv_offset 偏移量按层累加                        ^ self
 *
 * self - klass->priv_total 即为 malloc 返回的块首，释放时必须用它。
 */
#ifndef QLGL_OBJECT_H
#define QLGL_OBJECT_H

#include "qlgl.h"

struct QLObject {
    QLObjectClass *klass;
    struct QLObject *parent;
    struct QLObject *first_child;
    struct QLObject *next_sibling;
    int32_t   ref_count;
    uint32_t  conn_head;     /* 事件连接池下标，QL_CONN16_NONE 表示空 */
    uint16_t  flags;         /* QL_OBJ_FLAG_* */
    uint8_t   state;         /* QL_OBJ_STATE_* */
    uint8_t   reserved;
};

/* 对象状态：析构期间禁止再次进入析构 */
enum {
    QL_OBJ_STATE_ALIVE      = 0,
    QL_OBJ_STATE_DESTROYING = 1,
    QL_OBJ_STATE_DEAD       = 2,
};

enum {
    QL_OBJ_FLAG_HIDDEN          = 1u << 0,
    QL_OBJ_FLAG_DIRTY_LAYOUT    = 1u << 1,
    QL_OBJ_FLAG_DIRTY_DRAW      = 1u << 2,
    QL_OBJ_FLAG_ATTACHED        = 1u << 3,   /* 已挂到父对象上 */
};

extern QLObjectClass ql_object_class;
void ql_object_class_setup(QLObjectClass *klass);

QLType ql_object_get_type(void);

/* ----- 生命周期 ----- */
QLObject *ql_object_new(QLType type);
QLObject *ql_object_ref(QLObject *self);
void      ql_object_unref(QLObject *self);

/* ----- 查询 ----- */
QLObjectClass *ql_object_get_class(QLObject *self);
QLType         ql_object_type(QLObject *self);
const char    *ql_object_type_name(QLObject *self);
int32_t        ql_object_ref_count(QLObject *self);
int            ql_object_is(QLObject *self, QLType type);   /* 含父类判断 */

/* ----- 对象树 ----- */
/*
 * set_parent 会为父对象增加一次对子对象的引用；调用方若不再使用该子对象，
 * 需要自己 ql_object_unref 一次。常见场景请直接用 add_child：
 * 它等价于 set_parent(child, parent) 之后再释放调用方那份引用。
 */
ql_err_t   ql_object_set_parent(QLObject *self, QLObject *parent);
ql_err_t   ql_object_add_child(QLObject *parent, QLObject *child);

QLObject  *ql_object_parent(QLObject *self);
QLObject  *ql_object_first_child(QLObject *self);
QLObject  *ql_object_next_sibling(QLObject *self);
uint32_t   ql_object_child_count(QLObject *self);

/*
 * 对象树结构变化钩子：挂接/摘除时被调用。
 * 渲染层用它把新挂上的控件标记为脏；对象层本身不关心绘制。
 */
typedef void (*ql_object_tree_hook)(QLObject *obj);
void ql_object_set_tree_hook(ql_object_tree_hook hook);

/* ----- 私有区（O(1)） ----- */
static inline void *ql_priv(QLObject *self, QLObjectClass *klass)
{
    return (char *)self - self->klass->priv_total + klass->priv_offset;
}

#endif /* QLGL_OBJECT_H */
