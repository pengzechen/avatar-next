# mk/vendor.mk — lwext4 / lwIP / FatFs 与 vendored 源码前置检查

# ─── §8  ext4 后端：lwext4（first-party）───────────────────────────────────────
LWEXT4_DIR      := $(FS_DIR)/ext4/lwext4
LWEXT4_PORT_DIR := $(FS_DIR)/ext4/port

# ─── §8a  内核网络栈：netdev + lwIP ─────────────────────────────────────────────
LWIP_DIR      := $(KERNEL_DIR)/net/lwip
LWIP_PORT_DIR := $(KERNEL_DIR)/net/lwip_port


LWIP_CORE_SRCS := $(LWIP_DIR)/src/core/init.c \
                  $(LWIP_DIR)/src/core/def.c \
                  $(LWIP_DIR)/src/core/dns.c \
                  $(LWIP_DIR)/src/core/inet_chksum.c \
                  $(LWIP_DIR)/src/core/ip.c \
                  $(LWIP_DIR)/src/core/mem.c \
                  $(LWIP_DIR)/src/core/memp.c \
                  $(LWIP_DIR)/src/core/netif.c \
                  $(LWIP_DIR)/src/core/pbuf.c \
                  $(LWIP_DIR)/src/core/raw.c \
                  $(LWIP_DIR)/src/core/stats.c \
                  $(LWIP_DIR)/src/core/sys.c \
                  $(LWIP_DIR)/src/core/tcp.c \
                  $(LWIP_DIR)/src/core/tcp_in.c \
                  $(LWIP_DIR)/src/core/tcp_out.c \
                  $(LWIP_DIR)/src/core/timeouts.c \
                  $(LWIP_DIR)/src/core/udp.c \
                  $(LWIP_DIR)/src/core/ipv4/etharp.c \
                  $(LWIP_DIR)/src/core/ipv4/icmp.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
                  $(LWIP_DIR)/src/core/ipv4/ip4_frag.c \
                  $(LWIP_DIR)/src/netif/ethernet.c
LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/core/%.c,$(VENDOR_BUILD_DIR)/lwip_core_%.o,$(filter $(LWIP_DIR)/src/core/%.c,$(filter-out $(LWIP_DIR)/src/core/ipv4/%,$(LWIP_CORE_SRCS))))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/core/ipv4/%.c,$(VENDOR_BUILD_DIR)/lwip_ipv4_%.o,$(filter $(LWIP_DIR)/src/core/ipv4/%.c,$(LWIP_CORE_SRCS)))
LWIP_OBJS += $(patsubst $(LWIP_DIR)/src/netif/%.c,$(VENDOR_BUILD_DIR)/lwip_netif_%.o,$(filter $(LWIP_DIR)/src/netif/%.c,$(LWIP_CORE_SRCS)))

LWIP_CFLAGS := $(CFLAGS)
LWIP_CFLAGS += -I$(LWIP_DIR)/src/include
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)
LWIP_CFLAGS += -I$(LWIP_PORT_DIR)/arch
LWIP_CFLAGS += -DLWIP_NO_CTYPE_H=1
LWIP_CFLAGS += -w

# lwext4 库源文件（第三方代码）
LWEXT4_SRCS     := $(wildcard $(LWEXT4_DIR)/src/*.c)
LWEXT4_OBJS     := $(patsubst $(LWEXT4_DIR)/src/%.c,$(VENDOR_BUILD_DIR)/lwext4_%.o,$(LWEXT4_SRCS))

# lwext4 移植胶水代码（属于本项目，使用 LWEXT4_CFLAGS）
# 注：driver/blk/ramblk.c 也是同一组标志，但它是驱动，归 §6e 的 DRIVER_OBJECTS。
LWEXT4_PORT_OBJS := $(BUILD_DIR)/lwext4_port_kmalloc.o \
                   $(BUILD_DIR)/lwext4_port_fs_lock.o

# lwext4 的全局串行化（详见 fs/ext4/port/fs_lock.c）。
#
# lwext4 是单线程用户态库，全局缓冲缓存无内部同步 —— SMP>1 时并发 exec
# 会把它的红黑树写坏。这里用**链接期符号包装**把所有公开 API 的引用改指向
# 我们的包装函数（进去先拿全局锁），真实实现仍可经 __real_* 调用。
#
# 为什么不直接在调用点加锁：内核里有 90+ 个调用点、22 个直接调用者中 15 个
# 是多出口的，手工配对 lock/unlock 必漏；而且粒度必须落在"每次 API 调用"
# 上（read_file_fully 要循环读 5.6MB，整函数持锁会关中断几十~几百毫秒）。
#
# ⚠️ 新增 lwext4 调用时，如果用到列表外的符号，**要把它加进来**，
#    否则那条路径不受保护。列表与 fs_lock.c 里的 FS_WRAP(...) 一一对应，
#    两处必须同时改。漏了包装不会报错（只是没锁），所以改 lwext4 调用时
#    顺手用 `grep -rhoE '\bext4_[a-z0-9_]+' kernel/ fs/ driver/` 对一遍。
LWEXT4_WRAP_SYMS := \
    ext4_block_fini ext4_block_init ext4_device_register ext4_device_unregister \
    ext4_dir_close ext4_dir_entry_next ext4_dir_mk ext4_dir_open ext4_dir_rm \
    ext4_fclose ext4_fopen ext4_fopen2 ext4_fread ext4_fremove ext4_frename \
    ext4_fseek ext4_fsize ext4_ftell ext4_ftruncate ext4_fwrite \
    ext4_inode_exist ext4_mode_get ext4_mount \
    ext4_raw_inode_fill ext4_readlink ext4_sb_read

LDFLAGS += $(foreach s,$(LWEXT4_WRAP_SYMS),-Wl,--wrap=$(s))

# lwext4 专用编译标志（在通用 CFLAGS 基础上添加）
LWEXT4_CFLAGS  := $(CFLAGS)
LWEXT4_CFLAGS  += -I$(FS_DIR)               # "vfs/vfs_internal.h"（跨文件引用）
LWEXT4_CFLAGS  += -I$(LWEXT4_DIR)/include   # lwext4 头文件
LWEXT4_CFLAGS  += -I$(LWEXT4_PORT_DIR)      # generated/ext4_config.h 所在目录
LWEXT4_CFLAGS  += -DCONFIG_USE_DEFAULT_CFG=0  # 使用自定义 ext4_config.h
# 以下定义与 generated/ext4_config.h 保持一致，防止默认值覆盖
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ERRNO=1
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_OFLAGS=1
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_PRINTF=0
LWEXT4_CFLAGS  += -DCONFIG_DEBUG_ASSERT=0
LWEXT4_CFLAGS  += -DCONFIG_HAVE_OWN_ASSERT=1
LWEXT4_CFLAGS  += -DCONFIG_USE_USER_MALLOC=1

# `-w` 只给 lwext4 自己的源码。**本项目的胶水代码（fs/ext4/port/*、
# driver/blk/ramblk.c）必须保持警告可见** —— 曾经这里是一刀切的 `-w`，
# 于是 fs_lock.c 里漏声明 `__real_ext4_*` （编译器按隐式声明当成返回 int）
# 这条致命警告被吞掉，最终表现成 aarch64 上指针被 `sxtw` 截断的
# "确定性崩溃"（详见 fs/ext4/port/fs_lock.c 顶部注释）。
LWEXT4_THIRD_PARTY_CFLAGS := $(LWEXT4_CFLAGS) -w

# ─── §8b  FAT 后端：FatFs（FAT12/16/32，first-party）───────────────────────────
# 源码在 fs/fat/fatfs/source（唯一 vendor patch 是 ffconf.h wrapper），
# 真正的配置在 fs/fat/port/ffconf.h，diskio 也在 fs/fat/port/。
FATFS_DIR      := $(FS_DIR)/fat/fatfs
FATFS_PORT_DIR := $(FS_DIR)/fat/port
FATFS_SRCS     := $(FATFS_DIR)/source/ff.c $(FATFS_DIR)/source/ffunicode.c
FATFS_OBJS     := $(patsubst $(FATFS_DIR)/source/%.c,$(VENDOR_BUILD_DIR)/fatfs_%.o,$(FATFS_SRCS))
FATFS_PORT_OBJS := $(BUILD_DIR)/fatfs_port_diskio.o
# FatFs 胶水需要 lwext4 的 ext4_blockdev 类型（共享 bdif），所以基于 LWEXT4_CFLAGS。
FATFS_CFLAGS   := $(LWEXT4_CFLAGS) -I$(FATFS_DIR)/source -I$(FATFS_PORT_DIR) -I$(FS_DIR)
FATFS_THIRD_PARTY_CFLAGS := $(FATFS_CFLAGS) -w

# ─── §9a  第三方源码前置检查 ─────────────────────────────────────────────────────
#
# lwext4、FatFs 与 lwIP 都**不再是 submodule**，而是内嵌在本仓库里
# 的源码副本。原因见各自的 README.md：lwext4 是因为我们 fork 并修改了它，而
# .gitmodules 指向上游、父仓库钉的却是 fork 的 commit —— 全新 clone 必失败；
# lwIP 是未修改的上游代码，一并内嵌以彻底去掉 "submodule 状态可能撒谎" 这个
# 失败面。
#
# 这个检查仍然保留：它现在挡的是**工作树不完整**（checkout 被中断、有人手工
# 删过内嵌源码）。那种情况下构建会死在一行极具误导性的错误上：
#
#     include/fs/vfs.h:14:10: fatal error: ext4.h: No such file or directory
#
# 这行指向的是**我们自己的**头文件，和真正的原因看不出关系 —— 照着它去查
# vfs.h 会白查很久。所以在任何编译发生之前先拦一道。
#
# 判据只能是文件本身，不能信版本控制的状态字段。历史教训：曾出现过
# "指针文件在、status 不带 '-'、update --init 还 exit 0，但工作树是空的"
# 这种状态，排查代价很高。
#
# 挂载点选在 KERNEL_RULE（见 §11b）而不是顶层的 kernel 目标：那样只能保证
# **链接前**检查，各 .o 的编译（也就是真正报 ext4.h 的地方）早就先炸了。
VENDOR_HEADERS := $(LWEXT4_DIR)/include/ext4.h \
                  $(LWIP_DIR)/src/include/lwip/init.h \
                  $(FATFS_DIR)/source/ff.h

# 兼容旧命令：本仓库不使用 submodule（lwIP 在 kernel/net/lwip，
# lwext4/FatFs 在 fs/）。保留空目标只为别让旧脚本/旧文档报错。
submodules:
	@echo "No submodule fetch is needed: lwIP lives under kernel/net/lwip, lwext4/FatFs under fs/."

check-submodules:
	@missing=""; \
	for h in $(VENDOR_HEADERS); do \
	    [ -e "$$h" ] || missing="$$missing $$h"; \
	done; \
	if [ -n "$$missing" ]; then \
	    echo ""; \
	    echo "════════════════════════════════════════════════════════════════"; \
	    echo " Required sources are missing (lwIP is under kernel/net/lwip; lwext4/FatFs under fs/)."; \
	    echo " The working tree is incomplete; please re-clone. Missing:"; \
	    for h in $$missing; do echo "     $$h"; done; \
	    echo ""; \
	    echo " Fix (run from the repository root):"; \
	    echo "     re-clone the repository (no submodules are used)"; \
	    echo "════════════════════════════════════════════════════════════════"; \
	    echo ""; \
	    exit 1; \
	fi
