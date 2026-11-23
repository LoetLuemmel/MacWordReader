/*
 * inflate.h - streaming raw DEFLATE (RFC 1951) decoder for a small Mac.
 *
 * Fixed memory: a 32 KB history window that doubles as the output buffer,
 * a 2 KB input buffer and the Huffman tables (~3 KB). The decompressed data
 * is never held in full - it streams into the sink, so a 5 MB document.xml
 * costs no more RAM than a 5 KB one.
 */
#ifndef INFLATE_H
#define INFLATE_H

#include "wrplat.h"

/* Sequential reader: returns bytes read, 0 at end, -1 on error. */
typedef long (*WrReadFn)(void *ctx, wr_u8 *buf, long max);

enum {
    INF_OK = 0,
    INF_ERR_MEM = 1,       /* no memory for the window */
    INF_ERR_DATA = 2,      /* not valid deflate data */
    INF_ERR_READ = 3,      /* the source failed */
    INF_ERR_STOPPED = 4    /* the sink asked to stop */
};

short inflate_stream(WrReadFn rd, void *rctx, WrWriteFn wr, void *wctx);

#endif
