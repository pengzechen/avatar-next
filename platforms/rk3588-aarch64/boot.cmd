# Avatar OS - TFTP Boot Script

setenv tftpblocksize 1468
setenv tftpwindowsize 32
bootdev hunt ethernet
setenv tftpdstp 69
setenv ipaddr 192.168.100.2
setenv serverip 192.168.100.1

echo "Loading kernel from TFTP..."
tftp 0x400000 kernel_aarch64.bin

echo "Loading rootfs from TFTP..."
tftp 0x10000000 rootfs-aarch64.img

echo "Starting Avatar OS..."
go 0x400000