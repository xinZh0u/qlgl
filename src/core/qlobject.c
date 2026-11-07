/**
 * @file    qlobject.c
 * @brief   根类实现：一次分配、父类先初始化、析构时子对象先走。
 */
#include <string.h>

#include <qlgl/qlgl.h>

/* 根类不用 QL_DEFINE_WITH_PRIVATE：它没有私有区，id 固定为 0 */
QLObjectClass ql_object_class;

void ql_object_class_setup(QLObjectClass *klass)
{
    klass->instance_size    = (uint16_t)sizeof(QLObject);
    klass->own_private_size = 0;
    klass->name             = "QLObject";
    klass->property_start   = QL_OBJECT_PROPERTY_START;
    klass->property_end     = QL_OBJECT_PROPERTY_END;
    klass->props            = NULL;
    klass->prop_count       = 0;
    klass->events           = NULL;
    klass->event_count      = 0;
    klass->init             = NULL;
    klass->deinit           = NULL;
    klass->class_init       = NULL;
    klass->set_property     = NULL;
    klass->get_property     = NULL;
    klass->event            = NULL;
    /* type 由 ql_class_register 分配 */
}

QLType ql_object_get_type(void)
{
    return ql_object_class.type;
}

/* ----- 查询 ----- */

QLObjectClass *ql_object_get_class(QLObject *self)
{
    return self != NULL ? self->klass : NULL;
}

QLType ql_object_type(QLObject *self)
{
    return (self != NULL) ? self->klass->type : QL_TYPE_NONE;
}

const char *ql_object_type_name(QLObject *self)
{
    if (self == NULL || self->klass->name == NULL)
        return "";
    return self->klass->name;
}

int32_t ql_object_ref_count(QLObject *self)
{
    return (self != NULL) ? self->ref_count : 0;
}

int ql_object_is(QLObject *self, QLType type)
{
    if (self == NULL)
        return 0;
    for (QLObjectClass *klass = self->klass; klass != NULL; klass = klass->parent) {
        if (klass->type == type)
            return 1;
    }
    return 0;
}

/* ----- 生命周期 ----- */

QLObject *ql_object_new(QLType type)
{
    QLObjectClass *klass = ql_class_get(type);
    if (klass == NULL) {
        QL_LOGE("ql_object_new: unknown type %u (did you call qlgl_init?)", (unsigned)type);
        return NULL;
    }

    /* 先把继承链收进栈上数组：既算深度，也用于 init 顺序 */
    QLObjectClass *chain[QL_CFG_MAX_INHERIT_DEPTH];
    int depth = 0;
    for (QLObjectClass *k = klass; k != NULL; k = k->parent) {
        if (depth >= QL_CFG_MAX_INHERIT_DEPTH) {
            QL_LOGE("ql_object_new: inheritance chain exceeds %d levels", QL_CFG_MAX_INHERIT_DEPTH);
            return NULL;
        }
        chain[depth++] = k;
    }

    size_t total = (size_t)klass->instance_size + klass->priv_total;
    char *mem = (char *)ql_mem_calloc(1, total);
    if (mem == NULL) {
        QL_LOGE("ql_object_new: failed to allocate %u bytes", (unsigned)total);
        return NULL;
    }

    /* 私有区在块首，实例在末尾；klass->priv_total 已按 max_align_t 对齐 */
    QLObject *self = (QLObject *)(void *)(mem + klass->priv_total);
    _Static_assert((_Alignof(max_align_t) & (_Alignof(max_align_t) - 1u)) == 0,
                   "max_align_t 必须是 2 的幂");
    self->klass = klass;
    self->parent = NULL;
    self->first_child = NULL;
    self->next_sibling = NULL;
    self->ref_count = 1;
    self->conn_head = QL_CONN16_NONE;
    self->flags = 0;
    self->state = QL_OBJ_STATE_ALIVE;

    /* 父 → 子 */
    for (int i = depth - 1; i >= 0; i--) {
        if (chain[i]->init != NULL)
            chain[i]->init(self);
    }

    return self;
}

QLObject *ql_object_ref(QLObject *self)
{
    if (self == NULL)
        return NULL;
    if (self->state == QL_OBJ_STATE_DEAD) {
        QL_LOGE("ql_object_ref: object already destroyed (%p)", (void *)self);
        return NULL;
    }
    self->ref_count++;
    return self;
}

static ql_object_tree_hook s_tree_hook;

void ql_object_set_tree_hook(ql_object_tree_hook hook)
{
    s_tree_hook = hook;
}

/* 只摘链，不改引用计数 */
static void object_unlink(QLObject *self)
{
    QLObject *parent = self->parent;
    if (parent == NULL) {
        self->next_sibling = NULL;
        return;
    }

    /* 摘除前通知：此时父链还在，能算出原来的绝对位置 */
    if (s_tree_hook != NULL)
        s_tree_hook(self);

    QLObject **link = &parent->first_child;
    while (*link != NULL) {
        if (*link == self) {
            *link = self->next_sibling;
            break;
        }
        link = &(*link)->next_sibling;
    }

    self->parent = NULL;
    self->next_sibling = NULL;
    self->flags &= (uint16_t)~QL_OBJ_FLAG_ATTACHED;
}

static void object_destroy(QLObject *self)
{
    self->state = QL_OBJ_STATE_DESTROYING;

    /* 1. 子对象：先摘链再 unref，避免递归修改链表时错位 */
    while (self->first_child != NULL) {
        QLObject *child = self->first_child;
        self->first_child = child->next_sibling;
        child->next_sibling = NULL;
        child->parent = NULL;
        child->flags &= (uint16_t)~QL_OBJ_FLAG_ATTACHED;
        ql_object_unref(child);
    }

    /* 2. 事件连接归还池 */
    ql_disconnect_all(self);

    /* 3. deinit：派生 → 根 */
    QLObjectClass *chain[QL_CFG_MAX_INHERIT_DEPTH];
    int depth = 0;
    for (QLObjectClass *k = self->klass; k != NULL && depth < QL_CFG_MAX_INHERIT_DEPTH;
         k = k->parent) {
        chain[depth++] = k;
    }
    for (int i = 0; i < depth; i++) {
        if (chain[i]->deinit != NULL)
            chain[i]->deinit(self);
    }

    /* 4. 与父对象解绑（父对子持有引用，此方向不反向引用父） */
    object_unlink(self);

    /* 5. 释放块首，绝不能释放 self */
    char *mem = (char *)self - self->klass->priv_total;
    self->state = QL_OBJ_STATE_DEAD;
    ql_mem_free(mem);
}

void ql_object_unref(QLObject *self)
{
    if (self == NULL)
        return;

    if (self->state == QL_OBJ_STATE_DESTROYING) {
        QL_LOGE("ql_object_unref: repeated unref while destroying (%p)", (void *)self);
        return;
    }

    if (self->ref_count <= 0)
        return;

    if (--self->ref_count > 0)
        return;

    object_destroy(self);
}

/* ----- 对象树 ----- */

ql_err_t ql_object_set_parent(QLObject *self, QLObject *parent)
{
    if (self == NULL)
        return QL_ERR_INVAL;

    /* 禁止成环：parent 不能是自己或自己的后代 */
    for (QLObject *p = parent; p != NULL; p = p->parent) {
        if (p == self)
            return QL_ERR_INVAL;
    }

    QLObject *old_parent = self->parent;
    if (old_parent == parent)
        return QL_OK;

    /* 守卫引用：归还旧父的引用时可能把自己引用到 0 */
    ql_object_ref(self);
    object_unlink(self);
    if (old_parent != NULL)
        ql_object_unref(self);

    if (parent != NULL) {
        /*
         * 追加到链尾而不是头插：child 链表顺序 == 绘制顺序（先挂的先画），
         * 这样"后挂的子控件覆盖先挂的"（与 DOM append 一致），
         * 命中测试取最后一个命中的节点也就是最上层的节点。
         * 挂接只发生在构建期，O(子控件数) 的尾查找可以接受，
         * 换来的是每个对象省一个 last_child 指针。
         */
        self->parent = parent;
        self->next_sibling = NULL;
        if (parent->first_child == NULL) {
            parent->first_child = self;
        } else {
            QLObject *tail = parent->first_child;
            while (tail->next_sibling != NULL)
                tail = tail->next_sibling;
            tail->next_sibling = self;
        }
        self->flags |= QL_OBJ_FLAG_ATTACHED;
        ql_object_ref(self);       /* 父持有子对象的引用 */
        if (s_tree_hook != NULL)
            s_tree_hook(self);     /* 新挂上的控件需要重绘 */
    }

    ql_object_unref(self);         /* 释放守卫 */
    return QL_OK;
}

ql_err_t ql_object_add_child(QLObject *parent, QLObject *child)
{
    if (child == NULL)
        return QL_ERR_INVAL;

    ql_err_t err = ql_object_set_parent(child, parent);
    if (err != QL_OK)
        return err;

    /* 父对象已持有一份引用，归还调用方这一份 */
    ql_object_unref(child);
    return QL_OK;
}

QLObject *ql_object_parent(QLObject *self)
{
    return (self != NULL) ? self->parent : NULL;
}

QLObject *ql_object_first_child(QLObject *self)
{
    return (self != NULL) ? self->first_child : NULL;
}

QLObject *ql_object_next_sibling(QLObject *self)
{
    return (self != NULL) ? self->next_sibling : NULL;
}

uint32_t ql_object_child_count(QLObject *self)
{
    uint32_t count = 0;
    if (self == NULL)
        return 0;
    for (QLObject *child = self->first_child; child != NULL; child = child->next_sibling)
        count++;
    return count;
}
