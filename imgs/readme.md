

三个架构的rootfs（动态链接版）


fatload mmc 0:1 0x89000000 rootfs.img  ; fatload mmc 0:1 0x80200000 StarryOS_sg2002.bin ; go 0x80200000

fatload mmc 0:1 0x89000000 rootfs.img  ; fatload mmc 0:1 0x80200000  kernel_riscv64.bin ; go 0x80200000