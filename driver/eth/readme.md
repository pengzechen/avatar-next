  
设置tap  
  sudo ip tuntap del dev tap0 mode tap
  sudo ip tuntap add dev tap0 mode tap user "$USER"
  sudo ip link set tap0 up
清理tap
  sudo ip link set tap0 down
  sudo ip tuntap del dev tap0 mode tap

启动qemu，要多加参数
make PLATFORM=qemu-virt-riscv64 run-net LOG=info -j4 QEMU_NET_FLAGS="-netdev tap,id=net0,ifname=tap0,script=no,downscript=no -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56"

tcpdump:
sudo tcpdump -i tap0 -e -XX 'ether proto 0x88b5'



python3 -m site --user-site