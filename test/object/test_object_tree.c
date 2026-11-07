/**
 * @file    test_object_tree.c
 * @brief   示例：父子关系、引用计数归属、级联销毁与成环检查。
 */
#include "../ql_test.h"

static int g_destroyed;

QL_PROPERTY_ENUM_START(QL_NODE, QLNode, QL_OBJECT)
    QL_NODE_PROPERTY_ID
QL_PROPERTY_ENUM_END(QL_NODE, 10)

QL_DECLARE(QLNode, ql_node)

struct QLNode        { QLObject parent; };
struct QLNodeClass   { QLObjectClass parent; };
struct QLNodePrivate { int32_t id; };

QL_DEFINE_WITH_PRIVATE(QL_NODE, QLNode, ql_node, ql_object)

static void ql_node_class_init(QLNodeClass *klass) { QL_UNUSED(klass); }
static void ql_node_init(QLNode *self)             { QL_UNUSED(self); }
static void ql_node_deinit(QLNode *self)
{
    struct QLNodePrivate *priv = ql_node_get_private(self);
    QL_UNUSED(priv);
    g_destroyed++;
}

QL_TEST_BEGIN("test_object_tree")

    QLType type = QL_REGISTER(ql_node, QL_TYPE_OBJECT);
    QL_CHECK(type != QL_TYPE_NONE);

    QLObject *root  = ql_object_new(type);
    QLObject *child1 = ql_object_new(type);
    QLObject *child2 = ql_object_new(type);
    QLObject *grand  = ql_object_new(type);
    QL_CHECK(root && child1 && child2 && grand);

    /* 1. 挂接：add_child 会接管调用方这一份引用 */
    QL_CHECK_EQ(ql_object_add_child(root, child1), QL_OK);
    QL_CHECK_EQ(ql_object_add_child(root, child2), QL_OK);
    QL_CHECK_EQ(ql_object_add_child(child1, grand), QL_OK);
    QL_CHECK_EQ(ql_object_ref_count(child1), 1);      /* 只剩父对象的引用 */
    QL_CHECK_EQ(ql_object_child_count(root), 2);
    QL_CHECK(ql_object_parent(grand) == child1);
    QL_CHECK(ql_object_first_child(root) != NULL);

    /* 2. 成环被拒绝：不能把自己的祖先设成子对象 */
    QL_CHECK_EQ(ql_object_set_parent(root, grand), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_object_set_parent(root, root), QL_ERR_INVAL);

    /* 3. 换父：旧父释放引用，新父取得引用 */
    QL_CHECK_EQ(ql_object_set_parent(child2, child1), QL_OK);
    QL_CHECK(ql_object_parent(child2) == child1);
    QL_CHECK_EQ(ql_object_child_count(root), 1);
    QL_CHECK_EQ(ql_object_child_count(child1), 2);
    QL_CHECK_EQ(ql_object_ref_count(child2), 1);

    /* 4. 摘除：调用方未持有引用时，摘除即销毁 */
    g_destroyed = 0;
    QL_CHECK_EQ(ql_object_set_parent(child2, NULL), QL_OK);
    QL_CHECK_EQ(g_destroyed, 1);

    /* 5. 级联销毁：unref(root) 会递归销毁 child1 → grand */
    g_destroyed = 0;
    QL_CHECK_EQ(ql_object_ref_count(root), 1);
    ql_object_unref(root);
    QL_CHECK_EQ(g_destroyed, 3);                      /* root + child1 + grand */

QL_TEST_END()
