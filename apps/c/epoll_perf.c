#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int parse_int(const char *s, int fallback)
{
    if (!s || !*s)
        return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 100000000L)
        return fallback;
    return (int)v;
}

static int run_pipe_bench(int iterations, int batch)
{
    int p[2];
    if (pipe(p) != 0) {
        perror("pipe");
        return 1;
    }

    int ep = epoll_create1(0);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        perror("epoll_ctl ADD");
        return 1;
    }

    char buf[256];
    memset(buf, 'x', sizeof(buf));
    if (batch > (int)sizeof(buf))
        batch = (int)sizeof(buf);

    struct epoll_event out[8];
    uint64_t t0 = now_us();
    uint64_t events_seen = 0;
    uint64_t bytes = 0;

    for (int i = 0; i < iterations; i++) {
        ssize_t wr = write(p[1], buf, (size_t)batch);
        if (wr != batch) {
            fprintf(stderr, "write failed at iter %d: wr=%zd errno=%d\n", i, wr, errno);
            return 1;
        }

        int n = epoll_wait(ep, out, 8, 1000);
        if (n <= 0) {
            fprintf(stderr, "epoll_wait failed at iter %d: n=%d errno=%d\n", i, n, errno);
            return 1;
        }
        events_seen += (uint64_t)n;

        ssize_t rd = read(p[0], buf, (size_t)batch);
        if (rd != batch) {
            fprintf(stderr, "read failed at iter %d: rd=%zd errno=%d\n", i, rd, errno);
            return 1;
        }
        bytes += (uint64_t)rd;
    }

    uint64_t dt = now_us() - t0;
    if (dt == 0)
        dt = 1;

    printf("epoll_perf pipe\n");
    printf("iterations=%d batch=%d bytes=%llu events=%llu time_us=%llu\n",
           iterations, batch,
           (unsigned long long)bytes,
           (unsigned long long)events_seen,
           (unsigned long long)dt);
    printf("event_rate=%llu/s byte_rate=%llu B/s avg=%llu ns/iter\n",
           (unsigned long long)((uint64_t)iterations * 1000000ULL / dt),
           (unsigned long long)(bytes * 1000000ULL / dt),
           (unsigned long long)(dt * 1000ULL / (uint64_t)iterations));

    close(ep);
    close(p[0]);
    close(p[1]);
    return 0;
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? parse_int(argv[1], 10000) : 10000;
    int batch = argc > 2 ? parse_int(argv[2], 1) : 1;
    return run_pipe_bench(iterations, batch);
}
