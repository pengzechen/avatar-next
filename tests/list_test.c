/*
 * 链表功能测试
 * 编译: gcc -D__x86_64__ -I../include -c list_test.c
 */

#include "list.h"
#include "types.h"

/* 测试用的数据结构 */
typedef struct {
    int         id;
    list_node_t node;
    char        name[32];
} test_data_t;

/* 测试计数器 */
static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(condition, msg) \
    do { \
        if (condition) { \
            test_passed++; \
        } else { \
            test_failed++; \
            /* printf("FAIL: %s\n", msg); */ \
        } \
    } while (0)

/* 测试 1: 链表初始化 */
static void
test_list_init(void)
{
    list_t list;
    list_init(&list);

    TEST_ASSERT(list.first == (list_node_t *)0, "list.first should be NULL");
    TEST_ASSERT(list.last == (list_node_t *)0, "list.last should be NULL");
    TEST_ASSERT(list.count == 0, "list.count should be 0");
}

/* 测试 2: 节点初始化 */
static void
test_node_init(void)
{
    list_node_t node;
    list_node_init(&node);

    TEST_ASSERT(node.pre == (list_node_t *)0, "node.pre should be NULL");
    TEST_ASSERT(node.next == (list_node_t *)0, "node.next should be NULL");
}

/* 测试 3: 插入首节点 */
static void
test_insert_first(void)
{
    list_t list;
    test_data_t data1, data2, data3;

    list_init(&list);
    list_node_init(&data1.node);
    list_node_init(&data2.node);
    list_node_init(&data3.node);

    list_insert_first(&list, &data1.node);
    TEST_ASSERT(list_count(&list) == 1, "count should be 1");
    TEST_ASSERT(list_first(&list) == &data1.node, "first should be data1");
    TEST_ASSERT(list_last(&list) == &data1.node, "last should be data1");

    list_insert_first(&list, &data2.node);
    TEST_ASSERT(list_count(&list) == 2, "count should be 2");
    TEST_ASSERT(list_first(&list) == &data2.node, "first should be data2");
    TEST_ASSERT(list_last(&list) == &data1.node, "last should be data1");
    TEST_ASSERT(list_node_next(&data2.node) == &data1.node, "data2.next should be data1");

    list_insert_first(&list, &data3.node);
    TEST_ASSERT(list_count(&list) == 3, "count should be 3");
    TEST_ASSERT(list_first(&list) == &data3.node, "first should be data3");
    TEST_ASSERT(list_node_next(&data3.node) == &data2.node, "data3.next should be data2");
}

/* 测试 4: 插入尾节点 */
static void
test_insert_last(void)
{
    list_t list;
    test_data_t data1, data2;

    list_init(&list);
    list_node_init(&data1.node);
    list_node_init(&data2.node);

    list_insert_last(&list, &data1.node);
    TEST_ASSERT(list_count(&list) == 1, "count should be 1");

    list_insert_last(&list, &data2.node);
    TEST_ASSERT(list_count(&list) == 2, "count should be 2");
    TEST_ASSERT(list_first(&list) == &data1.node, "first should be data1");
    TEST_ASSERT(list_last(&list) == &data2.node, "last should be data2");
}

/* 测试 5: 删除首节点 */
static void
test_delete_first(void)
{
    list_t list;
    test_data_t data1, data2, data3;

    list_init(&list);
    list_node_init(&data1.node);
    list_node_init(&data2.node);
    list_node_init(&data3.node);

    list_insert_last(&list, &data1.node);
    list_insert_last(&list, &data2.node);
    list_insert_last(&list, &data3.node);

    list_node_t *removed = list_delete_first(&list);
    TEST_ASSERT(removed == &data1.node, "removed should be data1");
    TEST_ASSERT(list_count(&list) == 2, "count should be 2");
    TEST_ASSERT(list_first(&list) == &data2.node, "new first should be data2");
    TEST_ASSERT(removed->pre == (list_node_t *)0, "removed.pre should be NULL");
    TEST_ASSERT(removed->next == (list_node_t *)0, "removed.next should be NULL");
}

/* 测试 6: 删除指定节点 */
static void
test_delete(void)
{
    list_t list;
    test_data_t data1, data2, data3;

    list_init(&list);
    list_node_init(&data1.node);
    list_node_init(&data2.node);
    list_node_init(&data3.node);

    list_insert_last(&list, &data1.node);
    list_insert_last(&list, &data2.node);
    list_insert_last(&list, &data3.node);

    list_node_t *removed = list_delete(&list, &data2.node);
    TEST_ASSERT(removed == &data2.node, "removed should be data2");
    TEST_ASSERT(list_count(&list) == 2, "count should be 2");
    TEST_ASSERT(list_node_next(&data1.node) == &data3.node, "data1.next should be data3");
    TEST_ASSERT(list_node_pre(&data3.node) == &data1.node, "data3.pre should be data1");
}

/* 测试 7: 空链表删除 */
static void
test_delete_empty(void)
{
    list_t list;
    list_init(&list);

    list_node_t *removed = list_delete_first(&list);
    TEST_ASSERT(removed == (list_node_t *)0, "should return NULL");
}

/* 测试 8: 链表包含检查 */
static void
test_contains(void)
{
    list_t list;
    test_data_t data1, data2;
    list_node_t dummy;

    list_init(&list);
    list_node_init(&data1.node);
    list_node_init(&data2.node);
    list_node_init(&dummy);

    list_insert_last(&list, &data1.node);
    list_insert_last(&list, &data2.node);

    TEST_ASSERT(list_contains(&list, &data1.node) == 1, "should contain data1");
    TEST_ASSERT(list_contains(&list, &data2.node) == 1, "should contain data2");
    TEST_ASSERT(list_contains(&list, &dummy) == 0, "should not contain dummy");
}

/* 测试 9: 父结构体访问 */
static void
test_parent_access(void)
{
    test_data_t data;
    data.id = 42;

    list_node_t *node = &data.node;
    test_data_t *parent = list_node_parent(node, test_data_t, node);

    TEST_ASSERT(parent == &data, "parent should be &data");
    TEST_ASSERT(parent->id == 42, "parent->id should be 42");
}

/* 测试 10: 链表遍历 */
static void
test_traversal(void)
{
    list_t list;
    test_data_t data[5];
    int32_t count = 0;

    list_init(&list);

    for (int i = 0; i < 5; i++) {
        data[i].id = i;
        list_node_init(&data[i].node);
        list_insert_last(&list, &data[i].node);
    }

    /* 正向遍历 */
    list_node_t *node = list_first(&list);
    int32_t expected = 0;
    while (node != (list_node_t *)0) {
        test_data_t *item = list_node_parent(node, test_data_t, node);
        TEST_ASSERT(item->id == expected, "traversal order should match");
        node = list_node_next(node);
        expected++;
        count++;
    }
    TEST_ASSERT(count == 5, "should traverse 5 nodes");
}

/* 运行所有测试 */
void
run_list_tests(void)
{
    test_list_init();
    test_node_init();
    test_insert_first();
    test_insert_last();
    test_delete_first();
    test_delete();
    test_delete_empty();
    test_contains();
    test_parent_access();
    test_traversal();

    /* 所有测试完成 */
    (void)test_passed;
    (void)test_failed;
}
