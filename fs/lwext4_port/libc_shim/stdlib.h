/*
 * fs/lwext4_port/libc_shim/stdlib.h - 为 lwext4 提供 stdlib.h 兼容层
 *
 * lwext4 通过 CONFIG_USE_USER_MALLOC=1 将 malloc/free 重定向到
 * ext4_user_malloc/ext4_user_free，因此此处只需声明这些函数。
 * 声明已在 generated/ext4_config.h 中提供。
 */
#ifndef _COMPAT_STDLIB_H_
#define _COMPAT_STDLIB_H_

#include <types.h>

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif /* _COMPAT_STDLIB_H_ */
