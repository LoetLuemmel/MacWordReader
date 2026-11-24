/*
 * image.c - PNG and JPEG to grey, streamed and scaled on the way.
 *
 * Both decoders pull bytes from a WrSource through a small buffered reader
 * and hand finished source rows to a box-filter scaler, which emits the
 * output rows. Peak memory is a few source rows plus, for PNG, the inflater's
 * 32 KB window - never the whole picture (except an interlaced PNG, whose
 * passes need a full grey frame; that is allowed up to kMaxFrame bytes).
 */
#include "image.h"
#include "inflate.h"

#define kMaxDim   16384L
#define kMaxFrame 200000L


/* ---------- buffered reader ---------- */

typedef struct {
    WrSource *s;
    wr_u32 pos;               /* file offset just after buf[n - 1] */
    long   n, i;
    short  err;
    wr_u8  buf[2048];
} Rd;

static void RdInit(Rd *r, WrSource *s)
{
    r->s = s;
    r->pos = 0;
    r->n = r->i = 0;
    r->err = 0;
}

static long RdFill(Rd *r)
{
    long want = (long)sizeof(r->buf), got;
    r->i = r->n = 0;
    if (r->err || r->pos >= r->s->size)
        return 0;
    if ((wr_u32)want > r->s->size - r->pos)
        want = (long)(r->s->size - r->pos);
    got = r->s->readAt(r->s->ctx, r->pos, r->buf, want);
    if (got <= 0) {
        r->err = IMG_ERR_READ;
        return 0;
    }
    r->pos += (wr_u32)got;
    r->n = got;
    return got;
}

/* the next byte, or -1 at the end */
static long RdByte(Rd *r)
{
    if (r->i >= r->n && RdFill(r) == 0)
        return -1;
    return r->buf[r->i++];
}

static long RdBytes(Rd *r, wr_u8 *d, long n)
{
    long k = 0, m;
    while (k < n) {
        if (r->i >= r->n && RdFill(r) == 0)
            break;
        m = r->n - r->i;
        if (m > n - k)
            m = n - k;
        WR_MEMCPY(d + k, r->buf + r->i, m);
        r->i += m;
        k += m;
    }
    return k;
}

static void RdSkip(Rd *r, wr_u32 n)
{
    wr_u32 have = (wr_u32)(r->n - r->i);
    if (have >= n) {
        r->i += (long)n;
        return;
    }
    n -= have;
    r->i = r->n = 0;
    r->pos = (n > r->s->size - r->pos) ? r->s->size : r->pos + n;
}

static wr_u32 Be32(const wr_u8 *b)
{
    return ((wr_u32)b[0] << 24) | ((wr_u32)b[1] << 16) | ((wr_u32)b[2] << 8) | b[3];
}

static wr_u16 Be16(const wr_u8 *b)
{
    return (wr_u16)((b[0] << 8) | b[1]);
}

static wr_u32 Le32(const wr_u8 *b)
{
    return ((wr_u32)b[3] << 24) | ((wr_u32)b[2] << 16) | ((wr_u32)b[1] << 8) | b[0];
}

static wr_u16 Le16(const wr_u8 *b)
{
    return (wr_u16)((b[1] << 8) | b[0]);
}

/* ---------- box-filter scaler ---------- */

/* Source rows in, output rows out. Shrinking averages every source pixel
   into its output pixel; enlarging repeats them. */
typedef struct {
    long   sw, sh, ow, oh;
    wr_u32 *acc;
    long   *cnt;              /* source columns per output column */
    wr_u16 recip[64];         /* 65536 / d for small d: no divide per pixel */
    wr_u8  *out;
    long   y, rows;           /* next source row; rows summed in acc */
    long   emitted;           /* output rows delivered */
    ImgRowFn fn;
    void  *ctx;
    short  stopped;
} Scaler;

static void ScFree(Scaler *s)
{
    if (s->acc) WR_FREE(s->acc);
    if (s->cnt) WR_FREE(s->cnt);
    if (s->out) WR_FREE(s->out);
    s->acc = 0;
    s->cnt = 0;
    s->out = 0;
}

static short ScInit(Scaler *s, long sw, long sh, long ow, long oh, ImgRowFn fn, void *ctx)
{
    long x, ox, e;
    s->sw = sw;
    s->sh = sh;
    s->ow = ow;
    s->oh = oh;
    s->y = s->rows = s->emitted = 0;
    s->fn = fn;
    s->ctx = ctx;
    s->stopped = 0;
    /* each output pixel sums at most 2^24 source pixels: no overflow */
    if ((sw / ow + 1) * (sh / oh + 1) > 0x1000000L)
        return IMG_ERR_SIZE;
    s->acc = (wr_u32 *)WR_ALLOC(ow * (long)sizeof(wr_u32));
    s->cnt = (long *)WR_ALLOC(ow * (long)sizeof(long));
    s->out = (wr_u8 *)WR_ALLOC(ow);
    if (s->acc == 0 || s->cnt == 0 || s->out == 0) {
        ScFree(s);
        return IMG_ERR_MEM;
    }
    for (x = 2; x < 64; x++)
        s->recip[x] = (wr_u16)((65536L + x - 1) / x);
    if (sw >= ow) {
        for (x = 0, ox = 0, e = 0; x < sw; x++) {
            s->cnt[ox]++;
            e += ow;
            while (e >= sw) {
                e -= sw;
                ox++;
            }
        }
    } else {
        for (ox = 0; ox < ow; ox++)
            s->cnt[ox] = 1;
    }
    return IMG_OK;
}

static void ScEmit(Scaler *s, long a, long b)
{
    long ox, oy;
    for (ox = 0; ox < s->ow; ox++) {
        wr_u32 d = (wr_u32)s->cnt[ox] * (wr_u32)s->rows, a = s->acc[ox];
        if (d == 1)
            s->out[ox] = (wr_u8)a;
        else if (d < 64)                        /* a < 255 * 64: fits 16 bits */
            s->out[ox] = (wr_u8)(((wr_u32)(wr_u16)a * s->recip[d] + 32767) >> 16);
        else
            s->out[ox] = (wr_u8)(d ? a / d : 255);
        s->acc[ox] = 0;
    }
    s->rows = 0;
    for (oy = a; oy < b && oy < s->oh && !s->stopped; oy++) {
        if (s->fn(s->ctx, (short)oy, s->out, (short)s->ow))
            s->stopped = 1;
        s->emitted++;
    }
}

static void ScRow(Scaler *s, const wr_u8 *g)
{
    long x, ox, e, y, a, b;
    if (s->stopped || s->y >= s->sh)
        return;
    if (s->sw >= s->ow) {
        for (x = 0, ox = 0, e = 0; x < s->sw; x++) {
            s->acc[ox] += g[x];
            e += s->ow;
            while (e >= s->sw) {
                e -= s->sw;
                ox++;
            }
        }
    } else {
        for (ox = 0, x = 0, e = 0; ox < s->ow; ox++) {
            s->acc[ox] += g[x];
            e += s->sw;
            while (e >= s->ow) {
                e -= s->ow;
                x++;
            }
        }
    }
    s->rows++;
    y = s->y++;
    if (s->sh >= s->oh) {
        a = y * s->oh / s->sh;
        b = (y + 1) * s->oh / s->sh;
        if (a != b || y + 1 == s->sh)
            ScEmit(s, a, a + 1);
    } else {
        a = (y * s->oh + s->sh - 1) / s->sh;
        b = ((y + 1) * s->oh + s->sh - 1) / s->sh;
        ScEmit(s, a, b);
    }
}

static wr_u8 Lum(long r, long g, long b)
{
    return (wr_u8)((77 * r + 150 * g + 29 * b) >> 8);
}

/* grey v with alpha a laid over white paper */
static wr_u8 OverWhite(long v, long a)
{
    return (wr_u8)((v * a + 255 * (255 - a) + 127) / 255);
}

/* ---------- PNG ---------- */

static const wr_u8 kAdamX[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const wr_u8 kAdamY[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const wr_u8 kAdamDX[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const wr_u8 kAdamDY[7] = { 8, 8, 8, 4, 4, 2, 2 };

typedef struct {
    Rd     rd;
    long   left;              /* bytes left in the current IDAT chunk */
    short  zskip;             /* zlib header bytes still to skip */
    short  end;               /* no more IDAT data */
    long   w, h;
    short  depth, ctype, interlace, chans, bpp;
    wr_u8 *cur, *prev;        /* filter byte + one row each */
    long   fill;              /* bytes of cur filled */
    short  pass;              /* Adam7 pass 0..6 (0 only, if not interlaced); 7 done */
    long   pw, ph, py, prb;   /* this pass: width, height, row, row bytes */
    wr_u8  pal[256];          /* palette entries as grey over white */
    short  trns;              /* colour key present */
    wr_u16 key[3];
    wr_u8 *grey;              /* one converted row */
    wr_u8 *full;              /* interlaced: the whole picture */
    Scaler sc;
} Png;

static long PngRead(void *ctx, wr_u8 *buf, long max)
{
    Png *g = (Png *)ctx;
    wr_u8 h[12];
    long got;
    for (;;) {
        if (g->end)
            return 0;
        if (g->left == 0) {                     /* CRC, then the next chunk's header */
            if (RdBytes(&g->rd, h, 12) != 12 ||
                !(h[8] == 'I' && h[9] == 'D' && h[10] == 'A' && h[11] == 'T')) {
                g->end = 1;
                return 0;
            }
            g->left = (long)(Be32(h + 4) & 0x7FFFFFFFL);
            continue;
        }
        while (g->zskip > 0 && g->left > 0) {
            if (RdByte(&g->rd) < 0) {
                g->end = 1;
                return 0;
            }
            g->zskip--;
            g->left--;
        }
        if (g->left == 0)
            continue;
        got = RdBytes(&g->rd, buf, max < g->left ? max : g->left);
        if (got <= 0) {
            g->end = 1;
            return g->rd.err ? -1 : 0;
        }
        g->left -= got;
        return got;
    }
}

static void PngPass(Png *g)
{
    long k;
    for (; g->pass < 7; g->pass++) {
        if (g->interlace) {
            g->pw = (g->w - kAdamX[g->pass] + kAdamDX[g->pass] - 1) / kAdamDX[g->pass];
            g->ph = (g->h - kAdamY[g->pass] + kAdamDY[g->pass] - 1) / kAdamDY[g->pass];
        } else if (g->pass == 0) {
            g->pw = g->w;
            g->ph = g->h;
        } else {
            g->pw = g->ph = 0;
        }
        if (g->pw > 0 && g->ph > 0)
            break;
    }
    g->py = 0;
    g->fill = 0;
    g->prb = (g->pw * g->chans * g->depth + 7) / 8;
    for (k = 0; k <= g->prb && g->pass < 7; k++)
        g->prev[k] = 0;
}

static short Unfilter(wr_u8 *c, const wr_u8 *p, long n, short bpp)
{
    wr_u8 *x = c + 1;
    const wr_u8 *u = p + 1;
    long i;
    switch (c[0]) {
    case 0:
        break;
    case 1:
        for (i = bpp; i < n; i++)
            x[i] = (wr_u8)(x[i] + x[i - bpp]);
        break;
    case 2:
        for (i = 0; i < n; i++)
            x[i] = (wr_u8)(x[i] + u[i]);
        break;
    case 3:
        for (i = 0; i < n; i++)
            x[i] = (wr_u8)(x[i] + (((i >= bpp ? x[i - bpp] : 0) + u[i]) >> 1));
        break;
    case 4:
        for (i = 0; i < n; i++) {
            short a = (short)(i >= bpp ? x[i - bpp] : 0), b = u[i];
            short cc = (short)(i >= bpp ? u[i - bpp] : 0);
            short pp = (short)(a + b - cc), pa, pb, pc;
            pa = (short)(pp > a ? pp - a : a - pp);
            pb = (short)(pp > b ? pp - b : b - pp);
            pc = (short)(pp > cc ? pp - cc : cc - pp);
            x[i] = (wr_u8)(x[i] + ((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : cc));
        }
        break;
    default:
        return 1;
    }
    return 0;
}

static long Sample(const wr_u8 *d, long x, short depth)
{
    if (depth == 8)
        return d[x];
    if (depth == 16)
        return ((long)d[2 * x] << 8) | d[2 * x + 1];
    return (d[(x * depth) >> 3] >> (8 - depth - ((x * depth) & 7))) & ((1 << depth) - 1);
}

static void PngGrey(Png *g, const wr_u8 *d, long n, wr_u8 *o)
{
    long x, v, r, gg, b, a;
    short dp = g->depth;
    for (x = 0; x < n; x++) {
        switch (g->ctype) {
        case 0:
            v = Sample(d, x, dp);
            if (g->trns && v == (long)g->key[0])
                o[x] = 255;
            else
                o[x] = (wr_u8)(dp == 16 ? v >> 8 : v * 255 / ((1L << dp) - 1));
            break;
        case 2:
            r = Sample(d, 3 * x, dp);
            gg = Sample(d, 3 * x + 1, dp);
            b = Sample(d, 3 * x + 2, dp);
            if (g->trns && r == (long)g->key[0] && gg == (long)g->key[1] && b == (long)g->key[2])
                o[x] = 255;
            else if (dp == 16)
                o[x] = Lum(r >> 8, gg >> 8, b >> 8);
            else
                o[x] = Lum(r, gg, b);
            break;
        case 3:
            o[x] = g->pal[Sample(d, x, dp) & 255];
            break;
        case 4:
            v = Sample(d, 2 * x, dp);
            a = Sample(d, 2 * x + 1, dp);
            if (dp == 16) { v >>= 8; a >>= 8; }
            o[x] = OverWhite(v, a);
            break;
        default:   /* 6 */
            r = Sample(d, 4 * x, dp);
            gg = Sample(d, 4 * x + 1, dp);
            b = Sample(d, 4 * x + 2, dp);
            a = Sample(d, 4 * x + 3, dp);
            if (dp == 16) { r >>= 8; gg >>= 8; b >>= 8; a >>= 8; }
            o[x] = OverWhite(Lum(r, gg, b), a);
            break;
        }
    }
}

/* inflated bytes arrive here: collect rows, unfilter, convert, scale */
static long PngWrite(void *ctx, const wr_u8 *b, long n)
{
    Png *g = (Png *)ctx;
    long m, x, y;
    wr_u8 *t;
    while (n > 0) {
        if (g->pass >= 7)
            return 0;                           /* surplus data: ignored */
        m = g->prb + 1 - g->fill;
        if (m > n)
            m = n;
        WR_MEMCPY(g->cur + g->fill, b, m);
        g->fill += m;
        b += m;
        n -= m;
        if (g->fill < g->prb + 1)
            break;
        if (Unfilter(g->cur, g->prev, g->prb, g->bpp))
            return 1;
        PngGrey(g, g->cur + 1, g->pw, g->grey);
        if (!g->interlace) {
            ScRow(&g->sc, g->grey);
            if (g->sc.stopped)
                return 1;
        } else {
            y = kAdamY[g->pass] + g->py * kAdamDY[g->pass];
            for (x = 0; x < g->pw; x++)
                g->full[y * g->w + kAdamX[g->pass] + x * kAdamDX[g->pass]] = g->grey[x];
        }
        t = g->cur;
        g->cur = g->prev;
        g->prev = t;
        g->fill = 0;
        if (++g->py >= g->ph) {
            g->pass++;
            PngPass(g);
            if (g->pass >= 7 && g->interlace)
                for (y = 0; y < g->h && !g->sc.stopped; y++)
                    ScRow(&g->sc, g->full + y * g->w);
        }
    }
    return 0;
}

static short PngDecode(WrSource *s, short outW, short outH, ImgRowFn fn, void *ctx)
{
    Png *g;
    wr_u8 h[16], plte[768], alpha[256];
    long len, k, nPal = 0, nAlpha = 0;
    short err = IMG_OK, haveHdr = 0;

    g = (Png *)WR_ALLOC(sizeof(Png));
    if (g == 0)
        return IMG_ERR_MEM;
    RdInit(&g->rd, s);
    if (RdBytes(&g->rd, h, 8) != 8 || h[0] != 0x89 || h[1] != 'P' || h[2] != 'N' || h[3] != 'G') {
        WR_FREE(g);
        return IMG_ERR_DATA;
    }
    for (k = 0; k < 256; k++)
        alpha[k] = 255;
    for (;;) {                                  /* chunks up to the first IDAT */
        if (RdBytes(&g->rd, h, 8) != 8) {
            err = IMG_ERR_DATA;
            break;
        }
        len = (long)(Be32(h) & 0x7FFFFFFFL);
        if (h[4] == 'I' && h[5] == 'H' && h[6] == 'D' && h[7] == 'R' && len == 13) {
            if (RdBytes(&g->rd, h, 13) != 13) {
                err = IMG_ERR_DATA;
                break;
            }
            g->w = (long)Be32(h);
            g->h = (long)Be32(h + 4);
            g->depth = h[8];
            g->ctype = h[9];
            g->interlace = h[12];
            haveHdr = 1;
            RdSkip(&g->rd, 4);
            continue;
        }
        if (h[4] == 'I' && h[5] == 'D' && h[6] == 'A' && h[7] == 'T') {
            g->left = len;
            break;
        }
        if (h[4] == 'I' && h[5] == 'E' && h[6] == 'N' && h[7] == 'D') {
            err = IMG_ERR_DATA;
            break;
        }
        if (h[4] == 'P' && h[5] == 'L' && h[6] == 'T' && h[7] == 'E' && len <= 768) {
            nPal = len / 3;
            RdBytes(&g->rd, plte, len);
            RdSkip(&g->rd, 4);
            continue;
        }
        if (h[4] == 't' && h[5] == 'R' && h[6] == 'N' && h[7] == 'S' && len <= 256) {
            wr_u8 t[256];
            RdBytes(&g->rd, t, len);
            if (g->ctype == 3) {
                nAlpha = len;
                for (k = 0; k < len; k++)
                    alpha[k] = t[k];
            } else if (g->ctype == 0 && len >= 2) {
                g->trns = 1;
                g->key[0] = Be16(t);
            } else if (g->ctype == 2 && len >= 6) {
                g->trns = 1;
                g->key[0] = Be16(t);
                g->key[1] = Be16(t + 2);
                g->key[2] = Be16(t + 4);
            }
            RdSkip(&g->rd, 4);
            continue;
        }
        RdSkip(&g->rd, (wr_u32)len + 4);
        if (g->rd.err || g->rd.pos >= s->size) {
            err = IMG_ERR_DATA;
            break;
        }
    }
    if (err == IMG_OK && !haveHdr)
        err = IMG_ERR_DATA;
    if (err == IMG_OK) {
        short d = g->depth, c = g->ctype;
        g->chans = (short)(c == 0 || c == 3 ? 1 : c == 2 ? 3 : c == 4 ? 2 : c == 6 ? 4 : 0);
        if (g->chans == 0 || g->interlace > 1 ||
            !(d == 1 || d == 2 || d == 4 || d == 8 || d == 16) ||
            (c == 3 && d == 16) || ((c == 2 || c == 4 || c == 6) && d < 8))
            err = IMG_ERR_DATA;
        else if (g->w < 1 || g->h < 1 || g->w > kMaxDim || g->h > kMaxDim)
            err = IMG_ERR_SIZE;
        else if (g->interlace && g->w * g->h > kMaxFrame)
            err = IMG_ERR_SIZE;
    }
    if (err == IMG_OK) {
        long rb = (g->w * g->chans * g->depth + 7) / 8;
        g->bpp = (short)((g->chans * g->depth + 7) / 8);
        for (k = 0; k < 256; k++) {
            long v = 0;
            if (k < nPal)
                v = Lum(plte[3 * k], plte[3 * k + 1], plte[3 * k + 2]);
            g->pal[k] = OverWhite(v, k < nAlpha ? alpha[k] : 255);
        }
        g->cur = (wr_u8 *)WR_ALLOC(rb + 1);
        g->prev = (wr_u8 *)WR_ALLOC(rb + 1);
        g->grey = (wr_u8 *)WR_ALLOC(g->w);
        if (g->interlace)
            g->full = (wr_u8 *)WR_ALLOC(g->w * g->h);
        if (g->cur == 0 || g->prev == 0 || g->grey == 0 || (g->interlace && g->full == 0))
            err = IMG_ERR_MEM;
        else
            err = ScInit(&g->sc, g->w, g->h, outW, outH, fn, ctx);
    }
    if (err == IMG_OK) {
        short ie;
        g->zskip = 2;                           /* the zlib header */
        g->pass = 0;
        PngPass(g);
        ie = inflate_stream(PngRead, g, PngWrite, g);
        if (g->sc.stopped)
            err = IMG_ERR_STOPPED;
        else if (g->pass < 7)
            err = ie == INF_ERR_MEM ? IMG_ERR_MEM : g->rd.err ? IMG_ERR_READ : IMG_ERR_DATA;
        ScFree(&g->sc);
    }
    if (g->cur) WR_FREE(g->cur);
    if (g->prev) WR_FREE(g->prev);
    if (g->grey) WR_FREE(g->grey);
    if (g->full) WR_FREE(g->full);
    WR_FREE(g);
    return err;
}

/* ---------- JPEG ---------- */

/* Baseline (and the first, DC-only scan of a progressive file). Only the
   luminance is needed for grey, so colour blocks are Huffman-decoded and
   dropped. Big pictures are reduced inside the transform: 1/8 size needs
   just each block's DC value, 1/4 and 1/2 a 2x2 or 4x4 inverse DCT. */

static const wr_u8 kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    short  fast[512];         /* 9-bit lookahead: (length << 8) | symbol; 0 = longer */
    wr_u8  acrl[512];         /* AC: code and value both within the 9 bits: */
    signed char acv[512];     /*   (run << 4) | total length, and the value */
    long   maxcode[18], valptr[17], mincode[17];
    wr_u8  vals[256];
    short  ok;
} Huff;

typedef struct {
    short id, h, v, tq, td, ta;
    long  pred;               /* DC predictor */
} Comp;

typedef struct {
    Rd     rd;
    Huff   dc[4], ac[4];
    wr_u16 q[4][64];          /* zigzag order */
    Comp   c[4];
    short  nc, hmax, vmax, progressive, al;
    long   w, h, ri;
    wr_u32 bits;              /* bit reader: MSB first */
    short  nbits, marker, bad;
    long   coef[64];
    short  bs;                /* pixels per block side in the output: 1, 2, 4, 8 */
    wr_u8  keep[64];          /* zigzag index -> coefficient used at this bs */
    wr_u8 *plane;             /* luminance of one MCU row, at the reduced size */
    long   planeW;
    Scaler sc;
} Jpeg;

static short BuildHuff(Huff *t, const wr_u8 *counts, const wr_u8 *vals, short n)
{
    long code = 0, a, b, j;
    short k = 0, l, i;
    t->ok = 0;
    for (i = 0; i < 512; i++)
        t->fast[i] = 0;
    for (l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        for (i = 0; i < counts[l - 1]; i++, k++, code++) {
            if (k >= n || code >= (1L << l))
                return 1;
            if (l <= 9)
                for (a = code << (9 - l), b = (code + 1) << (9 - l), j = a; j < b; j++)
                    t->fast[j] = (short)((l << 8) | vals[k]);
        }
        t->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFFL;
    for (i = 0; i < n; i++)
        t->vals[i] = vals[i];
    for (j = 0; j < 512; j++) {                 /* short AC codes with their value */
        short e = t->fast[j], s, run, m, v;
        t->acrl[j] = 0;
        if (e == 0)
            continue;
        l = (short)(e >> 8);
        s = (short)(e & 15);
        run = (short)((e >> 4) & 15);
        if (s == 0 || l + s > 9)
            continue;
        m = (short)((j >> (9 - l - s)) & ((1 << s) - 1));
        v = (short)(m < (1 << (s - 1)) ? m - (1 << s) + 1 : m);
        if (v < -128 || v > 127)
            continue;
        t->acv[j] = (signed char)v;
        t->acrl[j] = (wr_u8)((run << 4) | (l + s));
    }
    t->ok = 1;
    return 0;
}

/* Keep at least 25 bits in the reader. A marker ends the entropy data:
   from there on zeros come in, and the marker waits in j->marker. */
static void JFill(Jpeg *j)
{
    Rd *r = &j->rd;
    wr_u32 bits = j->bits;
    short nb = j->nbits;
    long c, d;
    while (nb <= 24) {
        c = 0;
        if (!j->marker) {
            if (r->i < r->n && r->buf[r->i] != 0xFF)
                c = r->buf[r->i++];             /* the usual case */
            else {
                c = RdByte(r);
                if (c < 0) {
                    j->marker = 0xD9;
                    c = 0;
                } else if (c == 0xFF) {
                    d = RdByte(r);
                    while (d == 0xFF)
                        d = RdByte(r);
                    if (d == 0)
                        c = 0xFF;
                    else {
                        j->marker = (short)(d < 0 ? 0xD9 : d);
                        c = 0;
                    }
                }
            }
        }
        bits |= (wr_u32)c << (24 - nb);
        nb += 8;
    }
    j->bits = bits;
    j->nbits = nb;
}

/* A code longer than the 9-bit lookahead (bits holds at least 16). */
static short JDecodeSlow(Jpeg *j, const Huff *t)
{
    short l;
    long code, idx;
    for (l = 10; l <= 16; l++) {
        code = (long)(j->bits >> (32 - l));
        if (code <= t->maxcode[l]) {
            idx = t->valptr[l] + code - t->mincode[l];
            j->bits = (j->bits << l) & 0xFFFFFFFFUL;
            j->nbits = (short)(j->nbits - l);
            if (idx < 0 || idx > 255)
                break;
            return t->vals[idx];
        }
    }
    j->bad = 1;
    return 0;
}

static long Clamp32k(long v)
{
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}

/* 16 x 16 bit multiply: inline on a 68000 (no call to the 32-bit routine) */
#define MUL16(a, b) ((long)(short)(a) * (short)(b))

static short S16(long v)
{
    return (short)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* The entropy decoder works on local copies of the bit buffer - registers
   on the 68k - and hands them back to j only to refill or to decode a long
   code. JNEED(n): at least n bits in hand. JSYM(t, out): one Huffman symbol.
   JTAKE(s, out): the next s bits as a signed value (s = 1..16). */
#define JSYNC() (j->bits = bits, j->nbits = nb)
#define JLOAD() (bits = j->bits, nb = j->nbits)
#define JNEED(n) if (nb < (n)) { JSYNC(); JFill(j); JLOAD(); }
#define JSYM(t, out) \
    JNEED(16) \
    e = (t)->fast[bits >> 23]; \
    if (e) { \
        l = (short)(e >> 8); \
        bits = (bits << l) & 0xFFFFFFFFUL; \
        nb = (short)(nb - l); \
        out = (short)(e & 0xFF); \
    } else { \
        JSYNC(); out = JDecodeSlow(j, t); JLOAD(); \
    }
#define JTAKE(s, out) \
    JNEED(s) \
    out = (long)(bits >> (32 - (s))); \
    bits = (bits << (s)) & 0xFFFFFFFFUL; \
    nb = (short)(nb - (s)); \
    if (out < (1L << ((s) - 1))) \
        out += 1 - (1L << (s));

/* One block. need: 0 = entropy only (colour), 1 = DC only, 2 = all */
static void JBlock(Jpeg *j, Comp *c, short need)
{
    const wr_u16 *q = j->q[c->tq];
    const Huff *ac = &j->ac[c->ta];
    const wr_u8 *keep = j->keep;
    long *coef = j->coef;
    wr_u32 bits;
    short nb, t, k, rs, s, e, l;
    long v = 0;
    JLOAD();
    JSYM(&j->dc[c->td], t)
    if (t > 16) {
        j->bad = 1;
        JSYNC();
        return;
    }
    if (t) {
        JTAKE(t, v)
        c->pred += v;
    }
    if (c->pred > 0xFFFFL || c->pred < -0xFFFFL)
        c->pred = 0;                            /* damaged: keep the sums small */
    if (need == 2) {
        if (j->bs == 8)
            for (k = 0; k < 64; k++)
                coef[k] = 0;
        else                                    /* only the corner is read */
            for (k = 0; k < 8 * j->bs; k += 8)
                coef[k] = coef[k + 1] = coef[k + 2] = coef[k + 3] = 0;
    }
    if (need)
        coef[0] = Clamp32k(MUL16(S16(j->al ? c->pred * (1L << j->al) : c->pred), q[0]));
    if (j->progressive) {
        JSYNC();
        return;                                 /* the DC scan has no AC values */
    }
    for (k = 1; k < 64; ) {
        JNEED(16)
        e = ac->acrl[bits >> 23];
        if (e) {                                /* run, value, length at once */
            l = (short)(e & 15);
            k = (short)(k + (e >> 4));
            v = ac->acv[bits >> 23];
            bits = (bits << l) & 0xFFFFFFFFUL;
            nb = (short)(nb - l);
            if (k > 63) {
                j->bad = 1;
                break;
            }
            if (need == 2 && keep[k])
                coef[kZigzag[k]] = Clamp32k(MUL16(v, q[k]));
            k++;
            continue;
        }
        JSYM(ac, rs)
        s = (short)(rs & 15);
        if (s == 0) {
            if (rs != 0xF0)
                break;                          /* end of block */
            k += 16;
            continue;
        }
        k = (short)(k + (rs >> 4));
        if (k > 63) {
            j->bad = 1;
            break;
        }
        JTAKE(s, v)
        if (need == 2 && keep[k])
            coef[kZigzag[k]] = Clamp32k(MUL16(v, q[k]));
        k++;
    }
    JSYNC();
}

static wr_u8 Px8(long v)
{
    return (wr_u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* the whole 8x8 inverse DCT (the integer form of libjpeg's "islow", as
   in stb_image: 12-bit constants, 2 extra bits kept between the passes) */
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * 2217L; t2 = p1 + p3 * -7568L; t3 = p1 + p2 * 3135L; \
    p2 = s0; p3 = s4; \
    t0 = (p2 + p3) * 4096L; t1 = (p2 - p3) * 4096L; \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * 4816L; \
    t0 = t0 * 1223L; t1 = t1 * 8410L; t2 = t2 * 12586L; t3 = t3 * 6149L; \
    p1 = p5 + p1 * -3686L; p2 = p5 + p2 * -10498L; \
    p3 = p3 * -8035L; p4 = p4 * -1598L; \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static void Idct8(const long *in, wr_u8 *out, long stride)
{
    long val[64], i, t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
    const long *d;
    long *v;
    wr_u8 *o;
    for (i = 0; i < 8; i++) {                   /* columns */
        d = in + i;
        v = val + i;
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 &&
            d[40] == 0 && d[48] == 0 && d[56] == 0) {
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = d[0] * 4;
            continue;
        }
        IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        v[0] = Clamp32k((x0 + t3) >> 10);
        v[56] = Clamp32k((x0 - t3) >> 10);
        v[8] = Clamp32k((x1 + t2) >> 10);
        v[48] = Clamp32k((x1 - t2) >> 10);
        v[16] = Clamp32k((x2 + t1) >> 10);
        v[40] = Clamp32k((x2 - t1) >> 10);
        v[24] = Clamp32k((x3 + t0) >> 10);
        v[32] = Clamp32k((x3 - t0) >> 10);
    }
    for (i = 0; i < 8; i++) {                   /* rows */
        v = val + i * 8;
        o = out + i * stride;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536L + (128L << 17); x1 += 65536L + (128L << 17);
        x2 += 65536L + (128L << 17); x3 += 65536L + (128L << 17);
        o[0] = Px8((x0 + t3) >> 17);
        o[7] = Px8((x0 - t3) >> 17);
        o[1] = Px8((x1 + t2) >> 17);
        o[6] = Px8((x1 - t2) >> 17);
        o[2] = Px8((x2 + t1) >> 17);
        o[5] = Px8((x2 - t1) >> 17);
        o[3] = Px8((x3 + t0) >> 17);
        o[4] = Px8((x3 - t0) >> 17);
    }
}

/* n x n pixels (n = 2 or 4) from the block's n x n lowest frequencies.
   The 4-point inverse DCT as a butterfly: the even half needs no multiply,
   the odd half four 16 x 16 bit ones (MULS.W - a 68000 has no 32-bit
   multiply, and the library call costs ten times as much). Both passes
   carry the factor 1448/4096 = 1/(2 sqrt 2), together exactly 1/8, so
   that comes out as shifts: pass 1 keeps half, pass 2 divides by 4. */
#define kC1 5352              /* 4096 * sqrt 2 * cos(pi/8) */
#define kC3 2217              /* 4096 * sqrt 2 * sin(pi/8) */

/* in place: the hot loops below avoid a call per value */
#define CLAMP16(v) if (v > 32767) v = 32767; else if (v < -32767) v = -32767
#define CLAMP8(v) if (v < 0) v = 0; else if (v > 255) v = 255

static void IdctSmall(const long *in, wr_u8 *out, long stride, short n)
{
    short tmp[16], y, a0, a1, a2, a3;
    long e0, e1, o0, o1, r0, r1, r2, r3;
    const long *d;
    const short *t;
    if (n == 2) {
        long p = in[0] + in[1], m = in[0] - in[1];
        long q = in[8] + in[9], r = in[8] - in[9];
        out[0] = Px8(((p + q + 4) >> 3) + 128);
        out[1] = Px8(((m + r + 4) >> 3) + 128);
        out[stride] = Px8(((p - q + 4) >> 3) + 128);
        out[stride + 1] = Px8(((m - r + 4) >> 3) + 128);
        return;
    }
    for (y = 0; y < 4; y++) {                   /* rows of coefficients */
        d = in + y * 8;
        a0 = S16(d[0]);
        if ((d[1] | d[2] | d[3]) == 0) {
            tmp[y * 4] = tmp[y * 4 + 1] = tmp[y * 4 + 2] = tmp[y * 4 + 3] = (short)(a0 >> 1);
            continue;
        }
        a1 = S16(d[1]); a2 = S16(d[2]); a3 = S16(d[3]);
        e0 = (long)a0 + a2;
        e1 = (long)a0 - a2;
        o0 = (MUL16(a1, kC1) + MUL16(a3, kC3) + 2048) >> 12;
        o1 = (MUL16(a1, kC3) - MUL16(a3, kC1) + 2048) >> 12;
        r0 = (e0 + o0) >> 1; CLAMP16(r0);
        r1 = (e1 + o1) >> 1; CLAMP16(r1);
        r2 = (e1 - o1) >> 1; CLAMP16(r2);
        r3 = (e0 - o0) >> 1; CLAMP16(r3);
        tmp[y * 4] = (short)r0;
        tmp[y * 4 + 1] = (short)r1;
        tmp[y * 4 + 2] = (short)r2;
        tmp[y * 4 + 3] = (short)r3;
    }
    for (y = 0; y < 4; y++) {                   /* columns */
        t = tmp + y;
        a0 = t[0]; a1 = t[4]; a2 = t[8]; a3 = t[12];
        e0 = (long)a0 + a2 + 514;               /* + 2 rounding, + 128 * 4 */
        e1 = (long)a0 - a2 + 514;
        if ((a1 | a3) == 0)
            o0 = o1 = 0;
        else {
            o0 = (MUL16(a1, kC1) + MUL16(a3, kC3) + 2048) >> 12;
            o1 = (MUL16(a1, kC3) - MUL16(a3, kC1) + 2048) >> 12;
        }
        r0 = (e0 + o0) >> 2; CLAMP8(r0);
        r1 = (e1 + o1) >> 2; CLAMP8(r1);
        r2 = (e1 - o1) >> 2; CLAMP8(r2);
        r3 = (e0 - o0) >> 2; CLAMP8(r3);
        out[y] = (wr_u8)r0;
        out[stride + y] = (wr_u8)r1;
        out[2 * stride + y] = (wr_u8)r2;
        out[3 * stride + y] = (wr_u8)r3;
    }
}

/* coefficients -> bs x bs pixels at out */
static void JPixels(Jpeg *j, wr_u8 *out, long stride, short bs)
{
    if (bs == 1) {
        long dc = j->coef[0];
        out[0] = Px8((dc >= 0 ? (dc + 4) / 8 : -((-dc + 4) / 8)) + 128);
    } else if (bs == 8)
        Idct8(j->coef, out, stride);
    else
        IdctSmall(j->coef, out, stride, bs);
}

static void JRestart(Jpeg *j)
{
    long c;
    short k;
    j->bits = 0;
    j->nbits = 0;
    while (!j->marker) {                        /* find the RSTn marker */
        c = RdByte(&j->rd);
        if (c < 0)
            j->marker = 0xD9;
        else if (c == 0xFF) {
            do
                c = RdByte(&j->rd);
            while (c == 0xFF);
            if (c != 0)
                j->marker = (short)(c < 0 ? 0xD9 : c);
        }
    }
    if (j->marker >= 0xD0 && j->marker <= 0xD7)
        j->marker = 0;
    for (k = 0; k < j->nc; k++)
        j->c[k].pred = 0;
}

/* Decode a scan holding the luminance; scan[] are component indexes. */
static short JScan(Jpeg *j, const short *scan, short ns, short outW, short outH, ImgRowFn fn, void *ctx)
{
    Comp *y = &j->c[0];
    long cw = (j->w * y->h + j->hmax - 1) / j->hmax;    /* luminance size */
    long ch = (j->h * y->v + j->vmax - 1) / j->vmax;
    long sw, sh, fed = 0, mx, my, mcuX, mcuY, rows, r, n = 0;
    short sc = 8, bs, err, i, bh, bv;

    /* the smallest reduction still giving 7/8 of the wanted size: a slightly
       enlarged picture looks no worse in 16 greys and costs far less */
    if (!j->progressive)
        while (sc > 1 && (((cw + sc - 1) / sc) * 8 < outW * 7L || ((ch + sc - 1) / sc) * 8 < outH * 7L))
            sc = (short)(sc >> 1);
    bs = (short)(8 / sc);
    j->bs = bs;
    for (i = 0; i < 64; i++)
        j->keep[i] = (wr_u8)((kZigzag[i] & 7) < bs && (kZigzag[i] >> 3) < bs);
    sw = (cw + sc - 1) / sc;
    sh = (ch + sc - 1) / sc;
    if (ns == 1) {                              /* one component: blocks in its own grid */
        mcuX = (cw + 7) / 8;
        mcuY = (ch + 7) / 8;
        bh = bv = 1;
    } else {
        mcuX = (j->w + 8L * j->hmax - 1) / (8L * j->hmax);
        mcuY = (j->h + 8L * j->vmax - 1) / (8L * j->vmax);
        bh = y->h;
        bv = y->v;
    }
    j->planeW = mcuX * bh * bs;
    rows = (long)bv * bs;
    if (j->planeW * rows > 160000L)
        return IMG_ERR_SIZE;
    j->plane = (wr_u8 *)WR_ALLOC(j->planeW * rows);
    if (j->plane == 0)
        return IMG_ERR_MEM;
    err = ScInit(&j->sc, sw, sh, outW, outH, fn, ctx);
    if (err != IMG_OK)
        return err;
    for (my = 0; my < mcuY && !j->bad && !j->sc.stopped; my++) {
        for (mx = 0; mx < mcuX && !j->bad; mx++, n++) {
            if (j->ri && n > 0 && n % j->ri == 0)
                JRestart(j);
            for (i = 0; i < ns; i++) {
                Comp *c = &j->c[scan[i]];
                short hh = ns == 1 ? 1 : c->h, vv = ns == 1 ? 1 : c->v, a, b;
                for (b = 0; b < vv; b++)
                    for (a = 0; a < hh; a++) {
                        if (scan[i] == 0) {
                            JBlock(j, c, (short)(bs == 1 ? 1 : 2));
                            JPixels(j, j->plane + (long)b * bs * j->planeW + (mx * bh + a) * bs,
                                    j->planeW, bs);
                        } else
                            JBlock(j, c, 0);
                    }
            }
        }
        for (r = 0; r < rows && fed < sh; r++, fed++)
            ScRow(&j->sc, j->plane + r * j->planeW);
    }
    if (j->sc.stopped)
        return IMG_ERR_STOPPED;
    return j->bad ? IMG_ERR_DATA : IMG_OK;
}

static short JpegDecode(WrSource *s, short outW, short outH, ImgRowFn fn, void *ctx)
{
    Jpeg *j = (Jpeg *)WR_ALLOC(sizeof(Jpeg));
    wr_u8 h[20], counts[16], vals[256];
    long c, len, k, total;
    short err = IMG_ERR_DATA, i, sof = 0, done = 0;
    if (j == 0)
        return IMG_ERR_MEM;
    RdInit(&j->rd, s);
    RdSkip(&j->rd, 2);                          /* SOI */
    while (!done) {
        c = RdByte(&j->rd);
        if (c != 0xFF)
            break;
        while (c == 0xFF)
            c = RdByte(&j->rd);
        if (c < 0 || c == 0xD9)
            break;
        if (c == 0x01 || (c >= 0xD0 && c <= 0xD7))
            continue;
        if (RdBytes(&j->rd, h, 2) != 2)
            break;
        len = (long)Be16(h) - 2;
        if (len < 0)
            break;
        if (c == 0xC0 || c == 0xC1 || c == 0xC2) {             /* frame */
            if (len < 6 || RdBytes(&j->rd, h, 6) != 6 || h[0] != 8) {
                err = h[0] != 8 ? IMG_ERR_FORMAT : IMG_ERR_DATA;
                break;
            }
            j->h = Be16(h + 1);
            j->w = Be16(h + 3);
            j->nc = h[5];
            j->progressive = (short)(c == 0xC2);
            if (j->nc != 1 && j->nc != 3) {
                err = IMG_ERR_FORMAT;                   /* CMYK and friends */
                break;
            }
            if (j->w < 1 || j->h < 1 || j->w > kMaxDim || j->h > kMaxDim || len < 6 + 3L * j->nc) {
                err = j->w > kMaxDim || j->h > kMaxDim ? IMG_ERR_SIZE : IMG_ERR_DATA;
                break;
            }
            j->hmax = j->vmax = 1;
            for (i = 0; i < j->nc; i++) {
                if (RdBytes(&j->rd, h, 3) != 3)
                    break;
                j->c[i].id = h[0];
                j->c[i].h = (short)(h[1] >> 4);
                j->c[i].v = (short)(h[1] & 15);
                j->c[i].tq = (short)(h[2] & 3);
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4)
                    break;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            if (i < j->nc)
                break;
            RdSkip(&j->rd, (wr_u32)(len - 6 - 3L * j->nc));
            sof = 1;
        } else if (c >= 0xC3 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) {
            err = IMG_ERR_FORMAT;                       /* lossless, arithmetic, ... */
            break;
        } else if (c == 0xC4) {                         /* Huffman tables */
            while (len > 0) {
                long tc;
                if ((tc = RdByte(&j->rd)) < 0 || RdBytes(&j->rd, counts, 16) != 16)
                    break;
                for (total = 0, k = 0; k < 16; k++)
                    total += counts[k];
                if (total > 256 || RdBytes(&j->rd, vals, total) != total)
                    break;
                if (BuildHuff((tc >> 4) ? &j->ac[tc & 3] : &j->dc[tc & 3], counts, vals, (short)total))
                    break;
                len -= 17 + total;
            }
            if (len != 0)
                break;
        } else if (c == 0xDB) {                         /* quantisation tables */
            while (len > 0) {
                long pq = RdByte(&j->rd);
                if (pq < 0)
                    break;
                for (k = 0; k < 64; k++) {
                    long v = RdByte(&j->rd);
                    if (pq >> 4)
                        v = (v << 8) | RdByte(&j->rd);
                    j->q[pq & 3][k] = (wr_u16)(v < 1 ? 1 : v > 32767 ? 32767 : v);
                }
                len -= 65 + ((pq >> 4) ? 64 : 0);
            }
            if (len != 0)
                break;
        } else if (c == 0xDD) {                         /* restart interval */
            if (len < 2 || RdBytes(&j->rd, h, 2) != 2)
                break;
            j->ri = Be16(h);
            RdSkip(&j->rd, (wr_u32)(len - 2));
        } else if (c == 0xDA) {                         /* a scan */
            short ns, scan[4], hasY = 0, ok = 1;
            long ss, se, ahal;
            if (!sof || (ns = (short)RdByte(&j->rd)) < 1 || ns > j->nc || len != 4 + 2L * ns)
                break;
            for (i = 0; i < ns; i++) {
                short k2;
                if (RdBytes(&j->rd, h, 2) != 2)
                    ok = 0;
                for (k2 = 0; k2 < j->nc && j->c[k2].id != h[0]; k2++)
                    ;
                if (k2 >= j->nc)
                    ok = 0;
                else {
                    scan[i] = k2;
                    j->c[k2].td = (short)((h[1] >> 4) & 3);
                    j->c[k2].ta = (short)(h[1] & 3);
                    if (k2 == 0)
                        hasY = 1;
                    if (!j->dc[j->c[k2].td].ok || (!j->progressive && !j->ac[j->c[k2].ta].ok))
                        ok = 0;
                }
            }
            ss = RdByte(&j->rd);
            se = RdByte(&j->rd);
            ahal = RdByte(&j->rd);
            if (!ok || ahal < 0)
                break;
            j->al = (short)(j->progressive ? ahal & 15 : 0);
            if (j->al > 13)
                j->al = 13;                     /* 8-bit JPEG: at most 13 */
            if (hasY && (j->progressive ? (ss == 0 && (ahal >> 4) == 0) : (ss == 0 && se == 63))) {
                err = JScan(j, scan, ns, outW, outH, fn, ctx);
                done = 1;
            } else {
                err = IMG_ERR_FORMAT;   /* colour first, or refinement first: rare */
                break;
            }
        } else {
            RdSkip(&j->rd, (wr_u32)len);
        }
    }
    if (!done && err == IMG_ERR_DATA && j->rd.err)
        err = IMG_ERR_READ;
    ScFree(&j->sc);
    if (j->plane)
        WR_FREE(j->plane);
    WR_FREE(j);
    return err;
}

/* JPEG frame header: size and whether it is progressive */
static short JpegProbe(WrSource *s, ImgInfo *info)
{
    Rd *r = (Rd *)WR_ALLOC(sizeof(Rd));
    long c, len;
    wr_u8 h[8];
    short err = IMG_ERR_DATA;
    if (r == 0)
        return IMG_ERR_MEM;
    RdInit(r, s);
    RdSkip(r, 2);
    for (;;) {
        c = RdByte(r);
        if (c != 0xFF)
            break;
        while (c == 0xFF)
            c = RdByte(r);
        if (c < 0 || c == 0xD9 || c == 0xDA)
            break;
        if (c == 0x01 || (c >= 0xD0 && c <= 0xD7))
            continue;
        if (RdBytes(r, h, 2) != 2)
            break;
        len = Be16(h);
        if (len < 2)
            break;
        if (c >= 0xC0 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) {
            if (RdBytes(r, h, 5) != 5)
                break;
            info->height = Be16(h + 1);
            info->width = Be16(h + 3);
            info->progressive = (short)(c == 0xC2 || c == 0xC6 || c == 0xCA || c == 0xCE);
            err = IMG_OK;
            break;
        }
        RdSkip(r, (wr_u32)(len - 2));
    }
    WR_FREE(r);
    return err;
}

/* ---------- entry points ---------- */

short img_probe(WrSource *s, ImgInfo *info)
{
    wr_u8 h[48];
    long n, i;
    info->type = IMG_NONE;
    info->width = info->height = 0;
    info->progressive = info->interlaced = 0;
    n = s->size < sizeof(h) ? (long)s->size : (long)sizeof(h);
    if (n < 4 || s->readAt(s->ctx, 0, h, n) != n)
        return IMG_ERR_DATA;
    if (n >= 29 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') {
        info->type = IMG_PNG;
        info->width = (long)Be32(h + 16);
        info->height = (long)Be32(h + 20);
        info->interlaced = h[28];
        return IMG_OK;
    }
    if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) {
        info->type = IMG_JPEG;
        return JpegProbe(s, info);
    }
    if (n >= 10 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F' && h[3] == '8') {
        info->type = IMG_GIF;
        info->width = Le16(h + 6);
        info->height = Le16(h + 8);
        return IMG_OK;
    }
    if (n >= 26 && h[0] == 'B' && h[1] == 'M') {
        long ht = (long)Le32(h + 22);
        info->type = IMG_BMP;
        info->width = (long)Le32(h + 18);
        info->height = ht < 0 ? -ht : ht;
        return IMG_OK;
    }
    if ((h[0] == 'I' && h[1] == 'I' && h[2] == 42 && h[3] == 0) ||
        (h[0] == 'M' && h[1] == 'M' && h[2] == 0 && h[3] == 42)) {
        info->type = IMG_TIFF;
        return IMG_OK;
    }
    if (n >= 44 && Le32(h) == 1 && h[40] == ' ' && h[41] == 'E' && h[42] == 'M' && h[43] == 'F') {
        info->type = IMG_EMF;
        return IMG_OK;
    }
    if ((h[0] == 0xD7 && h[1] == 0xCD && h[2] == 0xC6 && h[3] == 0x9A) ||
        ((h[0] == 1 || h[0] == 2) && h[1] == 0 && h[2] == 9 && h[3] == 0)) {
        info->type = IMG_WMF;
        return IMG_OK;
    }
    for (i = 0; i < n && (h[i] == ' ' || h[i] == '\r' || h[i] == '\n' || h[i] == '\t' ||
                          h[i] == 0xEF || h[i] == 0xBB || h[i] == 0xBF); i++)
        ;
    if (i < n && h[i] == '<') {
        info->type = IMG_SVG;
        return IMG_OK;
    }
    return IMG_ERR_FORMAT;
}

short img_decode(WrSource *s, short outW, short outH, ImgRowFn row, void *ctx)
{
    ImgInfo info;
    short err;
    if (outW < 1 || outH < 1)
        return IMG_ERR_SIZE;
    err = img_probe(s, &info);
    if (err != IMG_OK)
        return err;
    if (info.type == IMG_PNG)
        return PngDecode(s, outW, outH, row, ctx);
    if (info.type == IMG_JPEG)
        return JpegDecode(s, outW, outH, row, ctx);
    return IMG_ERR_FORMAT;
}

const char *img_type_name(short type)
{
    static const char *kNames[] = { "unknown", "PNG", "JPEG", "GIF", "BMP", "TIFF",
                                    "EMF", "WMF", "SVG" };
    return (type >= 0 && type <= IMG_SVG) ? kNames[type] : kNames[0];
}
