/**
 * @file    qlprop.c
 * @brief   属性派发：沿继承链找到 id 区间所属的类。
 */
#include <qlgl/qlgl.h>

static QLObjectClass *find_owner(QLObject *obj, ql_property_t property)
{
    for (QLObjectClass *klass = obj->klass; klass != NULL; klass = klass->parent) {
        if (property >= klass->property_start && property <= klass->property_end)
            return klass;
    }
    return NULL;
}

ql_err_t ql_object_set_property(QLObject *obj, ql_property_t property, QLValue value)
{
    if (obj == NULL)
        return QL_ERR_INVAL;

    QLObjectClass *owner = find_owner(obj, property);
    if (owner == NULL || owner->set_property == NULL)
        return QL_ERR_INVAL;

    return owner->set_property(obj, property, value);
}

QLValue ql_object_get_property(QLObject *obj, ql_property_t property)
{
    if (obj == NULL)
        return QL_VALUE_NULL;

    QLObjectClass *owner = find_owner(obj, property);
    if (owner == NULL || owner->get_property == NULL)
        return QL_VALUE_NULL;

    return owner->get_property(obj, property);
}

int ql_object_property_owner(QLObject *obj, ql_property_t property, QLType *owner_type)
{
    if (obj == NULL)
        return 0;
    QLObjectClass *owner = find_owner(obj, property);
    if (owner == NULL)
        return 0;
    if (owner_type != NULL)
        *owner_type = owner->type;
    return 1;
}
