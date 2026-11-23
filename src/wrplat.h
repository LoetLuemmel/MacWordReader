/*
 * wrplat.h - the few things the portable core needs from its platform.
 *
 * The core (inflate, zip directory, docx parser, MacRoman table) is plain C89
 * and builds twice: with MPW SC for the 68k Mac, and with the host's cc for
 * the test harness (-DWR_HOST). Only memory allocation differs.
 *
 * Integer widths: MPW SC and the host disagree about nothing that matters as
 * long as the code says what it means - `long` for anything that can pass
 * 32767, `short` only for small values. Never plain `int` for sizes.
 */
#ifndef WRPLAT_H
#define WRPLAT_H

#ifdef WR_HOST
#include <stdlib.h>
#include <string.h>
#define WR_ALLOC(n)     calloc(1, (size_t)(n))
#define WR_FREE(p)      free(p)
#define WR_MEMCPY(d, s, n) memcpy((d), (s), (size_t)(n))
#else
#include <MacMemory.h>
#define WR_ALLOC(n)     ((void *)NewPtrClear((Size)(n)))
#define WR_FREE(p)      DisposePtr((Ptr)(p))
#define WR_MEMCPY(d, s, n) BlockMoveData((const void *)(s), (void *)(d), (Size)(n))
#endif

typedef unsigned char  wr_u8;
typedef unsigned short wr_u16;
typedef unsigned long  wr_u32;

/* Write sink: 0 = go on, non-zero = stop (out of memory, user abort). */
typedef long (*WrWriteFn)(void *ctx, const wr_u8 *buf, long n);

/* Random-access byte source: the open document. readAt returns the number
   of bytes read (short only at end of file), or -1 on an I/O error. */
typedef long (*WrReadAtFn)(void *ctx, wr_u32 off, wr_u8 *buf, long n);
typedef struct {
    WrReadAtFn readAt;
    void      *ctx;
    wr_u32     size;
} WrSource;

#endif
