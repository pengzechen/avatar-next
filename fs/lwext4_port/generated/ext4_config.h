/*
 * generated/ext4_config.h - Avatar OS lwext4 裸机移植配置
 *
 * 此文件在 CONFIG_USE_DEFAULT_CFG=0 时被 ext4_config.h 自动包含。
 * 无需手动修改 lwext4 源码。
 */

/* ── 使用自定义 errno 定义（不依赖 errno.h）────────────────────── */
#define CONFIG_HAVE_OWN_ERRNO 1

/* ── 使用自定义文件标志（不依赖 fcntl.h）────────────────────────── */
#define CONFIG_HAVE_OWN_OFLAGS 1

/* ── 禁用调试 printf（不依赖 stdio.h）───────────────────────────── */
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0

/* ── 使用自定义断言（不依赖 assert.h）───────────────────────────── */
#define CONFIG_HAVE_OWN_ASSERT 1

/* ── 使用自定义内存分配器（不依赖 malloc/free）─────────────────── */
#define CONFIG_USE_USER_MALLOC 1

/* ── 前向声明自定义内存分配函数 ──────────────────────────────────
 * 使用 GCC 内置类型 __SIZE_TYPE__ 避免对 stddef.h 的依赖。
 */
void *ext4_user_malloc(__SIZE_TYPE__ size);
void *ext4_user_calloc(__SIZE_TYPE__ nmemb, __SIZE_TYPE__ size);
void *ext4_user_realloc(void *ptr, __SIZE_TYPE__ size);
void ext4_user_free(void *ptr);

/* ── 文件系统功能特性 ───────────────────────────────────────────── */
#define CONFIG_EXT_FEATURE_SET_LVL 4 /* ext4 */
#define CONFIG_JOURNALING_ENABLE   1
/* 0：xattr 支持已整块移除。原实现 ext4_xattr.c 是 lwext4 里两个
 * GPLv2 文件之一（另一个是 ext4_extent.c，已重写）。本内核不暴露
 * xattr API，摘掉它才能让 lwext4 变成纯 BSD-3-Clause。 */
#define CONFIG_XATTR_ENABLE        0
#define CONFIG_EXTENTS_ENABLE      1

/* ── 块设备和挂载点限制 ─────────────────────────────────────────── */
#define CONFIG_BLOCK_DEV_CACHE_SIZE   8
#define CONFIG_EXT4_BLOCKDEVS_COUNT   2
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 2
#define CONFIG_EXT4_MAX_BLOCKDEV_NAME 32
#define CONFIG_EXT4_MAX_MP_NAME       32

/* ── 统计功能（可选）────────────────────────────────────────────── */
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0

/* ── 对齐访问（64 位架构支持非对齐访问）────────────────────────── */
#define CONFIG_UNALIGNED_ACCESS 1
