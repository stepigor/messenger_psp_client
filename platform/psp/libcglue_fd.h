/* Read-only descriptor ABI from PSPSDK src/libcglue/fdman.h (BSD license).
 * Copyright (c) 2006 Rafael Cabezas <rafpsp@gmail.com>
 * Keep this layout aligned with the installed SDK, not with firmware handles.
 * The native socket number is different from curl's POSIX file descriptor. */
#ifndef ST_LIBCGLUE_FD_H
#define ST_LIBCGLUE_FD_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint32_t descriptor,flags,ref_count;
    char *filename;
    uint8_t type;
} StLibcFd;
enum { ST_LIBC_SOCKET=3 };
extern StLibcFd *__descriptormap[];
#ifdef __PSP__
_Static_assert(offsetof(StLibcFd,type)==16,"PSPSDK descriptor ABI changed");
#endif
#endif
