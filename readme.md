
``` bash
make clean && sleep 1 && make ARCH=riscv64 rootfs && ./install-apps.sh ARCH=riscv64
make ARCH=riscv64 PLATFORM=qemu run-fs LOG=info -j4
```

``` bash
make clean && sleep 1 && make ARCH=aarch64 rootfs && ./install-apps.sh ARCH=aarch64
make ARCH=aarch64  PLATFORM=qemu run-fs LOG=info -j4
```

``` bash
make clean && sleep 1 && make ARCH=x86_64 rootfs && ./install-apps.sh ARCH=x86_64
make ARCH=x86_64  PLATFORM=qemu run-fs LOG=info -j4
```