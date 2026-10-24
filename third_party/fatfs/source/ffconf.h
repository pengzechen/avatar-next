/*----------------------------------------------------------------------------/
/  Avatar OS vendor patch: keep the real configuration outside third_party.
/
/  ff.h does `#include "ffconf.h"` with quotes, so the compiler finds this
/  file next to ff.h before any -I directory.  The actual Avatar config lives
/  in fs/fatfs_port/ffconf.h; bumping FatFs only requires re-applying this
/  one-line wrapper (see ../../README.md).
/----------------------------------------------------------------------------*/
#include <fatfs_port/ffconf.h>
