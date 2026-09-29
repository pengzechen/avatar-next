/*
 * fs/lwext4_port/fs_lock.c — lwext4 的全局串行化
 *
 * 为什么需要：lwext4 是个**单线程用户态库**，它的全局缓冲缓存（一棵红黑树）
 * 没有任何内部同步。内核这边从前也没有在 fs 路径上加过锁，于是 SMP>1 时
 * 多个核并发 exec 就会把那棵树写坏：
 *
 *     #0  ext4_bcache_free / ext4_buf_lru_RB_REMOVE_COLOR   (两个不同 trace)
 *     #4  ext4_fread <- read_file_fully <- sys_execve
 *
 * 实测：riscv64 / aarch64 纯 exec 压力下，SMP=1 连续多次干净，
 *       **SMP=4 连续两次都崩**。
 *
 * 为什么用 --wrap 而不是在调用点加锁：
 *   1. 内核里有 90+ 个 lwext4 调用点，散在 8 个文件；22 个直接调用者里有
 *      15 个是**多出口**的（提前 return），手工配对 lock/unlock 必然漏。
 *   2. 粒度要落在**每次 API 调用**上，不能落在"整个内核函数"上 ——
 *      read_file_fully 要循环读 5.6MB，整函数持锁意味着关中断几十到几百
 *      毫秒，会把 timer tick 丢光。
 *   `-Wl,--wrap=<sym>` 让链接器把所有 `sym` 引用改指向 `__wrap_sym`，
 *   真实实现仍可经 `__real_sym` 调用 —— 正好是"每次调用"的粒度，
 *   而且零调用点改动。
 *
 * 锁是**可重入**的：lwext4 内部会回调内核（ext4_user_calloc 等），
 * 那些回调走的路径也可能再进来。按 CPU 记 owner + 深度计数。
 * 持锁期间关本核中断：否则本核上的另一个任务抢占后再拿这把锁会自死锁
 * （owner 判定会把它误认成递归）。
 *
 * 强度说明：这是把 lwext4 当成**一次一个核**的临界区，不是细粒度锁。
 * 好处是绝对安全；代价是 fs 操作在 SMP 下串行。等真要抠性能时再谈
 * 按 inode/挂载点分锁。
 *
 * ⚠️ 包装函数**必须**经下面的 FS_WRAP 宏生成，不许手写。手抄签名的代价
 *    实测过：漏掉 `__real_*` 的声明 -> 编译器按隐式声明当成返回 `int`
 *    -> aarch64 上指针返回值被 `sxtw` 截成低 32 位
 *    （`0xffff0000416d20c0` 变成 `0x416d20c0`），`ls` 第一次 getdents 就
 *    死在 `ext4_dir_getdents+0x40`，FAR 每次都是同一个值 —— 一个"看起来
 *    完全像竞态"的确定性崩溃。FS_WRAP 用 `__typeof__(fn)` 从头文件取真身
 *    签名，再用 _Static_assert 钉住，签名对不上直接编译失败。
 */
#include "types.h"
#include "arch.h"
#include "spinlock.h"
#include "klog.h"
#include "task/cpu.h"

#include <ext4.h>
#include <ext4_mbr.h>
#include <ext4_blockdev.h>
#include <ext4_super.h> /* ext4_sb_read */

#define FS_LOCK_NO_OWNER ((uint32_t)-1)

static spinlock_t g_fs_lock = SPINLOCK_INIT;
static volatile uint32_t g_fs_owner = FS_LOCK_NO_OWNER;
static uint32_t g_fs_depth[AVATAR_MAX_CPUS];
static uint64_t g_fs_flags[AVATAR_MAX_CPUS];

static inline void fs_lock(void)
{
    uint32_t me = get_current_cpu_id();
    if (me >= AVATAR_MAX_CPUS)
        me = 0;

    /* 递归：本核已经持有 */
    if (__atomic_load_n(&g_fs_owner, __ATOMIC_ACQUIRE) == me) {
        g_fs_depth[me]++;
        return;
    }

    g_fs_flags[me] = arch_irq_save();
    spin_lock(&g_fs_lock);
    g_fs_depth[me] = 1;
    __atomic_store_n(&g_fs_owner, me, __ATOMIC_RELEASE);
}

static inline void fs_unlock(void)
{
    uint32_t me = get_current_cpu_id();
    if (me >= AVATAR_MAX_CPUS)
        me = 0;

    if (--g_fs_depth[me] != 0)
        return;

    __atomic_store_n(&g_fs_owner, FS_LOCK_NO_OWNER, __ATOMIC_RELEASE);
    spin_unlock(&g_fs_lock);
    arch_irq_restore(g_fs_flags[me]);
}

/*
 * 上面两个是 --wrap 包装用的**每次调用**粒度。但有些操作是**跨多次调用**的，
 * 单独锁每次调用不够：比如目录迭代 —— `ext4_dir_entry_next` 把条目拷进
 * `dir->de` 并返回它的地址，调用方要拿着这个指针继续用（lwext4 的
 * `ext4_dir` 是内联在 `vfs_file_t` 里的，`de` 就在同一个对象里）。如果只锁
 * 单次调用，两次调用之间另一个核可以对同一个目录对象做别的事。
 *
 * 这类地方要把**整个序列**圈进一个临界区。锁是可重入的，所以外面加一层
 * 粗锁、里面每次调用那层细锁照常嵌套，不会死锁。
 */
void fs_lwext4_lock(void)
{
    fs_lock();
}

void fs_lwext4_unlock(void)
{
    fs_unlock();
}

/* ── 包装函数 ──────────────────────────────────────────────────────
 *
 * `extern __typeof__(fn) __real_##fn;` 让 __real_* 自动拿到头文件里
 * 那份真实原型（一个字节都不用抄）；_Static_assert 再把包装自己的
 * 签名和它钉在一起。返回类型由 __auto_type 推导，所以不存在"返回值
 * 被截断"的可能。
 *
 * 参数里的 `args` 是形参表（带名字，给定义用），`call_args` 是转发时
 * 的实参表。两者都是类型的一部分，写错了断言会响。
 *
 * 下面这一整块关掉 clang-format：宏体要的是行尾 `\` 对齐，而表格那部分
 * clang-format 对宏实参不按声明符解析，会把 `ext4_dir *dir` 排成
 * `ext4_dir * dir`（读起来像乘法）。这张表是生成的，要的是"原型原样"。
 */
/* clang-format off */
#define FS_WRAP(ret, fn, args, call_args)                                     \
    extern __typeof__(fn) __real_##fn;                                        \
    _Static_assert(__builtin_types_compatible_p(ret args, __typeof__(fn)),    \
                   "fs_lock: wrapper signature does not match lwext4 header"); \
    ret __wrap_##fn args                                                      \
    {                                                                         \
        fs_lock();                                                            \
        __auto_type _r = __real_##fn call_args;                               \
        fs_unlock();                                                          \
        return _r;                                                            \
    }

/*
 * 这张表从 third_party/lwext4/include 下的头文件机械提取生成，勿手改。
 * 新增 lwext4 调用时，如果用到表外的符号，要在这里补一条，并在 Makefile 的
 * LWEXT4_WRAP_SYMS 里同步 —— 否则那条路径不受保护。补的时候注意：符号所在
 * 的头文件必须已经 include（FS_WRAP 会检查，漏了会直接编译失败）。
 */
FS_WRAP(int, ext4_block_fini, (struct ext4_blockdev *bdev), (bdev))
FS_WRAP(int, ext4_block_init, (struct ext4_blockdev *bdev), (bdev))
FS_WRAP(int, ext4_device_register,
        (struct ext4_blockdev *bd, const char *dev_name), (bd, dev_name))
FS_WRAP(int, ext4_device_unregister, (const char *dev_name), (dev_name))
FS_WRAP(int, ext4_dir_close, (ext4_dir *dir), (dir))
FS_WRAP(const ext4_direntry *, ext4_dir_entry_next, (ext4_dir *dir), (dir))
FS_WRAP(int, ext4_dir_mk, (const char *path), (path))
FS_WRAP(int, ext4_dir_open, (ext4_dir *dir, const char *path), (dir, path))
FS_WRAP(int, ext4_dir_rm, (const char *path), (path))
FS_WRAP(int, ext4_fclose, (ext4_file *file), (file))
FS_WRAP(int, ext4_fopen,
        (ext4_file *file, const char *path, const char *flags),
        (file, path, flags))
FS_WRAP(int, ext4_fopen2, (ext4_file *file, const char *path, int flags),
        (file, path, flags))
FS_WRAP(int, ext4_fread,
        (ext4_file *file, void *buf, size_t size, size_t *rcnt),
        (file, buf, size, rcnt))
FS_WRAP(int, ext4_fremove, (const char *path), (path))
FS_WRAP(int, ext4_frename, (const char *path, const char *new_path),
        (path, new_path))
FS_WRAP(int, ext4_fseek, (ext4_file *file, int64_t offset, uint32_t origin),
        (file, offset, origin))
FS_WRAP(uint64_t, ext4_fsize, (ext4_file *file), (file))
FS_WRAP(uint64_t, ext4_ftell, (ext4_file *file), (file))
FS_WRAP(int, ext4_ftruncate, (ext4_file *file, uint64_t size), (file, size))
FS_WRAP(int, ext4_fwrite,
        (ext4_file *file, const void *buf, size_t size, size_t *wcnt),
        (file, buf, size, wcnt))
FS_WRAP(int, ext4_inode_exist, (const char *path, int type), (path, type))
FS_WRAP(int, ext4_mbr_scan,
        (struct ext4_blockdev *parent, struct ext4_mbr_bdevs *bdevs),
        (parent, bdevs))
FS_WRAP(int, ext4_mode_get, (const char *path, uint32_t *mode), (path, mode))
FS_WRAP(int, ext4_mount,
        (const char *dev_name, const char *mount_point, bool read_only),
        (dev_name, mount_point, read_only))
FS_WRAP(int, ext4_raw_inode_fill,
        (const char *path, uint32_t *ret_ino, struct ext4_inode *inode),
        (path, ret_ino, inode))
FS_WRAP(int, ext4_readlink,
        (const char *path, char *buf, size_t bufsize, size_t *rcnt),
        (path, buf, bufsize, rcnt))
FS_WRAP(int, ext4_sb_read, (struct ext4_blockdev *bdev, struct ext4_sblock *s),
        (bdev, s))
/* clang-format on */
