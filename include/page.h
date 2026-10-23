#ifndef PAGE_H
#define PAGE_H

/*
   include/page.h - 页大小的唯一定义

   以前 PAGE_SIZE / PAGE_SHIFT / PAGE_MASK 定义在 mm_vm.h 里, 于是 PMM
   只为了一个常量就得 include 整套用户虚拟内存 API(还会连带三架构页表头)。
   这里只放"页"这一层最基础的常量, mm_vm.h 和 pmm 都从这里取。
*/

#define PAGE_SIZE  4096
#define PAGE_SHIFT 12
#define PAGE_MASK  (~(PAGE_SIZE - 1))

#endif /* PAGE_H */
