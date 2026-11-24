/*
 * inflate.c - streaming raw DEFLATE decoder (RFC 1951).
 *
 * Canonical-Huffman decoding after Mark Adler's puff.c, plus a 9-bit lookup
 * table so the common short codes cost one table read instead of a bit-by-bit
 * walk - that is most of the speed on a 33 MHz 68030.
 */
#include "inflate.h"

#define WSIZE    32768L
#define WMASK    (WSIZE - 1)
#define INBUF    2048
#define FASTBITS 9
#define MAXBITS  15

typedef struct {
    short          count[MAXBITS + 1];
    short          symbol[288];
    unsigned short fast[1 << FASTBITS];   /* (len << 12) | sym, 0 = slow path */
} Huff;

typedef struct {
    WrReadFn  rd;   void *rctx;
    WrWriteFn wr;   void *wctx;
    wr_u8     in[INBUF];
    long      inPos, inLen;
    short     eof;          /* source exhausted */
    short     overrun;      /* zero bytes fed past the end */
    wr_u32    bitbuf;
    short     bitcnt;
    wr_u8     win[WSIZE];
    long      wpos;         /* next write position in win */
    long      flushed;      /* start of not-yet-written bytes in win */
    wr_u32    total;        /* bytes produced so far */
    short     err;
    Huff      lit, dist, fixLit, fixDist;
    short     fixedReady;
} State;

static const short kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577 };
static const short kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static wr_u8 NextByte(State *s)
{
    if (s->inPos >= s->inLen) {
        long n;
        if (s->eof) {
            s->overrun++;
            return 0;
        }
        n = s->rd(s->rctx, s->in, INBUF);
        if (n < 0) {
            s->err = INF_ERR_READ;
            s->eof = 1;
            return 0;
        }
        if (n == 0) {
            s->eof = 1;
            s->overrun++;
            return 0;
        }
        s->inLen = n;
        s->inPos = 0;
    }
    return s->in[s->inPos++];
}

static void Need(State *s, short n)
{
    while (s->bitcnt < n) {
        s->bitbuf |= (wr_u32)NextByte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
}

static wr_u32 Bits(State *s, short n)
{
    wr_u32 v;
    if (n == 0)
        return 0;
    Need(s, n);
    v = s->bitbuf & ((1UL << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* ---- output through the window ---- */

static void Flush(State *s)
{
    if (s->wpos > s->flushed && s->err == INF_OK) {
        if (s->wr(s->wctx, s->win + s->flushed, s->wpos - s->flushed) != 0)
            s->err = INF_ERR_STOPPED;
    }
    s->flushed = s->wpos;
}

static void Put(State *s, wr_u8 b)
{
    s->win[s->wpos++] = b;
    s->total++;
    if (s->wpos == WSIZE) {
        Flush(s);
        s->wpos = 0;
        s->flushed = 0;
    }
}

/* ---- Huffman tables ---- */

static short Reverse(short code, short len)
{
    short r = 0;
    while (len-- > 0) {
        r = (short)((r << 1) | (code & 1));
        code >>= 1;
    }
    return r;
}

/* Returns 0 for a complete or incomplete code, -1 when over-subscribed. */
static short Build(Huff *h, const short *length, short n)
{
    short offs[MAXBITS + 1];
    short len, sym, left;
    long code;
    short next[MAXBITS + 1];
    long i;

    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    for (i = 0; i < (1 << FASTBITS); i++)
        h->fast[i] = 0;
    if (h->count[0] == n)
        return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = sym;

    /* canonical codes, then the fast table for the short ones */
    code = 0;
    for (len = 1; len <= MAXBITS; len++) {
        next[len] = (short)code;
        code = (code + h->count[len]) << 1;
    }
    for (sym = 0; sym < n; sym++) {
        len = length[sym];
        if (len != 0 && len <= FASTBITS) {
            short r = Reverse(next[len], len);
            for (i = r; i < (1 << FASTBITS); i += (1L << len))
                h->fast[i] = (unsigned short)((len << 12) | sym);
        }
        if (len != 0)
            next[len]++;
    }
    return 0;
}

static short Decode(State *s, Huff *h)
{
    short len, count, code, first, index;
    unsigned short e;

    Need(s, FASTBITS);
    e = h->fast[s->bitbuf & ((1 << FASTBITS) - 1)];
    if (e != 0) {
        len = (short)(e >> 12);
        s->bitbuf >>= len;
        s->bitcnt -= len;
        return (short)(e & 0x0FFF);
    }
    code = first = index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        code |= (short)Bits(s, 1);
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* ---- blocks ---- */

static short StoredBlock(State *s)
{
    wr_u32 len, nlen;
    s->bitbuf >>= (s->bitcnt & 7);          /* to a byte boundary */
    s->bitcnt -= (short)(s->bitcnt & 7);
    len = Bits(s, 16);
    nlen = Bits(s, 16);
    if (len != (~nlen & 0xFFFFUL))
        return INF_ERR_DATA;
    while (len-- > 0 && s->err == INF_OK)
        Put(s, (wr_u8)Bits(s, 8));
    return s->err;
}

/* The hot loop. Bit buffer, bit count and window position live in locals
   (registers on the 68k); the State is updated only around the slow paths.
   A literal with a short code costs one table read and one store. */
#define SYNC() (s->bitbuf = bb, s->bitcnt = bc, s->total += (wr_u32)(wp - wp0), s->wpos = wp, wp0 = wp)
#define LOAD() (bb = s->bitbuf, bc = s->bitcnt, wp = wp0 = s->wpos)

static short Codes(State *s, Huff *lit, Huff *dist)
{
    wr_u32 bb;
    short bc, sym;
    long wp, wp0, len, d, from;
    unsigned short e;
    wr_u8 *win = s->win;
    const unsigned short *lfast = lit->fast;

    LOAD();
    for (;;) {
        if (s->err != INF_OK || s->overrun > 4) {
            SYNC();
            return s->err != INF_OK ? s->err : INF_ERR_DATA;
        }
        /* at least 9 bits: from the input buffer directly while it lasts */
        while (bc < FASTBITS) {
            if (s->inPos < s->inLen)
                bb |= (wr_u32)s->in[s->inPos++] << bc;
            else {
                SYNC();
                bb |= (wr_u32)NextByte(s) << bc;
            }
            bc += 8;
        }
        e = lfast[bb & ((1 << FASTBITS) - 1)];
        if (e != 0 && (e & 0x0FFF) < 256) {     /* a literal */
            bb >>= (e >> 12);
            bc -= (short)(e >> 12);
            win[wp++] = (wr_u8)e;
            if (wp == WSIZE) {
                SYNC();
                Flush(s);
                s->wpos = s->flushed = 0;
                LOAD();
            }
            continue;
        }
        SYNC();
        sym = Decode(s, lit);
        if (sym < 0)
            return INF_ERR_DATA;
        if (sym < 256) {
            Put(s, (wr_u8)sym);
        } else if (sym == 256) {
            return INF_OK;
        } else {
            sym -= 257;
            if (sym >= 29)
                return INF_ERR_DATA;
            len = kLenBase[sym] + (long)Bits(s, kLenExtra[sym]);
            sym = Decode(s, dist);
            if (sym < 0 || sym >= 30)
                return INF_ERR_DATA;
            d = kDistBase[sym] + (long)Bits(s, kDistExtra[sym]);
            if ((wr_u32)d > s->total)
                return INF_ERR_DATA;
            /* the copy: straight through the window unless it wraps */
            from = s->wpos - d;
            if (from >= 0 && s->wpos + len < WSIZE) {
                wr_u8 *o = win + s->wpos, *f = win + from;
                s->wpos += len;
                s->total += (wr_u32)len;
                while (len-- > 0)
                    *o++ = *f++;
            } else
                while (len-- > 0)
                    Put(s, win[(s->wpos - d) & WMASK]);
        }
        LOAD();
    }
}

static short FixedBlock(State *s)
{
    if (!s->fixedReady) {
        short l[288], i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        Build(&s->fixLit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        Build(&s->fixDist, l, 30);
        s->fixedReady = 1;
    }
    return Codes(s, &s->fixLit, &s->fixDist);
}

static short DynamicBlock(State *s)
{
    static const short kOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    short nlen, ndist, ncode, index, sym, len, i;

    nlen = (short)(Bits(s, 5) + 257);
    ndist = (short)(Bits(s, 5) + 1);
    ncode = (short)(Bits(s, 4) + 4);
    if (nlen > 286 || ndist > 30)
        return INF_ERR_DATA;
    for (i = 0; i < ncode; i++)
        lengths[kOrder[i]] = (short)Bits(s, 3);
    for (; i < 19; i++)
        lengths[kOrder[i]] = 0;
    if (Build(&s->lit, lengths, 19) != 0)
        return INF_ERR_DATA;

    index = 0;
    while (index < nlen + ndist) {
        sym = Decode(s, &s->lit);
        if (sym < 0 || s->overrun > 4)
            return INF_ERR_DATA;
        if (sym < 16) {
            lengths[index++] = sym;
        } else {
            len = 0;
            if (sym == 16) {
                if (index == 0)
                    return INF_ERR_DATA;
                len = lengths[index - 1];
                sym = (short)(3 + Bits(s, 2));
            } else if (sym == 17) {
                sym = (short)(3 + Bits(s, 3));
            } else {
                sym = (short)(11 + Bits(s, 7));
            }
            if (index + sym > nlen + ndist)
                return INF_ERR_DATA;
            while (sym-- > 0)
                lengths[index++] = len;
        }
    }
    if (lengths[256] == 0)
        return INF_ERR_DATA;
    if (Build(&s->lit, lengths, nlen) != 0)
        return INF_ERR_DATA;
    if (Build(&s->dist, lengths + nlen, ndist) != 0)
        return INF_ERR_DATA;
    return Codes(s, &s->lit, &s->dist);
}

short inflate_stream(WrReadFn rd, void *rctx, WrWriteFn wr, void *wctx)
{
    State *s;
    short last, type, err;

    s = (State *)WR_ALLOC(sizeof(State));
    if (s == 0)
        return INF_ERR_MEM;
    s->rd = rd; s->rctx = rctx;
    s->wr = wr; s->wctx = wctx;

    do {
        last = (short)Bits(s, 1);
        type = (short)Bits(s, 2);
        if (type == 0)
            err = StoredBlock(s);
        else if (type == 1)
            err = FixedBlock(s);
        else if (type == 2)
            err = DynamicBlock(s);
        else
            err = INF_ERR_DATA;
        if (err == INF_OK && s->err != INF_OK)
            err = s->err;
        if (err == INF_OK && s->overrun > 4)
            err = INF_ERR_DATA;
    } while (!last && err == INF_OK);

    if (err == INF_OK) {
        Flush(s);
        err = s->err;
    }
    WR_FREE(s);
    return err;
}
