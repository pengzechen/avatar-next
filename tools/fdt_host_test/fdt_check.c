/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include <stdio.h>
int guest_loader_dtb_node_has_prop(const unsigned char *dtb, unsigned size,
                                   const char *node, const char *prop);
int main(int argc, char **argv)
{
    static unsigned char buf[16384];
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    printf("  %-34s no-map=%d dma-coherent=%d\n", argv[1],
        guest_loader_dtb_node_has_prop(buf, (unsigned)n, "npu-pool@79000000", "no-map"),
        guest_loader_dtb_node_has_prop(buf, (unsigned)n, "npu@fdab0000", "dma-coherent"));
    return 0;
}
