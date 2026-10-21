#ifndef SHARED_PAGE_H
#define SHARED_PAGE_H

#include "types.h"

/*
 * MAP_SHARED 物理页的引用计数。
 *
 * PTE_NOFREE 防止 fork 子进程释放共享页，但当所有持有者都退出后，需要有
 * 人释放物理页。此模块为每个 NOFREE 页维护引用计数，最后一个 unref 真正
 * 把页还给 PMM。
 *
 * shared_page_ref() 返回 false 表示没有记账能力（表满 / 参数非法）。
 * 调用方必须在页表里留下 NOFREE 标记**之前**处理这个失败：把这次映射
 * 回滚掉。否则 unmap 时找不到账本，页会被当成普通共享页处理，最终要么
 * 泄漏、要么把别人还在用的页释放掉。
 */
bool shared_page_ref(uint64_t paddr);
void shared_page_unref(uint64_t paddr);

#endif /* SHARED_PAGE_H */
