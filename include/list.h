#ifndef LIST_H
#define LIST_H

#include "types.h"

/*
 * 双向链表实现
 * 所有函数都是内联的，无链接冲突
 */

typedef struct _list_node_t
{
    struct _list_node_t *pre;
    struct _list_node_t *next;
} list_node_t;

typedef struct _list_t
{
    list_node_t *first;
    list_node_t *last;
    int32_t      count;
} list_t;

/* ===== 节点操作 ===== */

/**
 * list_node_init - 初始化链表节点
 * @node: 节点指针
 */
static inline void
list_node_init(list_node_t *node)
{
    node->pre = node->next = (list_node_t *)0;
}

/**
 * list_node_pre - 获取节点的前驱
 * @node: 节点指针
 *
 * 返回值: 前驱节点指针
 */
static inline list_node_t *
list_node_pre(list_node_t *node)
{
    return node->pre;
}

/**
 * list_node_next - 获取节点的后继
 * @node: 节点指针
 *
 * 返回值: 后继节点指针
 */
static inline list_node_t *
list_node_next(list_node_t *node)
{
    return node->next;
}

/* ===== 链表查询操作 ===== */

/**
 * list_is_empty - 检查链表是否为空
 * @list: 链表指针
 *
 * 返回值: 1 表示空，0 表示非空
 */
static inline int32_t
list_is_empty(list_t *list)
{
    return list->count == 0;
}

/**
 * list_count - 获取链表节点数量
 * @list: 链表指针
 *
 * 返回值: 节点数量
 */
static inline int32_t
list_count(list_t *list)
{
    return list->count;
}

/**
 * list_first - 获取链表首节点
 * @list: 链表指针
 *
 * 返回值: 首节点指针，空链表返回 NULL
 */
static inline list_node_t *
list_first(list_t *list)
{
    return list->first;
}

/**
 * list_last - 获取链表尾节点
 * @list: 链表指针
 *
 * 返回值: 尾节点指针，空链表返回 NULL
 */
static inline list_node_t *
list_last(list_t *list)
{
    return list->last;
}

/* ===== 链表修改操作 ===== */

/**
 * list_init - 初始化链表
 * @list: 链表指针
 */
static inline void
list_init(list_t *list)
{
    list->first = list->last = (list_node_t *)0;
    list->count              = 0;
}

/**
 * list_insert_first - 在链表头部插入节点
 * @list: 链表指针
 * @node: 要插入的节点（必须已初始化）
 */
static inline void
list_insert_first(list_t *list, list_node_t *node)
{
    node->next = list->first;
    node->pre  = (list_node_t *)0;

    if (list->count == 0) {
        list->last = list->first = node;
    } else {
        list->first->pre = node;
        list->first      = node;
    }

    list->count++;
}

/**
 * list_insert_last - 在链表尾部插入节点
 * @list: 链表指针
 * @node: 要插入的节点（必须已初始化，且不在链表中）
 */
static inline void
list_insert_last(list_t *list, list_node_t *node)
{
    node->pre  = list->last;
    node->next = (list_node_t *)0;

    if (list->count == 0) {
        list->first = list->last = node;
    } else {
        list->last->next = node;
        list->last       = node;
    }

    list->count++;
}

/**
 * list_delete_first - 删除链表首节点
 * @list: 链表指针
 *
 * 返回值: 被删除的节点指针，空链表返回 NULL
 */
static inline list_node_t *
list_delete_first(list_t *list)
{
    if (list->count == 0) {
        return (list_node_t *)0;
    }

    list_node_t *remove_node = list->first;
    list->first              = remove_node->next;

    if (list->first == (list_node_t *)0) {
        list->last = (list_node_t *)0;
    } else {
        list->first->pre = (list_node_t *)0;
    }

    remove_node->pre = remove_node->next = (list_node_t *)0;
    list->count--;

    return remove_node;
}

/**
 * list_delete - 从链表中删除指定节点
 * @list: 链表指针
 * @node: 要删除的节点
 *
 * 返回值: 被删除的节点指针
 */
static inline list_node_t *
list_delete(list_t *list, list_node_t *node)
{
    if (node->pre) {
        node->pre->next = node->next;
    } else {
        list->first = node->next;
    }

    if (node->next) {
        node->next->pre = node->pre;
    } else {
        list->last = node->pre;
    }

    node->pre = node->next = (list_node_t *)0;
    list->count--;

    return node;
}

/**
 * list_contains - 检查节点是否在链表中
 * @list: 链表指针
 * @node: 要检查的节点
 *
 * 返回值: 1 表示在链表中，0 表示不在
 */
static inline int32_t
list_contains(list_t *list, list_node_t *node)
{
    list_node_t *cur = list->first;
    while (cur != (list_node_t *)0) {
        if (cur == node)
            return 1;
        cur = cur->next;
    }
    return 0;
}

/* ===== 宏工具函数 ===== */

/**
 * offset_in_parent - 计算结构体中成员的偏移量
 * @parent_type: 父结构体类型
 * @node_name: 成员名称
 *
 * 返回值: 成员在结构体中的字节偏移量
 */
#define offset_in_parent(parent_type, node_name) \
    ((uint64_t) & (((parent_type *)0)->node_name))

/**
 * parent_addr - 根据成员地址获取父结构体地址
 * @node: 成员指针
 * @parent_type: 父结构体类型
 * @node_name: 成员名称
 *
 * 返回值: 父结构体指针
 */
#define parent_addr(node, parent_type, node_name) \
    ((uint64_t)node - offset_in_parent(parent_type, node_name))

/**
 * list_node_parent - 从链表节点获取父结构体
 * @node: 链表节点指针
 * @parent_type: 父结构体类型
 * @node_name: 链表节点在父结构体中的成员名
 *
 * 返回值: 父结构体指针，如果 node 为 NULL 则返回 NULL
 */
#define list_node_parent(node, parent_type, node_name) \
    ((parent_type *)(node ? parent_addr(node, parent_type, node_name) : 0))

#endif /* LIST_H */
