#include "fdt.c"
/* 把宿主侧的四个补丁按 guest_loader.c 的调用顺序跑一遍 */
int host_patch_all(unsigned char *buf, unsigned len)
{
    static const char *const bad[] = { "pl061@9030000", "pl031@9010000", "gpio-keys" };
    guest_loader_patch_dtb_memory(buf, len, 0x70000000ULL, 0xc000000ULL);
    guest_loader_patch_dtb_bootargs(buf, len, " console=ttyAMA0 rdinit=/init");
    guest_loader_patch_dtb_initrd(buf, len, 0x78000000ULL, 0x788bc000ULL);
    guest_loader_nop_dtb_nodes(buf, len, bad, 3);
    return 0;
}
