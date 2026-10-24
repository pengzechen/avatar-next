# FatFs R0.15

Upstream: https://elm-chan.org/fsw/ff/ (archive `ff15.zip`)

Vendored files:

- `source/ff.c`, `source/ff.h`, `source/ffunicode.c`, `source/diskio.h`
- `LICENSE.txt` (from the same archive)
- `source/ffconf.h` — **the only vendor patch**, a one-line wrapper that
  includes `<fatfs_port/ffconf.h>`; the real configuration is in
  `fs/fatfs_port/ffconf.h` because `ff.h` includes `"ffconf.h"` from its own
  directory first.

Not vendored: `source/diskio.c` (Avatar implements `diskio` in
`fs/fatfs_port/diskio.c`) and `source/ffsystem.c` (not used).

When upgrading FatFs:

1. Replace `ff.c`, `ff.h`, `ffunicode.c`, `diskio.h` and `LICENSE.txt`.
2. Re-apply the `source/ffconf.h` wrapper.
3. Check whether `FFCONF_DEF` in `fs/fatfs_port/ffconf.h` still matches the
   new `FF_DEFINED` in `ff.h`.
4. Read the upstream `00history.txt` for new `ffconf.h` options.
