
make clean
make ARCH=riscv64 run-fs LOG=warn -j4
cp imgs/rootfs-riscv64.img build/
make ARCH=riscv64 run-fs LOG=warn -j4


make clean
make ARCH=aarch64 run-fs LOG=warn -j4
cp imgs/rootfs-aarch64.img build/
make ARCH=aarch64 run-fs LOG=warn -j4


make clean
make ARCH=x86_64 run-fs LOG=warn -j4
cp imgs/rootfs-x86_64.img build/
make ARCH=x86_64 run-fs LOG=warn -j4