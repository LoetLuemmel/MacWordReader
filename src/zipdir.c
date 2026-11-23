/*
 * zipdir.c - ZIP central directory lookup and member streaming.
 */
#include "zipdir.h"
#include "inflate.h"

static wr_u32 Le16(const wr_u8 *p) { return (wr_u32)p[0] | ((wr_u32)p[1] << 8); }
static wr_u32 Le32(const wr_u8 *p)
{
    return (wr_u32)p[0] | ((wr_u32)p[1] << 8) | ((wr_u32)p[2] << 16) | ((wr_u32)p[3] << 24);
}

static short ReadExact(WrSource *src, wr_u32 off, wr_u8 *buf, long n)
{
    if (off + (wr_u32)n > src->size)
        return ZIP_ERR_READ;
    return src->readAt(src->ctx, off, buf, n) == n ? ZIP_OK : ZIP_ERR_READ;
}

static short SameName(const wr_u8 *a, long alen, const char *b)
{
    long i;
    for (i = 0; i < alen; i++) {
        wr_u8 x = a[i], y = (wr_u8)b[i];
        if (y == 0)
            return 0;
        if (x >= 'A' && x <= 'Z') x = (wr_u8)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (wr_u8)(y + 32);
        if (x != y)
            return 0;
    }
    return b[alen] == 0;
}

/* Finds the end-of-central-directory record in the last 64 KB + 22 bytes. */
static short FindEocd(WrSource *src, wr_u32 *cdOff, wr_u32 *cdCount)
{
    wr_u8 *buf;
    long n, i;
    wr_u32 start;
    short err = ZIP_ERR_NOTZIP;

    if (src->size < 22)
        return ZIP_ERR_NOTZIP;
    n = src->size < 65558UL ? (long)src->size : 65558L;
    start = src->size - (wr_u32)n;
    buf = (wr_u8 *)WR_ALLOC(n);
    if (buf == 0)
        return ZIP_ERR_MEM;
    if (ReadExact(src, start, buf, n) != ZIP_OK) {
        WR_FREE(buf);
        return ZIP_ERR_READ;
    }
    for (i = n - 22; i >= 0; i--) {
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) {
            *cdCount = Le16(buf + i + 10);
            *cdOff = Le32(buf + i + 16);
            err = (*cdOff < src->size) ? ZIP_OK : ZIP_ERR_NOTZIP;
            break;
        }
    }
    WR_FREE(buf);
    return err;
}

short zip_find(WrSource *src, const char *name, ZipEntry *e)
{
    wr_u32 cdOff, count, off, i;
    wr_u8 hdr[46], nm[256], loc[30];
    long nlen, xlen, clen;
    short err;

    err = FindEocd(src, &cdOff, &count);
    if (err != ZIP_OK)
        return err;
    off = cdOff;
    for (i = 0; i < count; i++) {
        if (ReadExact(src, off, hdr, 46) != ZIP_OK)
            return ZIP_ERR_READ;
        if (!(hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 1 && hdr[3] == 2))
            return ZIP_ERR_NOTZIP;
        nlen = (long)Le16(hdr + 28);
        xlen = (long)Le16(hdr + 30);
        clen = (long)Le16(hdr + 32);
        if (nlen < (long)sizeof(nm)) {
            if (ReadExact(src, off + 46, nm, nlen) != ZIP_OK)
                return ZIP_ERR_READ;
            if (SameName(nm, nlen, name)) {
                wr_u32 lh = Le32(hdr + 42);
                e->flags = (short)Le16(hdr + 8);
                e->method = (short)Le16(hdr + 10);
                e->compSize = Le32(hdr + 20);
                e->size = Le32(hdr + 24);
                if (ReadExact(src, lh, loc, 30) != ZIP_OK)
                    return ZIP_ERR_READ;
                if (!(loc[0] == 'P' && loc[1] == 'K' && loc[2] == 3 && loc[3] == 4))
                    return ZIP_ERR_NOTZIP;
                e->dataOff = lh + 30 + Le16(loc + 26) + Le16(loc + 28);
                if (e->dataOff + e->compSize > src->size)
                    return ZIP_ERR_READ;
                return ZIP_OK;
            }
        }
        off += 46 + (wr_u32)(nlen + xlen + clen);
    }
    return ZIP_ERR_NOENTRY;
}

/* ---- streaming the member's bytes ---- */

typedef struct {
    WrSource *src;
    wr_u32    pos, end;
} Range;

static long RangeRead(void *ctx, wr_u8 *buf, long max)
{
    Range *r = (Range *)ctx;
    long n;
    if (r->pos >= r->end)
        return 0;
    n = (long)(r->end - r->pos);
    if (n > max)
        n = max;
    n = r->src->readAt(r->src->ctx, r->pos, buf, n);
    if (n <= 0)
        return -1;
    r->pos += (wr_u32)n;
    return n;
}

short zip_extract(WrSource *src, const ZipEntry *e, WrWriteFn wr, void *wctx)
{
    Range r;
    short err;

    if (e->flags & 1)
        return ZIP_ERR_CRYPT;
    r.src = src;
    r.pos = e->dataOff;
    r.end = e->dataOff + e->compSize;

    if (e->method == 0) {
        wr_u8 buf[1024];
        long n;
        while ((n = RangeRead(&r, buf, sizeof(buf))) > 0)
            if (wr(wctx, buf, n) != 0)
                return ZIP_ERR_STOPPED;
        return n < 0 ? ZIP_ERR_READ : ZIP_OK;
    }
    if (e->method != 8)
        return ZIP_ERR_METHOD;

    err = inflate_stream(RangeRead, &r, wr, wctx);
    switch (err) {
    case INF_OK:          return ZIP_OK;
    case INF_ERR_MEM:     return ZIP_ERR_MEM;
    case INF_ERR_READ:    return ZIP_ERR_READ;
    case INF_ERR_STOPPED: return ZIP_ERR_STOPPED;
    default:              return ZIP_ERR_DATA;
    }
}
