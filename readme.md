
``` bash
make clean && make ARCH=riscv64 rootfs && ./install-apps.sh ARCH=riscv64
make ARCH=riscv64 PLATFORM=qemu run-fs LOG=debug
```

``` bash
make clean && make ARCH=aarch64 rootfs && ./install-apps.sh ARCH=aarch64
make ARCH=aarch64  PLATFORM=qemu run-fs LOG=debug
```

``` bash
make clean && make ARCH=x86_64 rootfs && ./install-apps.sh ARCH=x86_64
make ARCH=x86_64  PLATFORM=qemu run-fs LOG=debug
```