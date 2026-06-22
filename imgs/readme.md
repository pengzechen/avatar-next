

三个架构的rootfs（动态链接版）


fatload mmc 0:1 0x89000000 rootfs.img  ; fatload mmc 0:1 0x80200000 StarryOS_sg2002.bin ; go 0x80200000

fatload mmc 0:1 0x89000000 rootfs.img  ; fatload mmc 0:1 0x80200000  kernel_riscv64.bin ; go 0x80200000


dropbear -R -F -B -p 22
ssh -o StrictHostKeyChecking=no root@192.168.100.2

ssh -vvv root@192.168.100.2

ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sudo cp /home/ajax/Desktop/Project/Kernel/avatar-next/third_party/dropbear-2024.86/dropbearmulti.stripped /media/ajax/avatarfs/usr/sbin/dropbearmulti
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sudo chmod +x /media/ajax/avatarfs/usr/sbin/dropbearmulti
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sudo ln -sf dropbearmulti /media/ajax/avatarfs/usr/sbin/dropbear
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sudo ln -sf ../sbin/dropbearmulti /media/ajax/avatarfs/usr/bin/dropbearkey
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sudo mkdir -p /media/ajax/avatarfs/etc/dropbear
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ echo 'root::0:0:root:/root:/bin/sh' | sudo tee /media/ajax/avatarfs/etc/passwd
root::0:0:root:/root:/bin/sh
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ echo 'root:x:0:' | sudo tee /media/ajax/avatarfs/etc/group
root:x:0:
ajax@ajax-BOD-WXX9:~/Desktop/Project/Kernel/avatar-next$ sync

