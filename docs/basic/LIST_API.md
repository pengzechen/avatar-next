# 双向链表库

## 概述

轻量级双向链表实现，所有函数都是内联的，无链接冲突。

## 数据结构

```c
/* 链表节点 */
typedef struct _list_node_t {
    struct _list_node_t *pre;   /* 前驱指针 */
    struct _list_node_t *next; /* 后继指针 */
} list_node_t;

/* 链表头 */
typedef struct _list_t {
    list_node_t *first; /* 首节点 */
    list_node_t *last;  /* 尾节点 */
    int32_t      count; /* 节点计数 */
} list_t;
```

## API 文档

### 节点操作

#### list_node_init
```c
void list_node_init(list_node_t *node);
```
初始化链表节点，将 pre 和 next 设置为 NULL。

#### list_node_pre
```c
list_node_t *list_node_pre(list_node_t *node);
```
获取节点的前驱节点。

#### list_node_next
```c
list_node_t *list_node_next(list_node_t *node);
```
获取节点的后继节点。

### 链表查询

#### list_is_empty
```c
int32_t list_is_empty(list_t *list);
```
检查链表是否为空。

#### list_count
```c
int32_t list_count(list_t *list);
```
获取链表中的节点数量。

#### list_first
```c
list_node_t *list_first(list_t *list);
```
获取链表的首节点。

#### list_last
```c
list_node_t *list_last(list_t *list);
```
获取链表的尾节点。

#### list_contains
```c
int32_t list_contains(list_t *list, list_node_t *node);
```
检查指定节点是否在链表中。

### 链表修改

#### list_init
```c
void list_init(list_t *list);
```
初始化链表，清空所有节点。

#### list_insert_first
```c
void list_insert_first(list_t *list, list_node_t *node);
```
在链表头部插入节点。

#### list_insert_last
```c
void list_insert_last(list_t *list, list_node_t *node);
```
在链表尾部插入节点。

#### list_delete_first
```c
list_node_t *list_delete_first(list_t *list);
```
删除链表的首节点，返回被删除的节点。

#### list_delete
```c
list_node_t *list_delete(list_t *list, list_node_t *node);
```
从链表中删除指定节点，返回被删除的节点。

### 工具宏

#### offset_in_parent
```c
#define offset_in_parent(parent_type, node_name)
```
计算结构体中成员的偏移量。

#### parent_addr
```c
#define parent_addr(node, parent_type, node_name)
```
根据成员地址获取父结构体地址。

#### list_node_parent
```c
#define list_node_parent(node, parent_type, node_name)
```
从链表节点获取父结构体指针。

## 使用示例

### 基本用法

```c
#include "list.h"

typedef struct {
    int         id;
    list_node_t node;  /* 必须包含 list_node_t */
    char        name[32];
} my_data_t;

/* 1. 初始化链表 */
list_t list;
list_init(&list);

/* 2. 准备数据 */
my_data_t data1, data2;
data1.id = 1;
data2.id = 2;
list_node_init(&data1.node);
list_node_init(&data2.node);

/* 3. 插入节点 */
list_insert_last(&list, &data1.node);
list_insert_last(&list, &data2.node);

/* 4. 遍历链表 */
list_node_t *node = list_first(&list);
while (node != (list_node_t *)0) {
    my_data_t *data = list_node_parent(node, my_data_t, node);
    printf("ID: %d\n", data->id);
    node = list_node_next(node);
}

/* 5. 删除节点 */
list_delete(&list, &data1.node);
```

### FIFO 队列

```c
/* 入队 */
void enqueue(list_t *queue, list_node_t *node)
{
    list_insert_last(queue, node);
}

/* 出队 */
list_node_t *dequeue(list_t *queue)
{
    return list_delete_first(queue);
}
```

### LIFO 栈

```c
/* 入栈 */
void push(list_t *stack, list_node_t *node)
{
    list_insert_first(stack, node);
}

/* 出栈 */
list_node_t *pop(list_t *stack)
{
    return list_delete_first(stack);
}
```

### 双端队列

```c
/* 头部插入 */
void push_front(list_t *deque, list_node_t *node)
{
    list_insert_first(deque, node);
}

/* 尾部插入 */
void push_back(list_t *deque, list_node_t *node)
{
    list_insert_last(deque, node);
}

/* 头部删除 */
list_node_t *pop_front(list_t *deque)
{
    return list_delete_first(deque);
}

/* 尾部删除 */
list_node_t *pop_back(list_t *deque)
{
    return list_delete(deque, list_last(deque));
}
```

## 特性

### 优势

1. **零依赖**：只依赖 types.h
2. **内联函数**：所有函数都是 `static inline`，无链接冲突
3. **类型安全**：使用强类型检查
4. **高效**：无函数调用开销
5. **灵活**：支持嵌入式到任何数据结构

### 注意事项

1. **节点必须初始化**：使用 `list_node_init()` 初始化节点
2. **不在链表中**：节点只能在一个链表中
3. **手动管理内存**：不负责分配/释放内存
4. **线程不安全**：多线程访问需要外部加锁

## 测试

运行 [examples/list_test.c](examples/list_test.c) 中的测试：

```bash
make ARCH=x86_64   # 包含 list_test.o
make ARCH=aarch64
make ARCH=riscv64
```

测试覆盖：
- ✅ 链表初始化
- ✅ 节点初始化
- ✅ 头部/尾部插入
- ✅ 删除操作
- ✅ 空链表处理
- ✅ 包含检查
- ✅ 父结构体访问
- ✅ 链表遍历

## 性能

所有操作的时间复杂度：

| 操作 | 时间复杂度 |
|------|-----------|
| 初始化 | O(1) |
| 头部插入 | O(1) |
| 尾部插入 | O(1) |
| 头部删除 | O(1) |
| 指定删除 | O(1) |
| 包含检查 | O(n) |
| 遍历 | O(n) |

## 内存布局

```
list_t 结构:
┌─────────────┐
│ first    ─┼──→ node1 ↔ node2 ↔ node3
│ last     ─┼──────────────────────→ (指向最后一个节点)
│ count   = 3
└─────────────┘

list_node_t 结构:
┌──────────────┐
│ pre      ◀──┼── (前驱节点)
│ next    ────┼──→ (后继节点)
└──────────────┘
```

## 扩展性

可以轻松扩展为：
- 循环链表
- 跳表（Skip List）
- 优先级队列
- 线程安全包装器
