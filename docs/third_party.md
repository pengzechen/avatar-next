# Third-party layout

Avatar keeps upstream third-party source separate from local porting code.

## Directory rules

- `third_party/<name>/`: upstream source only. Do not add Avatar-specific
  headers, stubs, or glue here unless carrying a deliberate vendor patch.
- `<subsystem>/<name>_port/`: Avatar port layer for that third-party package.
  This includes config headers, OS callbacks, allocator hooks, and libc shims
  that are specific to one package.
- Shared kernel facilities belong in `include/`, `lib/`, `kernel/`, or
  `driver/`, not in a package compat directory.

Current packages:

- `third_party/lwext4` with port code in `fs/lwext4_port`
- `third_party/lwip` with port code in `kernel/net/lwip_port`
- `third_party/lua54` with port code in `lib/lua54_port`

## Libc shim design

Libc shim headers are package-local by default. Each third-party package expects
a different libc surface, so forcing all packages through one global shim
directory makes accidental ABI and macro coupling likely.

Use this order:

1. Put common, real kernel APIs in shared kernel headers.
2. Put package-specific missing libc headers in that package's `_port/libc_shim`.
3. Add a shared freestanding libc shim only after at least two packages need the
   same complete semantics, not just the same header name.

Build flags must put a package's libc shim include directory before upstream and
kernel include directories only for that package's objects.
