/*
 * fs/lwext4_port/fs_lock.h - lwext4 全局串行化锁的跨模块接口
 *
 * 每次 lwext4 API 调用那层锁由 `-Wl,--wrap` 自动加上（见 fs_lock.c），
 * 内核侧**不需要**手工配对。
 *
 * 这两个函数只给一种场景用：**跨多次 lwext4 调用的临界区**。典型是目录
 * 迭代 —— `ext4_dir_entry_next` 把条目拷进 `ext4_dir`（内联在
 * `vfs_file_t` 里）并返回它的地址，调用方在循环里一直要用；只锁单次调用
 * 挡不住两次调用之间别的核对同一个目录对象做操作。
 *
 * 锁可重入（按 CPU 记 owner + 深度），所以外面套一层粗锁、里面那层
 * 每次调用的细锁照常嵌套，不会自死锁。持锁期间会关本核中断，**临界区
 * 里不要做长耗时的事**。
 */
#ifndef LWEXT4_PORT_FS_LOCK_H
#define LWEXT4_PORT_FS_LOCK_H

void fs_lwext4_lock(void);
void fs_lwext4_unlock(void);

#endif /* LWEXT4_PORT_FS_LOCK_H */
