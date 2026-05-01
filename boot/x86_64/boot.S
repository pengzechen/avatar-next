/*
 * x86_64 boot code for QEMU Q35 machine
 *
 * Boot protocol: Multiboot1
 *   - QEMU delivers control in 32-bit protected mode
 *   - We set up GDT, page tables, then transition to 64-bit long mode
 *   - Finally jump to high-half virtual address and call kernel_main
 *
 * Memory layout:
 *   Physical base : 0x00200000  (2 MB)
 *   Virtual  base : 0xffff8000_00200000
 *   Offset        : 0xffff8000_00000000
 *
 * Multiboot flags: bit 1 (memory map) + bit 16 (address fields)
 *   With bit 16 the loader uses our explicit physical load address,
 *   not the ELF program-header addresses.
 */

/* Physical-to-virtual offset */
.set PHYS_OFFSET,   0xffff800000000000

/* Multiboot1 constants */
.set MB_MAGIC,      0x1BADB002
.set MB_FLAGS,      0x00010002          /* bit 1: mem-info | bit 16: addr-fields */
.set MB_CKSUM,      -(MB_MAGIC + MB_FLAGS)

/* CR0 / CR4 / EFER bits */
.set CR0_PE,        (1 << 0)
.set CR0_PG,        (1 << 31)
.set CR4_PAE,       (1 << 5)
.set CR4_PGE,       (1 << 7)
.set EFER_MSR,      0xC0000080
.set EFER_LME,      (1 << 8)
.set EFER_NXE,      (1 << 11)

/* ======================================================
 * Section: .text.boot
 *   All boot code/data that must appear near the start
 *   of the ELF image so QEMU finds the multiboot magic
 *   within the first 8 KB of the first LOAD segment.
 * ====================================================== */
.section .text.boot

/* ----------------------------------------------------------
 * 32-bit entry point  (multiboot calls _start in 32-bit PM)
 * ---------------------------------------------------------- */
.code32
.global _start
.type   _start, @function
_start:
    /* Save multiboot magic / info pointer (not used yet) */
    mov     %eax, %edi
    mov     %ebx, %esi
    jmp     boot32_entry

/* ----------------------------------------------------------
 * Multiboot1 header (within first 8 KB, 4-byte aligned)
 * Bit 16 is set so the loader uses our physical addresses
 * instead of ELF program-header virtual addresses.
 * ---------------------------------------------------------- */
.align 4
multiboot_header:
    .long   MB_MAGIC
    .long   MB_FLAGS
    .long   MB_CKSUM
    /* Address fields required by bit 16 -- all PHYSICAL */
    .long   multiboot_header  - PHYS_OFFSET     /* header_addr  */
    .long   _skernel          - PHYS_OFFSET     /* load_addr    */
    .long   _edata            - PHYS_OFFSET     /* load_end_addr*/
    .long   _ebss             - PHYS_OFFSET     /* bss_end_addr */
    .long   _start            - PHYS_OFFSET     /* entry_addr   */

/* ----------------------------------------------------------
 * 32-bit setup: GDT -> page tables -> long mode -> far jump
 * ---------------------------------------------------------- */
.code32
boot32_entry:
    cli

    /* Load our GDT FIRST (physical address) */
    movl    $(boot_gdt_desc - PHYS_OFFSET), %eax
    lgdt    (%eax)

    /* Now reload data segment registers with our GDT data selector */
    mov     $0x18, %ax
    mov     %ax,   %ss
    mov     %ax,   %ds
    mov     %ax,   %es
    mov     %ax,   %fs
    mov     %ax,   %gs

    /* Enable PAE and PGE in CR4 */
    mov     $(CR4_PAE | CR4_PGE), %eax
    mov     %eax, %cr4

    /* Load page-table root (physical address into CR3) */
    movl    $(boot_pml4 - PHYS_OFFSET), %eax
    mov     %eax, %cr3

    /* Enable long mode (LME) and NX (NXE) in IA32_EFER */
    mov     $EFER_MSR, %ecx
    rdmsr
    or      $(EFER_LME | EFER_NXE), %eax
    wrmsr

    /* Enable protected mode + paging in CR0 */
    mov     %cr0, %eax
    or      $(CR0_PE | CR0_PG), %eax
    mov     %eax, %cr0

    /* Far jump into 64-bit code segment (GDT selector 0x10) */
    ljmp    $0x10, $(boot64_entry - PHYS_OFFSET)

.size _start, . - _start

/* ----------------------------------------------------------
 * 64-bit entry  (still running at physical address)
 * ---------------------------------------------------------- */
.code64
.global boot64_entry
boot64_entry:
    /* Clear segment registers (not needed in 64-bit but tidy) */
    xor     %ax, %ax
    mov     %ax, %ss
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    /* Jump to virtual (high-half) address */
    movabs  $boot64_virt, %rax
    jmp     *%rax

boot64_virt:
    /* Set up stack at virtual address */
    movabs  $boot_stack_top, %rsp

    /* Clear BSS */
    cld
    movabs  $__bss_start, %rdi
    movabs  $__bss_end,   %rcx
    sub     %rdi, %rcx
    shr     $3,   %rcx
    xor     %eax, %eax
    rep stosq

    /* Jump to kernel */
    call    kernel_main

.Lhang:
    hlt
    jmp     .Lhang

/* ----------------------------------------------------------
 * Temporary GDT
 *   0x00 – null
 *   0x08 – 32-bit code (for completeness)
 *   0x10 – 64-bit code  (L=1, used by far-jump)
 *   0x18 – 32/64 data
 * ---------------------------------------------------------- */
.align 16
boot_gdt:
    .quad   0x0000000000000000   /* 0x00: null                              */
    .quad   0x00cf9b000000ffff   /* 0x08: code32  base=0 limit=4G DPL=0     */
    .quad   0x00af9b000000ffff   /* 0x10: code64  L=1   limit=4G DPL=0     */
    .quad   0x00cf93000000ffff   /* 0x18: data    base=0 limit=4G DPL=0     */
boot_gdt_end:

.align 4
boot_gdt_desc:
    .word   boot_gdt_end - boot_gdt - 1   /* limit                          */
    .long   boot_gdt - PHYS_OFFSET        /* base  (32-bit physical address) */

/* ----------------------------------------------------------
 * Temporary page tables  (embedded in .text.boot so they
 * land inside the first LOAD segment near the start of the
 * file – the multiboot header is already there too).
 *
 * Layout (all 1 GB huge pages, PS bit set):
 *   PML4[0]   -> pdpt_low   maps 0x0000_0000_0000 .. 0x0080_0000_0000 (identity)
 *   PML4[256] -> pdpt_high  maps 0xffff_8000_0000 .. 0xffff_8080_0000 -> same phys
 * ---------------------------------------------------------- */
.align 4096
boot_pml4:
    .quad   (boot_pdpt_low  - PHYS_OFFSET) + 0x3    /* entry  0: lower half */
    .zero   8 * 255
    .quad   (boot_pdpt_high - PHYS_OFFSET) + 0x3    /* entry 256: upper half */
    .zero   8 * 255

.align 4096
boot_pdpt_low:
.set i, 0
.rept 512
    .quad   (0x40000000 * i) | 0x83     /* 1 GB huge page: P+RW+PS */
    .set    i, i + 1
.endr

.align 4096
boot_pdpt_high:
.set i, 0
.rept 512
    .quad   (0x40000000 * i) | 0x83     /* 1 GB huge page: P+RW+PS */
    .set    i, i + 1
.endr

/* ----------------------------------------------------------
 * Boot stack  (BSS so the ELF does not carry 16 KB of zeros)
 * ---------------------------------------------------------- */
.section .bss
.align 16
.global boot_stack_bottom
.global boot_stack_top
boot_stack_bottom:
    .space  16384               /* 16 KB */
boot_stack_top:
