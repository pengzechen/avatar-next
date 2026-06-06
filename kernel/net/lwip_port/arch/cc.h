#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include "types.h"

typedef uint8_t  u8_t;
typedef int8_t   s8_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;
typedef uintptr_t mem_ptr_t;

#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define BYTE_ORDER LITTLE_ENDIAN
#define SSIZE_MAX ((ssize_t)(SIZE_MAX >> 1))
#define LWIP_NO_UNISTD_H 1

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { } while (1)

#endif /* LWIP_ARCH_CC_H */
