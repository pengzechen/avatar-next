#ifndef X86_64_MEM_LAYOUT_H
#define X86_64_MEM_LAYOUT_H

/*
   include/x86_64/mem_layout.h — x86_64 内核高半区与物理加载基址

   Makefile 会以 -DX86_KERNEL_VMA_OFF=... / -DX86_KERNEL_LOAD_PHYS=...
   传入；link.ld 通过 --defsym=KERNEL_VMA_OFF / KERNEL_LOAD_PHYS 得到
   同一组值。这里的 fallback 仅为单独包含本头文件的场合保留。

   本文件只放 #define, 保证 .S 也能 include。
*/
#ifndef X86_KERNEL_VMA_OFF
#define X86_KERNEL_VMA_OFF 0xffff800000000000
#endif

/*
   物理加载基址 = RAM 基址 + lowmem 保留区(platform.conf 的
   lowmem_1m_2m)。QEMU/multiboot 把内核装在这里。
*/
#ifndef X86_KERNEL_LOAD_PHYS
#define X86_KERNEL_LOAD_PHYS 0x00200000
#endif

/* boot.S 里 multiboot 的虚拟地址 → 物理地址换算 */
#define X86_PHYS_OFFSET X86_KERNEL_VMA_OFF

#endif /* X86_64_MEM_LAYOUT_H */
