/*
 * zipdir.h - find one member of a ZIP archive (an OOXML package) and stream it.
 *
 * Uses the central directory, never the local headers' sizes: Word and
 * LibreOffice may write members with a data descriptor, whose local header
 * carries zero sizes.
 */
#ifndef ZIPDIR_H
#define ZIPDIR_H

#include "wrplat.h"

typedef struct {
    wr_u32 dataOff;       /* first byte of the compressed data */
    wr_u32 compSize;
    wr_u32 size;          /* uncompressed */
    short  method;        /* 0 stored, 8 deflate */
    short  flags;
} ZipEntry;

enum {
    ZIP_OK = 0,
    ZIP_ERR_NOTZIP = 10,    /* no end-of-central-directory record */
    ZIP_ERR_NOENTRY = 11,   /* no member of that name */
    ZIP_ERR_READ = 12,
    ZIP_ERR_METHOD = 13,    /* compression other than stored/deflate */
    ZIP_ERR_CRYPT = 14,     /* encrypted member */
    ZIP_ERR_DATA = 15,      /* corrupt deflate stream */
    ZIP_ERR_MEM = 16,
    ZIP_ERR_STOPPED = 17
};

/* Case-insensitive lookup of `name` (no leading '/'). */
short zip_find(WrSource *src, const char *name, ZipEntry *e);

/* Streams the uncompressed member into the sink. */
short zip_extract(WrSource *src, const ZipEntry *e, WrWriteFn wr, void *wctx);

#endif
