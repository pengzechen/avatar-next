# RISC-V64 musl C Development Rootfs

This package adds a musl-based C development environment to the SG2002/RISC-V64
rootfs. It is intended for simple on-board C development under Avatar OS.

## Build Overlay

```sh
bash tools/package-rv64-musl-dev.sh
```

Outputs:

```text
build/rv64-musl-dev-rootfs/
build/rv64-musl-dev-rootfs.tar.gz
```

The overlay contains:

- `/usr/bin/tcc`
- `/usr/bin/cc -> tcc`
- `/usr/bin/musl-cc -> tcc`
- `/usr/include` from the RISC-V64 musl sysroot
- `/usr/lib` from the RISC-V64 musl sysroot
- `/usr/lib/tcc/libtcc1.a`
- `/lib/libc.so`
- `/lib/ld-musl-riscv64.so.1 -> libc.so`
- `/lib/ld-musl-riscv64v0p7_xthead.so.1 -> libc.so`

## Install To SD Rootfs

Mount the SD card's second partition on the host, then extract the overlay into
that mount point:

```sh
sudo tar -C /mnt/rootfs -xzf build/rv64-musl-dev-rootfs.tar.gz
sync
```

## Use On Board

For normal `.c` files:

```sh
cc hello.c -o hello
./hello
```

If the source file has a non-C suffix, such as `1.txt`, force C mode:

```sh
cc -x c 1.txt -o hello
./hello
```

## Scope

This is not a full native GCC toolchain. The host `riscv64-linux-musl-gcc` in
`~/Desktop/Software/compiler/riscv64-linux-musl-cross` is an x86-hosted cross
compiler and cannot run on the RISC-V board. Building full native GCC/binutils
for Avatar OS requires additional work and more Linux syscall compatibility.

TinyCC is used here as a small native RISC-V64 compiler frontend that links
against musl headers and libraries.
