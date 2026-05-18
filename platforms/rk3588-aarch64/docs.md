setenv tftpblocksize 1468; setenv tftpwindowsize 32
bootdev hunt ethernet; setenv tftpdstp 69; setenv ipaddr 192.168.100.2; setenv serverip 192.168.100.1; tftp 0x400000 kernel_aarch64.bin; tftp 0x10000000 rootfs-aarch64.img; go 0x400000

bootdev hunt ethernet; setenv tftpdstp 69; setenv ipaddr 192.168.100.2; setenv serverip 192.168.100.1; tftp 0x400000 kernel_aarch64.bin; go 0x400000