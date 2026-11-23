# Vendored / upstream code layout

Avatar keeps imported upstream source **in-tree, next to its port layer**. There is
no `third_party/` directory anymore: everything needed for a clone lives under
`kernel/`, `fs/`, or `apps/`.

## Directory rules

- `kernel/net/lwip/`: upstream lwIP source only (`src/` + `COPYING`,
  BSD-3-Clause). It is unmodified; do not add Avatar-specific headers or glue here.
- `kernel/net/lwip_port/`: Avatar port layer for lwIP (OS callbacks, config,
  arch glue).
- `fs/ext4/lwext4/`, `fs/fat/fatfs/`: **first-party** filesystem implementations
  that started life upstream but are now maintained in this repository. Keep their
  core sources in upstream style; they are excluded from `make format` and
  `make apidoc`.
- `fs/<backend>/port/`: Avatar port layer for a filesystem backend
  (`fs/ext4/port`, `fs/fat/port`). This includes config headers, OS callbacks,
  allocator hooks, and libc shims that are specific to one package.
- `fs/vfs/`, `fs/pseudofs/`, `fs/init.c`: kernel filesystem layer (VFS,
  pseudo filesystems, mount/init). Public interfaces live in `include/fs/`.
- `apps/dropbear-2024.86/`: vendored Dropbear build tree. Only
  `dropbearmulti-aarch64` is tracked; everything else is ignored by `.gitignore`.
  The rootfs script looks for `dropbearmulti-<arch>` here and skips the package
  if the binary is absent.
- Shared kernel facilities belong in `include/`, `lib/`, `kernel/`, or
  `driver/`, not in a package compat directory.

## Packages

- **lwIP** (BSD-3-Clause, unmodified upstream):
  `kernel/net/lwip` with port code in `kernel/net/lwip_port`
- **lwext4** (BSD-3-Clause, forked and maintained first-party):
  `fs/ext4/lwext4` with port code in `fs/ext4/port`
- **FatFs** (ChaN license, first-party): `fs/fat/fatfs` with port code in
  `fs/fat/port`

`fs/fat/fatfs` carries one deliberate vendor patch: `source/ffconf.h` is a
one-line wrapper that includes the real Avatar configuration from
`fs/fat/port/ffconf.h` (FatFs uses a quoted `#include "ffconf.h"`, so the
upstream directory wins over `-I`). The README in that package records how to
re-apply it when upgrading.

## Libc shim design

Libc shim headers are package-local by default. Each package expects a different
libc surface, so forcing all packages through one global shim directory makes
accidental ABI and macro coupling likely.

Use this order:

1. Put common, real kernel APIs in shared kernel headers.
2. Put package-specific missing libc headers in that package's `_port/libc_shim`.
3. Add a shared freestanding libc shim only after at least two packages need the
   same complete semantics, not just the same header name.

Build flags must put a package's libc shim include directory before upstream and
kernel include directories only for that package's objects.
