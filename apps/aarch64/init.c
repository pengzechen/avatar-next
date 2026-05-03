/*
 * apps/aarch64/init.c - 用户空间 init 程序
 *
 * 启动 busybox，传递完整的 argv/envp，
 * 让 musl libc 的 _start 能正确初始化 TLS 和运行时。
 *
 * 编译后放入 rootfs 的 /init（或 /sbin/init）。
 * 内核通过 ELF 加载器运行此程序。
 */

/* ── 系统调用号 ─────────────────────────────────────────────── */
#define SYS_EXIT     0
#define SYS_WRITE   20
#define SYS_EXECVE   4

/* ── 基本类型（无 libc）─────────────────────────────────────── */
typedef unsigned long  size_t;
typedef long           ssize_t;

/* ── 系统调用包装（来自 lib/syscall.S）──────────────────────── */
extern void    exit(int code) __attribute__((noreturn));
extern ssize_t write(int fd, const void *buf, size_t len);
extern int     execve(const char *path, char *const argv[], char *const envp[]);

/* ── 简单字符串输出（无需 printf）──────────────────────────── */
static void puts_fd(int fd, const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    write(fd, s, len);
}

/* ── main ──────────────────────────────────────────────────── */
int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    puts_fd(1, "[init] Starting busybox...\n");

    /* argv 传给 busybox：argv[0] = 程序名，后续可加命令行参数 */
    static char *bx_argv[] = {
        "/busybox",
        "sh",       /* 启动交互式 shell */
        (char *)0
    };

    /* 基本环境变量 */
    static char *bx_envp[] = {
        "PATH=/",
        "HOME=/",
        "TERM=vt100",
        "SHELL=/busybox",
        (char *)0
    };

    execve("/busybox", bx_argv, bx_envp);

    /* execve 不应该返回 */
    puts_fd(2, "[init] ERROR: execve failed!\n");
    exit(1);
}
