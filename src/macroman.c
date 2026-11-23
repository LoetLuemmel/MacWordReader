/*
 * macroman.c - Unicode -> Mac OS Roman (System 7 variant: 0xDB = U+00A4).
 * See macroman.h for the rules.
 */
#include "macroman.h"

/* Bytes 0x80..0xFF of Mac OS Roman as Unicode (Apple's ROMAN.TXT, with the
   pre-Mac OS 8.5 currency sign at 0xDB). */
static const wr_u16 kHigh[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, /* 80 */
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, /* 90 */
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, /* A0 */
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, /* B0 */
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, /* C0 */
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, /* D0 */
    0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, /* E0 */
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, /* F0 */
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

/* Latin Extended-A (U+0100..U+017F) without its accents: Polish, Czech,
   Hungarian, Turkish, ... become readable plain letters. Œ œ ı Ÿ are caught
   by the MacRoman table before this one is consulted. */
static const char kLatinExtA[129] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/* Stand-ins for characters MacRoman lacks. Sorted by code point. */
typedef struct { wr_u16 cp; const char *s; } Subst;
static const Subst kSubst[] = {
    { 0x00A6, "|" },   { 0x00AD, "" },    { 0x00B2, "2" },   { 0x00B3, "3" },
    { 0x00B9, "1" },   { 0x00BC, "1/4" }, { 0x00BD, "1/2" }, { 0x00BE, "3/4" },
    { 0x00D0, "D" },   { 0x00D7, "x" },   { 0x00DD, "Y" },   { 0x00DE, "Th" },
    { 0x00F0, "d" },   { 0x00FD, "y" },   { 0x00FE, "th" },
    { 0x0218, "S" },   { 0x0219, "s" },   { 0x021A, "T" },   { 0x021B, "t" },
    { 0x02BC, "\xD5" },{ 0x02DA, "\xFB" },
    { 0x2002, " " },   { 0x2003, " " },   { 0x2004, " " },   { 0x2005, " " },
    { 0x2006, " " },   { 0x2007, " " },   { 0x2008, " " },   { 0x2009, " " },
    { 0x200A, " " },   { 0x200B, "" },    { 0x200C, "" },    { 0x200D, "" },
    { 0x200E, "" },    { 0x200F, "" },
    { 0x2010, "-" },   { 0x2011, "-" },   { 0x2012, "\xD0" },{ 0x2015, "\xD1" },
    { 0x2016, "||" },  { 0x201B, "\xD4" },{ 0x201F, "\xD2" },{ 0x2023, "\xA5" },
    { 0x2024, "." },   { 0x2025, ".." },  { 0x2027, "\xE1" },{ 0x202F, " " },
    { 0x2032, "'" },   { 0x2033, "\"" },  { 0x2043, "-" },   { 0x205F, " " },
    { 0x2060, "" },    { 0x20AC, "EUR" }, { 0x2116, "No." }, { 0x2190, "<-" },
    { 0x2192, "->" },  { 0x2194, "<->" }, { 0x21D0, "<=" },  { 0x21D2, "=>" },
    { 0x2212, "-" },   { 0x2215, "/" },   { 0x2217, "*" },   { 0x2219, "\xE1" },
    { 0x2223, "|" },   { 0x223C, "~" },   { 0x2500, "-" },   { 0x2502, "|" },
    { 0x25A0, "\xA5" },{ 0x25AA, "\xA5" },{ 0x25CB, "o" },   { 0x25CF, "\xA5" },
    { 0x25E6, "o" },   { 0x2610, "[ ]" }, { 0x2611, "[x]" }, { 0x2612, "[x]" },
    { 0x2713, "v" },   { 0x2714, "v" },   { 0x2717, "x" },   { 0x2022, "\xA5" },
    { 0xF0A7, "\xA5" },{ 0xF0B7, "\xA5" },{ 0xF0D8, ">" },   { 0xF0FC, "v" },
    { 0xFEFF, "" },    { 0xFFFC, "" }
};

/* MacRoman's high half sorted by Unicode, built on first use. */
static wr_u16 gRevCp[128];
static wr_u8  gRevByte[128];
static short  gRevReady = 0;

static void BuildReverse(void)
{
    short i, j;
    for (i = 0; i < 128; i++) {
        wr_u16 cp = kHigh[i];
        j = i;
        while (j > 0 && gRevCp[j - 1] > cp) {
            gRevCp[j] = gRevCp[j - 1];
            gRevByte[j] = gRevByte[j - 1];
            j--;
        }
        gRevCp[j] = cp;
        gRevByte[j] = (wr_u8)(0x80 + i);
    }
    gRevReady = 1;
}

static wr_u8 LookupHigh(wr_u32 cp)
{
    short lo = 0, hi = 127, mid;
    if (!gRevReady)
        BuildReverse();
    while (lo <= hi) {
        mid = (short)((lo + hi) / 2);
        if (gRevCp[mid] == cp)
            return gRevByte[mid];
        if (gRevCp[mid] < cp)
            lo = (short)(mid + 1);
        else
            hi = (short)(mid - 1);
    }
    return 0;
}

static const char *LookupSubst(wr_u32 cp)
{
    short i, n = (short)(sizeof(kSubst) / sizeof(kSubst[0]));
    for (i = 0; i < n; i++)        /* short table, unsorted tail: linear is fine */
        if (kSubst[i].cp == cp)
            return kSubst[i].s;
    return 0;
}

short mr_from_unicode(wr_u32 cp, wr_u8 *out, long *lossy)
{
    wr_u8 b;
    const char *s;
    short n;

    if (cp < 0x80) {
        out[0] = (wr_u8)cp;
        return 1;
    }
    if (cp <= 0xFFFF && (b = LookupHigh(cp)) != 0) {
        out[0] = b;
        return 1;
    }
    if ((s = LookupSubst(cp)) != 0) {
        for (n = 0; s[n]; n++)
            out[n] = (wr_u8)s[n];
        return n;
    }
    if (cp >= 0x0100 && cp <= 0x017F) {
        out[0] = (wr_u8)kLatinExtA[cp - 0x0100];
        return 1;
    }
    if (cp >= 0x0300 && cp <= 0x036F)      /* stray combining mark: drop */
        return 0;
    if (cp >= 0xFE00 && cp <= 0xFE0F)      /* variation selectors (emoji) */
        return 0;
    if (lossy)
        (*lossy)++;
    out[0] = '?';
    return 1;
}

wr_u8 mr_compose(wr_u8 base, wr_u32 mark)
{
    /* base letters and the precomposed Latin-1 letter each mark makes */
    static const char *kBase[7] = {
        "AEIOUaeiou",          /* U+0300 grave */
        "AEIOUYaeiouy",        /* U+0301 acute */
        "AEIOUaeiou",          /* U+0302 circumflex */
        "ANOano",              /* U+0303 tilde */
        "AEIOUaeiouy",         /* U+0308 diaeresis */
        "Aa",                  /* U+030A ring */
        "Cc"                   /* U+0327 cedilla */
    };
    static const wr_u16 kComp[7][12] = {
        { 0xC0,0xC8,0xCC,0xD2,0xD9,0xE0,0xE8,0xEC,0xF2,0xF9 },
        { 0xC1,0xC9,0xCD,0xD3,0xDA,0xDD,0xE1,0xE9,0xED,0xF3,0xFA,0xFD },
        { 0xC2,0xCA,0xCE,0xD4,0xDB,0xE2,0xEA,0xEE,0xF4,0xFB },
        { 0xC3,0xD1,0xD5,0xE3,0xF1,0xF5 },
        { 0xC4,0xCB,0xCF,0xD6,0xDC,0xE4,0xEB,0xEF,0xF6,0xFC,0xFF },
        { 0xC5,0xE5 },
        { 0xC7,0xE7 }
    };
    short k, i;
    switch (mark) {
    case 0x0300: k = 0; break;
    case 0x0301: k = 1; break;
    case 0x0302: k = 2; break;
    case 0x0303: k = 3; break;
    case 0x0308: k = 4; break;
    case 0x030A: k = 5; break;
    case 0x0327: k = 6; break;
    default: return 0;
    }
    for (i = 0; kBase[k][i]; i++)
        if ((wr_u8)kBase[k][i] == base)
            return LookupHigh(kComp[k][i]);
    return 0;
}

wr_u32 mr_to_unicode(wr_u8 b)
{
    return b < 0x80 ? (wr_u32)b : (wr_u32)kHigh[b - 0x80];
}
