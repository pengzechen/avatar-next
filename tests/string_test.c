/*
 * tests/string_test.c — 字符串/内存函数自检
 *
 * 覆盖 memcpy / memset / memmove 的**长度 × 源对齐 × 目的对齐**组合，
 * 用参考实现（逐字节）逐字节比对，并检查拷贝区间之外的守卫字节没被踩。
 *
 * 为什么值得单独测这几个：
 *   1. 它们是全内核最热的函数，且有多条实现路径（generic 的 8/4/2 字节阶梯、
 *      aarch64 的 NEON 阈值分派），路径选择取决于长度与对齐；
 *   2. NEON 版本是手写汇编（ld1/st1/dup），边界（不足 16 字节的尾巴、
 *      对齐前缀）出错不会让内核崩，只会悄悄拷错数据；
 *   3. 长度矩阵跨过 128 字节阈值两侧，确保两条路径都被走到。
 *
 * 编译开关：make PLATFORM=... STRING_TEST=1 （见 Makefile），
 * 在 kernel_main 里由 RUN_STRING_TEST 调起；默认不编译进启动路径。
 */

#include "string.h"
#include "types.h"
#include "klog.h"

#define BUF_SIZE   1200u    /* 足够容纳最大测试长度 + 前后守卫（守卫靠整缓冲区比对覆盖） */
#define BLOB_SIZE  900u     /* 大结构体赋值：要大到 GCC 走 bl memcpy 而不是内联展开 */

/* 16 字节对齐，便于构造各种对齐组合（偏移量仍然任取） */
static uint8_t g_src[BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_dst[BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_ref[BUF_SIZE] __attribute__((aligned(16)));

static int g_cases;
static int g_failed;

/* ── 参考实现（逐字节，作为唯一权威）────────────────────────────── */
static void
ref_memcpy(uint8_t *d, const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
}

static void
ref_memset(uint8_t *d, uint8_t b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = b;
}

/* ── 用例驱动 ─────────────────────────────────────────────────── */

static void
fail(const char *what, size_t n, unsigned doff, unsigned soff)
{
    g_failed++;
    KLOG_ERROR("[string_test] FAIL %s: n=%u doff=%u soff=%u\n",
               what, (unsigned) n, doff, soff);
}

/*
 * 用给定的长度/对齐组合测一次 memcpy。
 * 期望：dst[c..c+n) == src 的对应内容，且前后守卫字节不变。
 */
static void
case_memcpy(size_t n, unsigned doff, unsigned soff)
{
    const uint8_t *s = g_src + soff;
    uint8_t       *d = g_dst + doff;

    g_cases++;

    /* 铺底：源、目的、参考各自填不同图案 */
    ref_memset(g_src, 0xA5, sizeof(g_src));
    for (unsigned i = 0; i < n; i++)
        g_src[soff + i] = (uint8_t) (i * 7 + 3);

    ref_memset(g_dst, 0x3C, sizeof(g_dst));
    ref_memset(g_ref, 0x3C, sizeof(g_ref));

    /* 参考结果 */
    ref_memcpy(g_ref + doff, s, n);

    /* 被测实现 */
    memcpy(d, s, n);

    /* 逐字节比对整个缓冲区：既查拷贝内容，也查守卫字节没被踩 */
    for (unsigned i = 0; i < (unsigned) sizeof(g_dst); i++) {
        if (g_dst[i] != g_ref[i]) {
            fail("memcpy", n, doff, soff);
            return;
        }
    }
}

static void
case_memset(size_t n, unsigned doff, uint8_t value)
{
    uint8_t *d = g_dst + doff;

    g_cases++;

    ref_memset(g_dst, 0x3C, sizeof(g_dst));
    ref_memset(g_ref, 0x3C, sizeof(g_ref));
    ref_memset(g_ref + doff, value, n);

    memset(d, value, n);

    for (unsigned i = 0; i < (unsigned) sizeof(g_dst); i++) {
        if (g_dst[i] != g_ref[i]) {
            fail("memset", n, doff, value);
            return;
        }
    }
}

static void
case_memmove(size_t n, unsigned doff, unsigned soff)
{
    /* 只用一块缓冲区，制造真实重叠：d 和 s 都在 g_dst 里 */
    uint8_t *d = g_dst + doff;
    uint8_t *s = g_dst + soff;

    g_cases++;

    ref_memset(g_dst, 0xA5, sizeof(g_dst));
    for (unsigned i = 0; i < n; i++)
        g_dst[soff + i] = (uint8_t) (i * 5 + 1);

    memcpy(g_ref, g_dst, sizeof(g_ref));   /* 参考快照（搬之前）*/

    /*
     * 参考：在快照上按逐字节 memmove 语义重放（重叠方向由地址高低决定）。
     * 注意参考必须作用在 g_ref 上 —— 写到 g_dst 上会把被测缓冲区改两遍。
     */
    if (doff < soff) {
        for (unsigned i = 0; i < n; i++)
            g_ref[doff + i] = g_ref[soff + i];
    } else if (doff > soff) {
        for (unsigned i = (unsigned) n; i > 0; i--)
            g_ref[doff + i - 1] = g_ref[soff + i - 1];
    }

    memmove(d, s, n);

    for (unsigned i = 0; i < (unsigned) sizeof(g_dst); i++) {
        if (g_dst[i] != g_ref[i]) {
            fail("memmove", n, doff, soff);
            return;
        }
    }
}

/* ── 入口 ─────────────────────────────────────────────────────── */

void
test_string_functions(void)
{
    /* 长度矩阵：跨过 0/1、对齐边界（16 的倍数附近）、以及 128 阈值两侧 */
    static const size_t lens[] = {
        0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 1000
    };
    static const unsigned offs[] = { 0, 1, 2, 3, 7, 8, 15, 16 };

    KLOG_INFO("=== string self-test (memcpy/memset/memmove) ===\n");

    g_cases = 0;
    g_failed = 0;

    /* memcpy：长度 × 目的偏移 × 源偏移 */
    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (unsigned di = 0; di < sizeof(offs) / sizeof(offs[0]); di++) {
            for (unsigned si = 0; si < sizeof(offs) / sizeof(offs[0]); si++)
                case_memcpy(lens[li], offs[di], offs[si]);
        }
    }

    /* memset：长度 × 目的偏移 × 几个代表性填充值 */
    static const uint8_t values[] = { 0x00, 0x01, 0x5A, 0x7F, 0xFF };
    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (unsigned di = 0; di < sizeof(offs) / sizeof(offs[0]); di++) {
            for (unsigned vi = 0; vi < sizeof(values) / sizeof(values[0]); vi++)
                case_memset(lens[li], offs[di], values[vi]);
        }
    }

    /* memmove：重叠（doff<soff、doff>soff、doff==soff 三种关系）*/
    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (unsigned di = 0; di < sizeof(offs) / sizeof(offs[0]); di++) {
            for (unsigned si = 0; si < sizeof(offs) / sizeof(offs[0]); si++)
                case_memmove(lens[li], offs[di], offs[si]);
        }
    }

    /*
     * 编译器**自己生成**的 memcpy 调用 —— 与上面那些源码调用走的是不同的
     * 代码路径：GCC 对大结构体整体赋值会发一个对外部符号 memcpy 的 bl，
     * 而不是用 string.h 里的 static inline。这条路径曾经落到 lib/string.c
     * 那个逐字节实现上（fork 的 816 字节 trap frame 拷贝就是受害者），
     * 所以专门测一下：拷贝结果必须与逐字节参考一致。
     */
    {
        static struct { uint8_t b[BLOB_SIZE]; } blob_a, blob_b;

        g_cases++;
        for (unsigned i = 0; i < BLOB_SIZE; i++)
            blob_a.b[i] = (uint8_t) (i * 13 + 7);
        ref_memset(blob_b.b, 0x11, BLOB_SIZE);

        blob_b = blob_a;        /* ← 期望编译成 bl memcpy（外部符号） */

        for (unsigned i = 0; i < BLOB_SIZE; i++) {
            if (blob_b.b[i] != blob_a.b[i]) {
                fail("struct-assign memcpy", BLOB_SIZE, 0, i);
                break;
            }
        }
    }

    /* 非内存函数顺带回归 */
    {
        char buf[64];

        if (strlen("hello") != 5)                 { g_failed++; KLOG_ERROR("[string_test] FAIL strlen\n"); }
        strcpy(buf, "hello ");
        strcat(buf, "world");
        if (strlen(buf) != 11)                    { g_failed++; KLOG_ERROR("[string_test] FAIL strcat\n"); }
        if (strcmp("abc", "abc") != 0)            { g_failed++; KLOG_ERROR("[string_test] FAIL strcmp(eq)\n"); }
        if (strcmp("abc", "abd") >= 0)            { g_failed++; KLOG_ERROR("[string_test] FAIL strcmp(lt)\n"); }
        if (strncmp("abc", "abd", 2) != 0)        { g_failed++; KLOG_ERROR("[string_test] FAIL strncmp\n"); }
        if (memcmp("abc", "abc", 3) != 0)         { g_failed++; KLOG_ERROR("[string_test] FAIL memcmp\n"); }
        if (atol("12345") != 12345)               { g_failed++; KLOG_ERROR("[string_test] FAIL atol\n"); }
        if (memchr("abcdef", 'd', 6) == NULL)     { g_failed++; KLOG_ERROR("[string_test] FAIL memchr\n"); }
        if (strchr("abc", 'b') == NULL)           { g_failed++; KLOG_ERROR("[string_test] FAIL strchr\n"); }
        g_cases += 9;
    }

    if (g_failed == 0)
        KLOG_INFO("=== STRING TEST: PASS (%u cases) ===\n", (unsigned) g_cases);
    else
        KLOG_ERROR("=== STRING TEST: %u FAILED of %u cases ===\n",
                   (unsigned) g_failed, (unsigned) g_cases);
}
