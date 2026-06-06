/* lib/lua54_port/libc_shim/types.h
 * Wrapper around the kernel types.h.  After pulling in the kernel types we
 * undefine UNUSED so that Lua's own llimits.h can define it as ((void)(x))
 * instead of the kernel's __attribute__((unused)) which would cause
 * "redeclared as different kind of symbol" errors in Lua source files.
 */
#include <../include/types.h>

#ifdef UNUSED
#undef UNUSED
#endif
