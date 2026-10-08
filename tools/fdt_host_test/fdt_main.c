#include <stdio.h>
#include <string.h>
int host_patch_all(unsigned char *buf, unsigned len);
int main(int argc, char **argv)
{
    static unsigned char buf[16384];
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(buf, 1, sizeof(buf), f), o;
    fclose(f);
    o = (size_t)-1;
    for (size_t i = 0; i + 6 < n; i++) if (!memcmp(buf+i, "no-map", 6)) { o = i; break; }
    printf("补丁前: %zu 字节, no-map 偏移 %zd\n", n, (ssize_t)o);
    host_patch_all(buf, (unsigned)n);
    o = (size_t)-1;
    for (size_t i = 0; i + 6 < n; i++) if (!memcmp(buf+i, "no-map", 6)) { o = i; break; }
    printf("补丁后: no-map 偏移 %zd   %s\n", (ssize_t)o, o != (size_t)-1 ? "✓ 还在" : "✗ 丢了");
    FILE *g = fopen(argv[2], "wb"); fwrite(buf, 1, n, g); fclose(g);
    return 0;
}
