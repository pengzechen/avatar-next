/*
 * include/elf.h - ELF 文件格式定义
 */

#ifndef ELF_H
#define ELF_H

#include "types.h"
#include "arch.h"

/* ELF 头标识 */
#define EI_NIDENT 16

/* ELF 类别 */
#define ELFCLASS64 2

/* ELF 数据编码 */
#define ELFDATA2LSB 1  /* 小端序 */

/* ELF 版本 */
#define EV_CURRENT 1

/* ELF 机器类型 */
#define EM_AARCH64 183

/* ELF 文件类型 */
#define ET_NONE   0
#define ET_REL    1  /* 可重定位文件 */
#define ET_EXEC   2  /* 可执行文件 */
#define ET_DYN    3  /* 共享对象 */

/* ELF 程序头类型 */
#define PT_NULL    0
#define PT_LOAD    1  /* 可加载段 */
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_TLS     7  /* 线程本地存储 */

/* ELF 段权限 */
#define PF_X 0x1  /* 可执行 */
#define PF_W 0x2  /* 可写 */
#define PF_R 0x4  /* 可读 */

/* DT_* 动态标签 */
#define DT_NULL    0
#define DT_RELA    7   /* RELA 重定位表地址 */
#define DT_RELASZ  8   /* RELA 重定位表大小 */
#define DT_RELAENT 9   /* RELA 条目大小 */

/* AArch64 重定位类型 */
#define R_AARCH64_RELATIVE 1027  /* 0x403: base + addend */

/* 64-bit ELF 动态段条目 */
typedef struct {
    int64_t  d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} elf64_dyn_t;

/* 64-bit ELF RELA 重定位条目 */
typedef struct {
    uint64_t r_offset;  /* 需要重定位的虚拟地址 */
    uint64_t r_info;    /* 符号索引 + 类型 */
    int64_t  r_addend;  /* 加数 */
} elf64_rela_t;

#define ELF64_R_TYPE(info)  ((uint32_t)(info))

/* 64-bit ELF 头 */
typedef struct {
    uint8_t  e_ident[EI_NIDENT]; /* 魔数和其他信息 */
    uint16_t e_type;             /* 文件类型 */
    uint16_t e_machine;          /* 机器类型 */
    uint32_t e_version;          /* 版本 */
    uint64_t e_entry;            /* 入口点虚拟地址 */
    uint64_t e_phoff;            /* 程序头表文件偏移 */
    uint64_t e_shoff;            /* 节头表文件偏移 */
    uint32_t e_flags;            /* 处理器特定标志 */
    uint16_t e_ehsize;           /* ELF 头大小 */
    uint16_t e_phentsize;        /* 程序头表条目大小 */
    uint16_t e_phnum;            /* 程序头表条目数量 */
    uint16_t e_shentsize;        /* 节头表条目大小 */
    uint16_t e_shnum;            /* 节头表条目数量 */
    uint16_t e_shstrndx;         /* 节头字符串表索引 */
} elf64_ehdr_t;

/* 64-bit ELF 程序头 */
typedef struct {
    uint32_t p_type;   /* 段类型 */
    uint32_t p_flags;  /* 段标志 */
    uint64_t p_offset; /* 段在文件中的偏移 */
    uint64_t p_vaddr;  /* 段的虚拟地址 */
    uint64_t p_paddr;  /* 段的物理地址（未使用） */
    uint64_t p_filesz; /* 段在文件中的大小 */
    uint64_t p_memsz;  /* 段在内存中的大小 */
    uint64_t p_align;  /* 段对齐 */
} elf64_phdr_t;

/* ELF 魔数 */
#define ELF_MAGIC "\x7f\x45\x4c\x46"

/* 工具函数 */
static inline int elf_check_magic(elf64_ehdr_t *ehdr)
{
    return ehdr->e_ident[0] == 0x7f &&
           ehdr->e_ident[1] == 'E' &&
           ehdr->e_ident[2] == 'L' &&
           ehdr->e_ident[3] == 'F';
}

static inline int elf_check_class(elf64_ehdr_t *ehdr)
{
    return ehdr->e_ident[4] == ELFCLASS64;
}

static inline int elf_check_data(elf64_ehdr_t *ehdr)
{
    return ehdr->e_ident[5] == ELFDATA2LSB;
}

static inline int elf_check_machine(elf64_ehdr_t *ehdr)
{
#if ARCH_AARCH64
    return ehdr->e_machine == EM_AARCH64;
#elif defined(__x86_64__)
    return ehdr->e_machine == 62;  /* EM_X86_64 */
#elif defined(__riscv) && (__riscv_xlen == 64)
    return ehdr->e_machine == 243; /* EM_RISCV */
#else
    return 0;
#endif
}

#endif /* ELF_H */
