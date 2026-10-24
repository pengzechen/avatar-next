/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/fatfs_port/ffconf.h - FatFs R0.15 configuration for Avatar OS

   third_party/fatfs/source/ffconf.h is only a wrapper that includes this
   file (ff.h uses a quoted include, so the upstream directory wins). Keep
   FFCONF_DEF in sync with FF_DEFINED in third_party/fatfs/source/ff.h.

   Design notes:
     - writable, FAT12/16/32, LFN on (8-bit OEM code page).
     - 512-byte sectors only: both ramblk and sdblk expose 512-byte sectors.
     - no RTC -> fixed timestamps.
     - no exFAT, no mkfs, no fast-seek, no string API.
     - FF_FS_REENTRANT = 0: Avatar serializes every FatFs call with the
       global FS lock (include/fs_lock.h); FatFs' own mutex hooks are unused.
*/
#ifndef AVATAR_FFCONF_H
#define AVATAR_FFCONF_H

#define FFCONF_DEF 80286 /* R0.15 */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY  0
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_MKFS     0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    1
#define FF_PRINT_FLOAT  1
#define FF_STRF_ENCODE  3

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

/*
   Boot files are ASCII. FF_LFN_UNICODE is 0, so the VFS' UTF-8 path bytes are
   interpreted as OEM code page 437; non-ASCII names are not guaranteed.
*/
#define FF_CODE_PAGE   437
#define FF_USE_LFN     1
#define FF_MAX_LFN     255
#define FF_LFN_UNICODE 0
#define FF_LFN_BUF     255
#define FF_SFN_BUF     12
#define FF_FS_RPATH    0

/* Only one FAT volume is registered (the /boot partition). */
#define FF_VOLUMES         1
#define FF_STR_VOLUME_ID   0
#define FF_VOLUME_STRS     "RAM", "NAND", "CF", "SD", "SD2", "USB", "USB2", "USB3"
#define FF_MULTI_PARTITION 0

/*---------------------------------------------------------------------------/
/ Physical Drive Configurations
/---------------------------------------------------------------------------*/

#define FF_MIN_SS   512
#define FF_MAX_SS   512
#define FF_LBA64    0
#define FF_MIN_GPT  0x10000000
#define FF_USE_TRIM 0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY      0
#define FF_FS_EXFAT     0
#define FF_FS_NORTC     1
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2026
#define FF_FS_NOFSINFO  0
#define FF_FS_LOCK      0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000

#endif /* AVATAR_FFCONF_H */
