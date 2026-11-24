/*
 * WordReader - read Microsoft Word documents (.docx and friends) on a 68k Mac.
 *
 * System 7, any 68k Mac with 1 MB to spare (built for a PowerBook Duo 230).
 * Open a document with File > Open, by dropping it on the icon, or by
 * double-clicking a document whose creator is 'WRdr'. The text is converted
 * from Unicode to Mac OS Roman (see macroman.c) and shown word-wrapped, with
 * headings, list bullets and tables (cells separated by tabs).
 *
 * The window draws its own text rather than using TextEdit: TextEdit stops
 * at 32 KB, a long Word document does not.
 *
 * Pictures (PNG, JPEG) are decoded when they first scroll into view, at the
 * size they are shown, into 16 grey levels (1 bit on a Mac without Color
 * QuickDraw), and kept in purgeable handles: when memory runs short the
 * Memory Manager takes them back, and they are decoded again when needed.
 */
#include <Types.h>
#include <MacMemory.h>
#include <QuickDraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Controls.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <Files.h>
#include <StandardFile.h>
#include <AppleEvents.h>
#include <Gestalt.h>
#include <Processes.h>
#include <Resources.h>
#include <Script.h>
#include <SegLoad.h>
#include <TextUtils.h>
#include <Errors.h>
#include <Scrap.h>
#include <Sound.h>
#include <LowMem.h>
#include <Folders.h>

#include "docx.h"
#include "zipdir.h"
#include "image.h"

QDGlobals qd;

#define kCreator      'WRdr'
#define kSBarWidth    15
#define kMargin       8
#define kTabWidth     36
#define kMaxWindows   8

/* control part codes and the scroll bar CDEF (Inside Macintosh: Toolbox
   Essentials 5-32); spelt out because Universal Interfaces 3.4 moved them
   to ControlDefinitions.h and older headers use other names */
#define kPartUp       20
#define kPartDown     21
#define kPartPageUp   22
#define kPartPageDown 23
#define kPartThumb    129
#define kScrollBarDef 16

#define rMenuBar      128
#define mApple        128
#define mFile         129
#define mEdit         130
#define mView         131
#define iAbout        1
#define iOpen         1
#define iClose        2
#define iSaveText     4
#define iInfo         6
#define iQuit         8
#define iUndo         1
#define iCut          3
#define iCopy         4
#define iPaste        5
#define iClear        6
#define iSelectAll    8
#define rAlertError   128
#define rAlertAbout   129
#define rAlertInfo    130

/* run face and size: QuickDraw face bits, size step from body text;
   sup: inside a note reference (DOCX_SUP_ON .. DOCX_SUP_OFF) */
typedef struct {
    unsigned char face;
    signed char   step;
    unsigned char sup;
} RunState;

typedef struct {
    long          start;     /* offset of the first byte in the text */
    short         len;       /* bytes on this line, without the CR */
    unsigned char face;      /* run state where the line starts */
    signed char   step;
    unsigned char sup;
    short         x0;        /* indent of the first character */
    short         left;      /* the paragraph's left indent (hanging tab stop) */
    short         shift;     /* centring / right alignment */
    short         asc;       /* line top to baseline, space before included */
    short         height;    /* whole line, space before and after included */
    short         pw, ph;    /* a picture line: the picture's size (pw > 0) */
} LineRec;

/* A decoded picture, kept for redrawing. */
#define kMaxPics  16
enum { kPicNone = 0, kPicReady, kPicFailed };
typedef struct {
    long   off;              /* its DOCX_PIC item in the text */
    short  w, h;             /* shown size, pixels */
    short  rowBytes;
    short  state;
    Handle bits;             /* purgeable: 4-bit grey, or 1-bit */
    long   used;             /* TickCount when last drawn */
    char   why[48];          /* not drawn: what the box says */
} PicEntry;

typedef struct {
    Handle        text;      /* MacRoman, CR-separated, with style markers */
    long          len, cap;
    Handle        lines;     /* LineRec[] */
    long          nLines, linesCap;
    long          top;       /* first visible line */
    long          maxTop;
    short         scale;     /* lines per scroll-bar unit (> 1 for huge texts) */
    ControlHandle vbar;
    FSSpec        spec;
    DocxStats     st;
    short         truncated;
    long          ticks;     /* conversion time */
    long          selStart;  /* selection: text offsets, selStart <= selEnd */
    long          selEnd;
    Boolean       active;    /* front and not suspended: show the selection */
    PicEntry      pics[kMaxPics];
    long          picCount;  /* pictures decoded so far, their total and slowest time */
    long          picTicks, picSlowest;
} DocRec, *DocPtr;

/* metrics of one face and size of Geneva */
typedef struct {
    short size, face;
    short ascent, height;
    short widths[256];
    unsigned char missing[256];   /* the font has no glyph: draw a stand-in */
} Metrics;

/* Stand-ins for MacRoman characters a bitmap font may lack. Geneva and
   friends predate some of MacRoman's later additions (‚ „ ‰ ﬁ ﬂ ¤ ...):
   the byte is right, the font just draws its empty box. */
static const char *GlyphStandIn(unsigned char c)
{
    switch (c) {
    case 0xE2: return ",";       /* ‚ single low quote */
    case 0xE3: return ",,";      /* „ double low quote */
    case 0xE4: return "%o";      /* ‰ */
    case 0xDE: return "fi";
    case 0xDF: return "fl";
    case 0xDA: return "/";       /* ⁄ fraction slash */
    case 0xDB: return "*";       /* ¤ currency */
    case 0xE1: return ".";       /* · middle dot: the width; DrawLine draws a real dot */
    case 0xDC: return "<";       /* ‹ */
    case 0xDD: return ">";       /* › */
    case 0xD7: return "<>";      /* ◊ */
    case 0xF0: return "@";       /* Apple logo */
    case 0xD0: return "-";       /* – */
    case 0xD1: return "--";      /* — */
    case 0xA5: return "*";       /* • */
    case 0xC9: return "...";     /* … */
    case 0xD2: case 0xD3: return "\"";
    case 0xD4: case 0xD5: return "'";
    default:   return "?";
    }
}

/* Draw € at the pen and advance by the digit width. No System 7 bitmap
   font has the glyph, so it is built from QuickDraw primitives: an open C
   (an arc missing its right quarter) crossed by two bars, sized from the
   style's ascent; headings are bold, so their strokes are two pixels wide. */
static void DrawEuro(short width, short ascent, Boolean bold)
{
    Point pen;
    Rect c;
    short capH, x, base, y1, y2, barEnd, stroke;

    GetPen(&pen);
    x = pen.h;
    base = pen.v;
    capH = (short)((ascent * 3 + 2) / 4);          /* about the height of a capital */
    if (capH < 6)
        capH = 6;
    stroke = (short)(bold ? 2 : 1);
    PenSize(stroke, 1);
    SetRect(&c, (short)(x + width / 6 + 1), (short)(base - capH), (short)(x + width), base);
    FrameArc(&c, 135, 270);                         /* 0 = top, clockwise: open to the right */
    if (bold) {                                     /* thicken vertically too */
        OffsetRect(&c, 0, -1);
        FrameArc(&c, 150, 240);
    }
    PenSize(1, 1);
    y1 = (short)(base - (capH * 5 + 4) / 8);
    y2 = (short)(base - (capH * 3 + 4) / 8);
    if (y2 - y1 < 2)
        y1 = (short)(y2 - 2);
    barEnd = (short)(x + (width * 2) / 3);
    MoveTo(x, y1); LineTo(barEnd, y1);
    MoveTo(x, y2); LineTo(barEnd, y2);
    if (bold) {
        MoveTo(x, (short)(y1 + 1)); LineTo(barEnd, (short)(y1 + 1));
        MoveTo(x, (short)(y2 + 1)); LineTo(barEnd, (short)(y2 + 1));
    }
    MoveTo((short)(x + width), base);
}

static short gBaseSize = 12;
static short gQuit = 0;
static long  gLastYield = 0;
static Boolean gHasAE = false;
static Boolean gHasCQD = false;       /* Color QuickDraw: pictures in 16 greys */
static CTabHandle gGreyCT = 0;
static unsigned char gDither[16][256];  /* Bayer level, grey -> colour index */
static ControlActionUPP gScrollUPP;

/* ------------------------------------------------------------------ util */

static DocPtr DocOf(WindowPtr w)
{
    if (w == 0 || ((WindowPeek)w)->windowKind != userKind)
        return 0;
    return (DocPtr)GetWRefCon(w);
}

static void NumToPStr(long n, Str255 s)
{
    char tmp[16];
    short i = 0, j;
    Boolean neg = n < 0;
    if (neg)
        n = -n;
    do {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0 && i < 15);
    j = 0;
    if (neg)
        s[++j] = '-';
    while (i > 0)
        s[++j] = (unsigned char)tmp[--i];
    s[0] = (unsigned char)j;
}

/* Thousands separators make the info box readable: 123,456 */
static void NumToPStrSep(long n, Str255 s)
{
    Str255 raw;
    short i, j = 0, digits;
    NumToPStr(n, raw);
    digits = raw[0];
    for (i = 1; i <= raw[0]; i++) {
        s[++j] = raw[i];
        digits--;
        if (digits > 0 && digits % 3 == 0 && raw[i] != '-')
            s[++j] = ',';
    }
    s[0] = (unsigned char)j;
}

static void PCat(Str255 d, ConstStr255Param s)
{
    short i;
    for (i = 1; i <= s[0] && d[0] < 255; i++)
        d[++d[0]] = s[i];
}

static void CCat(Str255 d, const char *s)
{
    while (*s && d[0] < 255)
        d[++d[0]] = (unsigned char)*s++;
}

static void ErrorAlert(ConstStr255Param what, ConstStr255Param why)
{
    InitCursor();
    ParamText(what, why, "\p", "\p");
    StopAlert(rAlertError, 0);
}

static void ErrorText(short err, Str255 msg)
{
    msg[0] = 0;
    switch (err) {
    case DOCX_ERR_OLE:
        CCat(msg, "This is a binary Office file: an old Word 97-2003 .doc, or a "
                  "password-protected .docx. Save it as .docx without a password.");
        break;
    case DOCX_ERR_RTF:
        CCat(msg, "This is an RTF file, not a .docx. Open it with a word processor.");
        break;
    case DOCX_ERR_NOTWORD:
        CCat(msg, "This is an office document, but not a text one (a spreadsheet or a presentation?).");
        break;
    case DOCX_ERR_UTF16:
        CCat(msg, "This XML file is UTF-16 encoded; only UTF-8 Word XML is supported.");
        break;
    case DOCX_ERR_FORMAT:
    case ZIP_ERR_NOTZIP:
        CCat(msg, "This is not a document WordReader knows (.docx, .docm, .dotx, Word XML, .odt, .fodt).");
        break;
    case ZIP_ERR_NOENTRY:
        CCat(msg, "The package has no main document part. Is it damaged?");
        break;
    case ZIP_ERR_CRYPT:
        CCat(msg, "The document is encrypted.");
        break;
    case ZIP_ERR_METHOD:
        CCat(msg, "The document uses an unknown compression method.");
        break;
    case ZIP_ERR_DATA:
        CCat(msg, "The document's compressed data is damaged.");
        break;
    case ZIP_ERR_MEM:
        CCat(msg, "Not enough memory. Give WordReader more memory in the Finder's Get Info.");
        break;
    default:
        CCat(msg, "The file could not be read (error ");
        {
            Str255 n;
            NumToPStr(err, n);
            PCat(msg, n);
        }
        CCat(msg, ").");
    }
}

/* Gives other applications - and the network - time while we work. */
static void Breathe(void)
{
    EventRecord ev;
    if (TickCount() - gLastYield > 15) {
        gLastYield = TickCount();
        WaitNextEvent(0, &ev, 0, 0);
        SetCursor(*GetCursor(watchCursor));
    }
}

/* --------------------------------------------------------------- metrics */

/* Metrics for one face and size, made when first needed. Sizes snap to
   Geneva's bitmap sizes so every run stays crisp on the Duo's LCD. */
#define kLadderN 7
static const short kLadder[kLadderN] = { 9, 10, 12, 14, 18, 20, 24 };
static Metrics *gM[8][kLadderN];
static short gBodyIdx = 2;            /* kLadder[gBodyIdx] == gBaseSize */

static short SizeIdx(short step)
{
    short i = (short)(gBodyIdx + step);
    return (short)(i < 0 ? 0 : i >= kLadderN ? kLadderN - 1 : i);
}

static void UseFont(const Metrics *m)
{
    TextFont(applFont);               /* Geneva */
    TextFace((Style)m->face);
    TextSize(m->size);
}

/* Which MacRoman characters does the font in use actually have? FMSwapFont
   hands back the strike QuickDraw will draw with; in a bitmap font record
   ('FONT'/'NFNT') the offset/width table holds -1 for every absent glyph
   (Inside Macintosh: Text 4-71). A TrueType font answers with an 'sfnt'
   instead - left alone, it has the whole MacRoman set. */
static void FindMissingGlyphs(Metrics *m)
{
    FMInput in;
    FMOutPtr out;
    FontRec *fr;
    short *owt, c;
    long off;

    in.family = applFont;
    in.size = m->size;
    in.face = (Style)m->face;
    in.needBits = true;
    in.device = 0;
    in.numer.h = in.numer.v = 1;
    in.denom.h = in.denom.v = 1;
    out = FMSwapFont(&in);
    if (out == 0 || out->fontHandle == 0 || *out->fontHandle == 0)
        return;
    fr = (FontRec *)*out->fontHandle;
    if ((fr->fontType & 0x9000) != 0x9000)      /* not a bitmap strike */
        return;
    off = (unsigned short)fr->owTLoc;
    if (fr->nDescent > 0)                       /* high word of a big offset */
        off |= (long)fr->nDescent << 16;
    owt = (short *)((char *)&fr->owTLoc + off * 2);
    for (c = 0x80; c < 256; c++) {
        if (c < fr->firstChar || c > fr->lastChar || owt[c - fr->firstChar] == -1) {
            const char *sub = GlyphStandIn((unsigned char)c);
            short n = 0;
            while (sub[n])
                n++;
            m->missing[c] = 1;
            m->widths[c] = TextWidth((Ptr)sub, 0, n);
        }
    }
}

static Metrics *GetM(short face, short idx)
{
    Metrics *m;
    FontInfo fi;
    short c, f, i;

    face &= 7;
    m = gM[face][idx];
    if (m)
        return m;
    m = (Metrics *)NewPtrClear(sizeof(Metrics));
    if (m == 0) {                               /* out of memory: anything we have */
        for (f = 0; f < 8; f++)
            for (i = 0; i < kLadderN; i++)
                if (gM[f][i])
                    return gM[f][i];
        return 0;                               /* not reached: body made at start */
    }
    m->size = kLadder[idx];
    m->face = face;
    UseFont(m);
    GetFontInfo(&fi);
    m->ascent = fi.ascent;
    m->height = (short)(fi.ascent + fi.descent + fi.leading);
    for (c = 0; c < 256; c++)
        m->widths[c] = (c < 0x20) ? 0 : CharWidth((char)c);
    FindMissingGlyphs(m);
    m->missing[DOCX_CH_EURO] = 1;               /* drawn by DrawEuro */
    m->widths[DOCX_CH_EURO] = m->widths['0'];
    gM[face][idx] = m;
    return m;
}

static void SetBodySize(short size)
{
    short i;
    gBaseSize = size;
    for (i = 0; i < kLadderN; i++)
        if (kLadder[i] == size)
            gBodyIdx = i;
    GetM(0, gBodyIdx);
}

/* ---------------------------------------------------------------- layout */

static void ContentRect(WindowPtr w, Rect *r)
{
    *r = w->portRect;
    r->right -= kSBarWidth;
}

/* Points in the document -> pixels, scaled with the chosen text size. */
static short Px(short pt)
{
    return (short)((long)pt * gBaseSize / 12);
}

typedef struct {
    short before, after, left, first, align;
    unsigned char marker;
} ParaInfo;

/* Read a paragraph's prefix (style marker, DOCX_PARA record); returns the
   offset of its first text byte. */
static long ParsePara(const unsigned char *t, long p, long end, ParaInfo *pi)
{
    Boolean rec = false;
    pi->before = pi->after = pi->left = pi->first = pi->align = 0;
    pi->marker = 0;
    while (p < end) {
        unsigned char c = t[p];
        if (DOCX_IS_MARKER(c)) {
            pi->marker = c;
            p++;
        } else if (c == DOCX_PARA && p + DOCX_PARA_LEN <= end) {
            pi->before = Px((short)(t[p + 1] - 0x20));
            pi->after = Px((short)(t[p + 2] - 0x20));
            pi->left = Px((short)((t[p + 3] - 0x20) * 2));
            pi->first = Px((short)((t[p + 4] - 0x20 - 100) * 2));
            pi->align = (short)(t[p + 5] - 0x20);
            p += DOCX_PARA_LEN;
            rec = true;
        } else
            break;
    }
    if (!rec)
        pi->after = (short)(gBaseSize / 3);     /* plain text: a little air */
    if (pi->marker && pi->before < gBaseSize / 2)
        pi->before = (short)(gBaseSize / 2);    /* headings: air above */
    return p;
}

/* A heading's look comes from its style marker. */
static void MarkerState(unsigned char mk, RunState *s)
{
    s->sup = 0;
    s->face = (unsigned char)(mk ? bold : 0);
    s->step = (signed char)(mk == DOCX_MK_TITLE ? 3 : mk == DOCX_MK_H1 ? 2 : mk == DOCX_MK_H2 ? 1 : 0);
}

/* The next tab stop: the hanging indent first, then every half inch. */
static short NextTab(short x, short left)
{
    if (x < left)
        return left;
    return (short)((x / kTabWidth + 1) * kTabWidth);
}

/* Walks a line byte by byte, following run face and size changes. */
typedef struct {
    RunState s;
    Metrics *m;
    short    x;          /* logical x: 0 = left margin, before alignment */
    short    rise;       /* a note reference: its baseline is this much higher */
} Walker;

/* The metrics for a run state: a note reference two sizes smaller, raised
   by a third of the run's own ascent. */
static void WalkFont(Walker *w)
{
    if (w->s.sup) {
        w->rise = (short)(GetM(w->s.face, SizeIdx(w->s.step))->ascent / 3);
        w->m = GetM(w->s.face, SizeIdx((short)(w->s.step - 2)));
    } else {
        w->rise = 0;
        w->m = GetM(w->s.face, SizeIdx(w->s.step));
    }
}

static void WalkStart(Walker *w, const LineRec *lr)
{
    w->s.face = lr->face;
    w->s.step = lr->step;
    w->s.sup = lr->sup;
    WalkFont(w);
    w->x = lr->x0;
}

/* Advance over byte c; returns its width (0 for control bytes). */
static short WalkStep(Walker *w, unsigned char c, short left)
{
    short cw;
    if (DOCX_IS_FACE(c)) {
        w->s.face = (unsigned char)(c - DOCX_FACE0);
        WalkFont(w);
        return 0;
    }
    if (DOCX_IS_SIZE(c)) {
        w->s.step = (signed char)(c - DOCX_SIZE0 - 2);
        WalkFont(w);
        return 0;
    }
    if (DOCX_IS_SUP(c)) {
        w->s.sup = (unsigned char)(c == DOCX_SUP_ON);
        WalkFont(w);
        return 0;
    }
    cw = (c == '\t') ? (short)(NextTab(w->x, left) - w->x) : w->m->widths[c];
    w->x += cw;
    return cw;
}

/* A picture's size on screen: its size in points (1 point = 1 pixel),
   shrunk to fit the column and the window. */
static void PicSize(const unsigned char *t, long i, long end, short maxW, short maxH,
                    short *pw, short *ph)
{
    char name[DOCX_PIC_NAMEMAX + 1];
    short w, h;
    if (!docx_pic_info(t, i, end, &w, &h, name) || w <= 0 || h <= 0) {
        w = 160;                                /* size unknown */
        h = 120;
    }
    if (maxW < 8)
        maxW = 8;
    if (w > maxW) {
        h = (short)((long)h * maxW / w);
        w = maxW;
    }
    if (h > maxH) {
        w = (short)((long)w * maxH / h);
        h = maxH;
    }
    *pw = (short)(w < 8 ? 8 : w);
    *ph = (short)(h < 8 ? 8 : h);
}

static Boolean AddLine(DocPtr d, const LineRec *src)
{
    if (d->nLines >= d->linesCap) {
        long ncap = d->linesCap ? d->linesCap * 2 : 1024;
        SetHandleSize(d->lines, ncap * (long)sizeof(LineRec));
        if (MemError() != noErr)
            return false;
        d->linesCap = ncap;
    }
    ((LineRec *)*d->lines)[d->nLines++] = *src;
    return true;
}

static void Layout(WindowPtr w)
{
    DocPtr d = DocOf(w);
    Rect r;
    short maxW, maxH, left, x0;
    long i, p, end, lineStart, lineEnd, next, lastBreak, k;
    unsigned char *t;
    long h, avail;
    ParaInfo pi;
    RunState st;
    LineRec lr;
    Walker wk;
    Boolean first, placed;

    if (d == 0)
        return;
    ContentRect(w, &r);
    maxW = (short)(r.right - r.left - 2 * kMargin);
    if (maxW < 40)
        maxW = 40;
    maxH = (short)(r.bottom - r.top - kMargin - 8);   /* a picture fits the window */
    if (maxH < 40)
        maxH = 40;
    d->nLines = 0;

    HLock(d->text);
    t = (unsigned char *)*d->text;
    p = 0;
    while (p < d->len) {
        end = p;
        while (end < d->len && t[end] != '\r')
            end++;
        p = ParsePara(t, p, end, &pi);
        MarkerState(pi.marker, &st);
        left = pi.left;
        if (left > maxW / 2) left = (short)(maxW / 2);
        x0 = (short)(pi.left + pi.first);
        if (x0 < 0) x0 = 0;
        if (x0 > maxW / 2) x0 = (short)(maxW / 2);
        lineStart = p;
        first = true;
        do {
            /* where does this line end? */
            wk.s = st;
            WalkFont(&wk);
            wk.x = x0;
            lastBreak = -1;
            placed = false;
            for (i = lineStart; i < end && i - lineStart < 2000; i++) {
                unsigned char c = t[i];
                short cw;
                if (c == DOCX_CH_LINEBREAK || c == DOCX_PIC)
                    break;
                cw = WalkStep(&wk, c, left);
                if (cw > 0 && wk.x > maxW && placed)
                    break;
                if (cw > 0)
                    placed = true;
                if (c == ' ' || c == '-' || c == '\t')
                    lastBreak = i + 1;
            }
            if (i < end && t[i] == DOCX_PIC && !placed) {
                /* a picture: a line of its own, aligned like its paragraph */
                lr.start = i;
                lr.len = (short)docx_ctrl_len(t, i, end);
                next = i + lr.len;
                st = wk.s;                      /* run changes just before it */
                lr.face = st.face;
                lr.step = st.step;
                lr.sup = st.sup;
                lr.x0 = x0;
                lr.left = left;
                PicSize(t, i, end, (short)(maxW - x0), maxH, &lr.pw, &lr.ph);
                lr.shift = 0;
                if (pi.align == DOCX_AL_CENTER)
                    lr.shift = (short)((maxW - x0 - lr.pw) / 2);
                else if (pi.align == DOCX_AL_RIGHT)
                    lr.shift = (short)(maxW - x0 - lr.pw);
                if (lr.shift < 0)
                    lr.shift = 0;
                lr.asc = (short)((first ? pi.before : 0) + 4);
                lr.height = (short)(lr.asc + lr.ph + 4 + (next >= end ? pi.after : 0));
                if (!AddLine(d, &lr))
                    goto done;
                lineStart = next;
                x0 = left;
                first = false;
                continue;
            }
            if (i < end && t[i] == DOCX_CH_LINEBREAK) {
                lineEnd = i;
                next = i + 1;
            } else if (i < end && t[i] == DOCX_PIC) {
                lineEnd = next = i;             /* text before a picture ends here */
            } else if (i >= end) {
                lineEnd = next = end;
            } else {
                lineEnd = next = (lastBreak > lineStart) ? lastBreak : i;
            }

            /* measure it: tallest run, width without trailing blanks */
            lr.start = lineStart;
            lr.len = (short)(lineEnd - lineStart);
            lr.face = st.face;
            lr.step = st.step;
            lr.sup = st.sup;
            lr.x0 = x0;
            lr.left = left;
            lr.shift = 0;
            lr.pw = lr.ph = 0;
            WalkStart(&wk, &lr);
            {
                short asc = (short)(wk.m->ascent + wk.rise);
                short desc = (short)(wk.m->height - wk.m->ascent - wk.rise);
                short xEnd = x0, content;
                for (k = lineStart; k < lineEnd; k++) {
                    unsigned char c = t[k];
                    if (WalkStep(&wk, c, left) > 0 || c == ' ') {
                        if (wk.m->ascent + wk.rise > asc) asc = (short)(wk.m->ascent + wk.rise);
                        if (wk.m->height - wk.m->ascent - wk.rise > desc)
                            desc = (short)(wk.m->height - wk.m->ascent - wk.rise);
                        if (c != ' ' && c != '\t')
                            xEnd = wk.x;
                    }
                }
                content = (short)(xEnd - x0);
                if (pi.align == DOCX_AL_CENTER)
                    lr.shift = (short)((maxW - x0 - content) / 2);
                else if (pi.align == DOCX_AL_RIGHT)
                    lr.shift = (short)(maxW - x0 - content);
                if (lr.shift < 0)
                    lr.shift = 0;
                lr.asc = (short)((first ? pi.before : 0) + asc);
                lr.height = (short)(lr.asc + desc + (next >= end ? pi.after : 0));
            }
            if (!AddLine(d, &lr))
                goto done;
            st = wk.s;
            lineStart = next;
            x0 = left;
            first = false;
        } while (lineStart < end);
        p = end + 1;
    }
done:
    HUnlock(d->text);

    /* the last top line that still fills the window */
    avail = r.bottom - r.top - kMargin;
    h = 0;
    for (i = d->nLines - 1; i >= 0; i--) {
        h += ((LineRec *)*d->lines)[i].height;
        if (h > avail)
            break;
    }
    d->maxTop = i + 1;
    if (d->maxTop < 0)
        d->maxTop = 0;
    if (d->top > d->maxTop)
        d->top = d->maxTop;
    d->scale = (short)(d->maxTop / 32000 + 1);
    SetControlMaximum(d->vbar, (short)(d->maxTop / d->scale));
    SetControlValue(d->vbar, (short)(d->top / d->scale));
}

/* --------------------------------------------------------------- drawing */

/* logical x of text offset pos on line lr */
static short LineX(const unsigned char *t, const LineRec *lr, long pos)
{
    Walker wk;
    long k;
    if (lr->pw > 0)
        return (short)(lr->x0 + (pos > lr->start ? lr->pw : 0));
    WalkStart(&wk, lr);
    for (k = lr->start; k < pos && k < lr->start + lr->len; k++)
        WalkStep(&wk, t[k], lr->left);
    return wk.x;
}

/* text offset nearest to logical x hx on line lr */
static long LinePos(const unsigned char *t, const LineRec *lr, short hx)
{
    Walker wk;
    long k;
    if (lr->pw > 0)
        return hx < lr->x0 + lr->pw / 2 ? lr->start : lr->start + lr->len;
    WalkStart(&wk, lr);
    for (k = lr->start; k < lr->start + lr->len; k++) {
        short x = wk.x, cw = WalkStep(&wk, t[k], lr->left);
        if (cw > 0 && hx < x + cw / 2)
            return k;
    }
    return lr->start + lr->len;
}

/* ------------------------------------------------------------- pictures */

typedef struct {
    short refNum;
} FileCtx;

static long FileReadAt(void *ctx, wr_u32 off, wr_u8 *buf, long n)
{
    FileCtx *f = (FileCtx *)ctx;
    long count = n;
    OSErr err;
    if (SetFPos(f->refNum, fsFromStart, (long)off) != noErr)
        return -1;
    err = FSRead(f->refNum, &count, buf);
    if (err != noErr && err != eofErr)
        return -1;
    return count;
}

/* a stored (uncompressed) member, read in place */
typedef struct {
    WrSource *base;
    wr_u32    off;
} SubCtx;

static long SubReadAt(void *ctx, wr_u32 off, wr_u8 *buf, long n)
{
    SubCtx *s = (SubCtx *)ctx;
    return s->base->readAt(s->base->ctx, s->off + off, buf, n);
}

/* a compressed member goes to a file in Temporary Items first: the picture
   decoders read their input at will, the inflater only streams forward */
static long TempWrite(void *ctx, const wr_u8 *b, long n)
{
    long count = n;
    return FSWrite(*(short *)ctx, &count, b) == noErr ? 0 : 1;
}

static OSErr OpenTemp(FSSpec *tmp, short *ref)
{
    short vRef;
    long dir;
    OSErr err = FindFolder(kOnSystemDisk, kTemporaryFolderType, kCreateFolder, &vRef, &dir);
    if (err != noErr)
        return err;
    err = FSMakeFSSpec(vRef, dir, "\pWordReader picture", tmp);
    if (err != noErr && err != fnfErr)
        return err;
    FSpDelete(tmp);
    err = FSpCreate(tmp, kCreator, 'TEMP', smSystemScript);
    if (err != noErr)
        return err;
    return FSpOpenDF(tmp, fsRdWrPerm, ref);
}

/* rows from the decoder, ordered-dithered into 16 greys (index 0 = white)
   or into black and white */
typedef struct {
    unsigned char *base;
    short rowBytes;
    long  rows;
} PicSink;

static const unsigned char kBayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

static long PicRow(void *ctx, short y, const wr_u8 *g, short w)
{
    PicSink *s = (PicSink *)ctx;
    unsigned char *row = s->base + (long)y * s->rowBytes;
    const unsigned char *bay = kBayer[y & 3];
    short x;
    if (gHasCQD) {
        const unsigned char *d0 = gDither[bay[0]], *d1 = gDither[bay[1]];
        const unsigned char *d2 = gDither[bay[2]], *d3 = gDither[bay[3]];
        for (x = 0; x + 3 < w; x += 4) {        /* two bytes, four pixels */
            row[x >> 1] = (unsigned char)((d0[g[x]] << 4) | d1[g[x + 1]]);
            row[(x >> 1) + 1] = (unsigned char)((d2[g[x + 2]] << 4) | d3[g[x + 3]]);
        }
        for (; x < w; x++) {
            short idx = gDither[bay[x & 3]][g[x]];
            row[x >> 1] |= (unsigned char)((x & 1) ? idx : idx << 4);
        }
    } else {
        for (x = 0; x < w; x++)
            if (g[x] < bay[x & 3] * 16 + 8)
                row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
    }
    s->rows++;
    Breathe();
    return 0;
}

static void SetWhy(PicEntry *e, const char *a, const char *b)
{
    short n = 0;
    e->state = kPicFailed;
    while (*a && n < (short)sizeof(e->why) - 1)
        e->why[n++] = *a++;
    while (b && *b && n < (short)sizeof(e->why) - 1)
        e->why[n++] = *b++;
    e->why[n] = 0;
}

/* Decode the picture of line lr into e (e->w x e->h). */
static void DecodePicture(DocPtr d, const unsigned char *t, const LineRec *lr, PicEntry *e)
{
    char name[DOCX_PIC_NAMEMAX + 1];
    short wPt, hPt, err, tref = 0;
    FileCtx fc, tc;
    WrSource src, pic;
    SubCtx sub;
    ZipEntry ze;
    ImgInfo info;
    PicSink ps;
    FSSpec tmp;
    long eof;

    if (!docx_pic_info(t, lr->start, lr->start + lr->len, &wPt, &hPt, name)) {
        SetWhy(e, "Picture", 0);
        return;
    }
    if (FSpOpenDF(&d->spec, fsRdPerm, &fc.refNum) != noErr) {
        SetWhy(e, "Picture: the document is gone", 0);
        return;
    }
    SetCursor(*GetCursor(watchCursor));
    GetEOF(fc.refNum, &eof);
    src.readAt = FileReadAt;
    src.ctx = &fc;
    src.size = (wr_u32)eof;
    pic = src;
    err = zip_find(&src, name, &ze);
    if (err != ZIP_OK) {
        SetWhy(e, "Picture missing: ", name);
    } else if (ze.method == 0) {
        sub.base = &src;
        sub.off = ze.dataOff;
        pic.readAt = SubReadAt;
        pic.ctx = &sub;
        pic.size = ze.size;
    } else {
        if (OpenTemp(&tmp, &tc.refNum) != noErr) {
            err = ZIP_ERR_READ;
            SetWhy(e, "Picture: no temporary file", 0);
        } else {
            tref = tc.refNum;
            err = zip_extract(&src, &ze, TempWrite, &tc.refNum);
            pic.readAt = FileReadAt;
            pic.ctx = &tc;
            pic.size = ze.size;
            if (err != ZIP_OK)
                SetWhy(e, err == ZIP_ERR_STOPPED ? "Picture: disk full" : "Picture: damaged", 0);
        }
    }
    if (err == ZIP_OK && img_probe(&pic, &info) == IMG_OK &&
        info.type != IMG_PNG && info.type != IMG_JPEG) {
        SetWhy(e, "Picture: ", img_type_name(info.type));
        err = IMG_ERR_FORMAT;
    }
    if (err == ZIP_OK) {
        e->rowBytes = (short)(gHasCQD ? ((e->w * 4L + 15) / 16) * 2 : ((e->w + 15L) / 16) * 2);
        e->bits = NewHandleClear((long)e->rowBytes * e->h);
        if (e->bits == 0) {
            SetWhy(e, "Picture: not enough memory", 0);
        } else {
            long t0 = TickCount();
            HLock(e->bits);
            ps.base = (unsigned char *)*e->bits;
            ps.rowBytes = e->rowBytes;
            ps.rows = 0;
            err = img_decode(&pic, e->w, e->h, PicRow, &ps);
            HUnlock(e->bits);
            t0 = TickCount() - t0;
            d->picCount++;
            d->picTicks += t0;
            if (t0 > d->picSlowest)
                d->picSlowest = t0;
            if (err == IMG_OK || (err == IMG_ERR_DATA && ps.rows > 0)) {
                e->state = kPicReady;
                HPurge(e->bits);                /* the Memory Manager may take it back */
            } else {
                DisposeHandle(e->bits);
                e->bits = 0;
                SetWhy(e, err == IMG_ERR_MEM ? "Picture: not enough memory" :
                          err == IMG_ERR_SIZE ? "Picture: too large" :
                          err == IMG_ERR_FORMAT ? "Picture: this kind of JPEG (CMYK?)" :
                          "Picture: damaged", 0);
            }
        }
    }
    if (tref) {
        FSClose(tref);
        FSpDelete(&tmp);
    }
    FSClose(fc.refNum);
}

/* The cache entry for the picture at text offset off, shown w x h. */
static PicEntry *FindPic(DocPtr d, long off, short w, short h)
{
    PicEntry *e = 0;
    short i;
    for (i = 0; i < kMaxPics; i++)
        if (d->pics[i].state != kPicNone && d->pics[i].off == off) {
            e = &d->pics[i];
            break;
        }
    if (e == 0) {                               /* a free slot, or the least recently drawn */
        e = &d->pics[0];
        for (i = 0; i < kMaxPics; i++) {
            if (d->pics[i].state == kPicNone) {
                e = &d->pics[i];
                break;
            }
            if (d->pics[i].used < e->used)
                e = &d->pics[i];
        }
    }
    if (e->state == kPicNone || e->off != off || e->w != w || e->h != h ||
        (e->state == kPicReady && (e->bits == 0 || *e->bits == 0))) {
        if (e->bits)
            DisposeHandle(e->bits);
        e->bits = 0;
        e->off = off;
        e->w = w;
        e->h = h;
        e->state = kPicNone;
        e->why[0] = 0;
    }
    return e;
}

static void PicFrame(const Rect *box, const char *why)
{
    short n = 0;
    PenPat(&qd.gray);
    FrameRect(box);
    PenNormal();
    while (why[n])
        n++;
    if (box->bottom - box->top >= 16 && box->right - box->left >= 40) {
        RgnHandle save = NewRgn();
        Rect in = *box;
        GetClip(save);
        InsetRect(&in, 2, 2);
        ClipRect(&in);                          /* intersected again below */
        SectRgn(save, qd.thePort->clipRgn, qd.thePort->clipRgn);
        TextFont(applFont);
        TextFace(0);
        TextSize(9);
        MoveTo((short)(box->left + 5), (short)(box->top + 13));
        DrawText((Ptr)why, 0, n);
        SetClip(save);
        DisposeRgn(save);
    }
}

static void DrawPic(DocPtr d, const unsigned char *t, const LineRec *lr, const Rect *box)
{
    PicEntry *e = FindPic(d, lr->start, lr->pw, lr->ph);
    Rect src;
    if (e->state == kPicNone) {
        PicFrame(box, "");
        DecodePicture(d, t, lr, e);
    }
    e->used = TickCount();
    if (e->state != kPicReady) {
        EraseRect(box);
        PicFrame(box, e->why);
        return;
    }
    SetRect(&src, 0, 0, e->w, e->h);
    HLock(e->bits);
    if (gHasCQD) {
        PixMap pm;
        char *z = (char *)&pm;
        short i;
        GDHandle gd = GetMainDevice();
        for (i = 0; i < (short)sizeof(pm); i++)  /* the tail differs between header versions */
            z[i] = 0;
        pm.baseAddr = *e->bits;
        pm.rowBytes = (short)(e->rowBytes | 0x8000);
        pm.bounds = src;
        pm.hRes = pm.vRes = 0x00480000;
        pm.pixelSize = 4;
        pm.cmpCount = 1;
        pm.cmpSize = 4;
        pm.pmTable = gGreyCT;
        ForeColor(blackColor);
        BackColor(whiteColor);
        CopyBits((BitMap *)&pm, &qd.thePort->portBits, &src, box,
                 (short)((**(**gd).gdPMap).pixelSize >= 4 ? srcCopy : ditherCopy), 0);
    } else {
        BitMap bm;
        bm.baseAddr = *e->bits;
        bm.rowBytes = e->rowBytes;
        bm.bounds = src;
        CopyBits(&bm, &qd.thePort->portBits, &src, box, srcCopy, 0);
    }
    HUnlock(e->bits);
}

static void FreePictures(DocPtr d)
{
    short i;
    for (i = 0; i < kMaxPics; i++)
        if (d->pics[i].bits) {
            DisposeHandle(d->pics[i].bits);
            d->pics[i].bits = 0;
        }
}

/* Draw line i whose top is at top; erase first if asked. The selection is
   inverted in the highlight colour; it runs to the right edge when it
   continues onto the next line, as in TextEdit. */
static void DrawLine(DocPtr d, const unsigned char *t, long i, short top,
                     const Rect *r, Boolean erase)
{
    LineRec *lr = (LineRec *)*d->lines + i;
    long a = lr->start, b = lr->start + lr->len, k, seg = a;
    long next = (i + 1 < d->nLines) ? lr[1].start : d->len + 1;
    short org = (short)(r->left + kMargin + lr->shift);
    short base = (short)(top + lr->asc);
    Walker wk;
    Rect lineR;

    SetRect(&lineR, r->left, top, r->right, (short)(top + lr->height));
    if (erase)
        EraseRect(&lineR);
    if (lr->pw > 0) {
        Rect box;
        box.left = (short)(org + lr->x0);
        box.top = (short)(top + lr->asc);
        box.right = (short)(box.left + lr->pw);
        box.bottom = (short)(box.top + lr->ph);
        DrawPic(d, t, lr, &box);
        if (d->active && d->selStart < b && d->selEnd > a)
            InvertRect(&box);       /* plain: highlight mode would only swap white */
        return;
    }
    WalkStart(&wk, lr);
    UseFont(wk.m);
    MoveTo((short)(org + wk.x), (short)(base - wk.rise));
    for (k = a; k <= b; k++) {
        unsigned char c = (k < b) ? t[k] : 0;
        Boolean special = (Boolean)(k == b || c < 0x20 || wk.m->missing[c]);
        if (!special) {
            WalkStep(&wk, c, lr->left);
            continue;
        }
        if (k > seg)
            DrawText((Ptr)(t + seg), 0, (short)(k - seg));
        seg = k + 1;
        if (k == b)
            break;
        if (DOCX_IS_FACE(c) || DOCX_IS_SIZE(c) || DOCX_IS_SUP(c)) {
            WalkStep(&wk, c, lr->left);
            UseFont(wk.m);
            MoveTo((short)(org + wk.x), (short)(base - wk.rise));
        } else if (c == '\t') {
            WalkStep(&wk, c, lr->left);
            MoveTo((short)(org + wk.x), (short)(base - wk.rise));
        } else if (c == DOCX_CH_EURO) {
            DrawEuro(wk.m->widths[c], wk.m->ascent, (Boolean)((wk.s.face & bold) != 0));
            WalkStep(&wk, c, lr->left);
        } else if (c == 0xE1) {                 /* middle dot, at half x-height */
            Point pen;
            Rect dot;
            short sz = (short)(wk.m->size >= 14 ? 2 : 1);
            GetPen(&pen);
            SetRect(&dot, (short)(pen.h + (wk.m->widths[c] - sz) / 2),
                    (short)(base - wk.rise - (wk.m->ascent * 3) / 8 - sz / 2), 0, 0);
            dot.right = (short)(dot.left + sz + ((wk.s.face & bold) ? 1 : 0));
            dot.bottom = (short)(dot.top + sz + ((wk.s.face & bold) ? 1 : 0));
            PaintRect(&dot);
            WalkStep(&wk, c, lr->left);
            MoveTo((short)(org + wk.x), (short)(base - wk.rise));
        } else if (c >= 0x20) {
            const char *sub = GlyphStandIn(c);
            short n = 0;
            while (sub[n])
                n++;
            DrawText((Ptr)sub, 0, n);
            WalkStep(&wk, c, lr->left);
        }
    }
    if (d->active && d->selEnd > d->selStart && d->selStart < next && d->selEnd > a) {
        Rect h = lineR;
        if (d->selStart > a)
            h.left = (short)(org + LineX(t, lr, d->selStart));
        if (d->selEnd <= b)
            h.right = (short)(org + LineX(t, lr, d->selEnd));
        LMSetHiliteMode((UInt8)(LMGetHiliteMode() & 0x7F));   /* highlight colour */
        InvertRect(&h);
    }
}

/* Redraw the visible lines touching text offsets [from, to]; from < 0: all. */
static void DrawLines(WindowPtr w, long from, long to)
{
    DocPtr d = DocOf(w);
    Rect r;
    RgnHandle saveClip;
    long i;
    short top;
    unsigned char *t;

    if (d == 0)
        return;
    SetPort(w);
    ContentRect(w, &r);
    saveClip = NewRgn();
    GetClip(saveClip);
    ClipRect(&r);
    if (from < 0)
        EraseRect(&r);
    HLock(d->text);
    HLock(d->lines);
    t = (unsigned char *)*d->text;
    top = (short)(r.top + kMargin / 2);
    for (i = d->top; i < d->nLines && top < r.bottom; i++) {
        LineRec *lr = (LineRec *)*d->lines + i;
        long next = (i + 1 < d->nLines) ? lr[1].start : d->len + 1;
        if (from < 0)
            DrawLine(d, t, i, top, &r, false);
        else if (from < next && to >= lr->start)
            DrawLine(d, t, i, top, &r, true);
        top += lr->height;
    }
    HUnlock(d->lines);
    HUnlock(d->text);
    UseFont(GetM(0, gBodyIdx));
    SetClip(saveClip);
    DisposeRgn(saveClip);
}

static void DrawDoc(WindowPtr w)
{
    DrawLines(w, -1, 0);
}

static void DrawWindow(WindowPtr w)
{
    DocPtr d = DocOf(w);
    SetPort(w);
    if (d)
        DrawDoc(w);
    DrawControls(w);
    DrawGrowIcon(w);
}

/* ------------------------------------------------------------- scrolling */

static void ScrollTo(WindowPtr w, long top)
{
    DocPtr d = DocOf(w);
    if (d == 0)
        return;
    if (top > d->maxTop) top = d->maxTop;
    if (top < 0) top = 0;
    if (top == d->top)
        return;
    d->top = top;
    SetControlValue(d->vbar, (short)(top / d->scale));
    DrawDoc(w);
}

/* Lines that fit in the window going down (dir > 0) or up from the top
   line, one text line of overlap kept. */
static long PageLines(WindowPtr w, short dir)
{
    DocPtr d = DocOf(w);
    Rect r;
    long i, n = 0, h = 0, avail;
    LineRec *lr;
    if (d == 0)
        return 1;
    ContentRect(w, &r);
    avail = r.bottom - r.top - kMargin - GetM(0, gBodyIdx)->height;
    lr = (LineRec *)*d->lines;
    if (dir > 0) {
        for (i = d->top; i < d->nLines && (h += lr[i].height) <= avail; i++)
            n++;
    } else {
        for (i = d->top - 1; i >= 0 && (h += lr[i].height) <= avail; i--)
            n++;
    }
    return n < 1 ? 1 : n;
}

static pascal void ScrollAction(ControlHandle c, short part)
{
    WindowPtr w = (*c)->contrlOwner;
    DocPtr d = DocOf(w);
    long delta = 0;
    if (d == 0)
        return;
    switch (part) {
    case kPartUp:   delta = -1; break;
    case kPartDown: delta = 1; break;
    case kPartPageUp:     delta = -PageLines(w, -1); break;
    case kPartPageDown:   delta = PageLines(w, 1); break;
    default: return;
    }
    ScrollTo(w, d->top + delta);
}

/* ------------------------------------------------------------ selection */

/* Text offset under a point in the window (local coordinates). */
static long PointToPos(WindowPtr w, Point pt)
{
    DocPtr d = DocOf(w);
    Rect r;
    long i, pos;
    short y;
    LineRec *lr;
    if (d == 0 || d->nLines == 0)
        return 0;
    ContentRect(w, &r);
    y = (short)(r.top + kMargin / 2);
    i = d->top;
    if (pt.v >= y) {
        for (; i < d->nLines; i++) {
            y += ((LineRec *)*d->lines)[i].height;
            if (pt.v < y)
                break;
        }
        if (i >= d->nLines) {
            lr = (LineRec *)*d->lines + d->nLines - 1;
            return lr->start + lr->len;
        }
    }
    HLock(d->text);
    lr = (LineRec *)*d->lines + i;
    pos = LinePos((unsigned char *)*d->text, lr, (short)(pt.h - r.left - kMargin - lr->shift));
    HUnlock(d->text);
    return pos;
}

/* Plain text for the outside world (clipboard, Save as Text): formatting
   bytes dropped, line breaks as CR, € spelt EUR (System 7 has no €).
   Writes the bytes for t[*i] to out (if given), advances *i, returns the count. */
static short PlainBytes(const unsigned char *t, long len, long *i, unsigned char *out)
{
    unsigned char c = t[*i];
    long skip = docx_ctrl_len(t, *i, len);
    if (c == DOCX_PIC) {
        static const char kPic[] = "[Picture]";
        short k;
        *i += skip;
        for (k = 0; out && k < 9; k++)
            out[k] = (unsigned char)kPic[k];
        return 9;
    }
    if (DOCX_IS_SUP(c)) {               /* a note reference: [1] */
        (*i)++;
        if (out)
            out[0] = (unsigned char)(c == DOCX_SUP_ON ? '[' : ']');
        return 1;
    }
    if (skip) {
        *i += skip;
        return 0;
    }
    (*i)++;
    if (c == DOCX_CH_EURO) {
        if (out) { out[0] = 'E'; out[1] = 'U'; out[2] = 'R'; }
        return 3;
    }
    if (out)
        out[0] = (unsigned char)(c == DOCX_CH_LINEBREAK ? '\r' : c);
    return 1;
}

static Boolean IsWordChar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    if (c == DOCX_CH_EURO)
        return true;
    if (c < 0x80)
        return false;
    /* MacRoman upper half: letters, except the punctuation among them */
    return !((c >= 0xC7 && c <= 0xC9) || (c >= 0xD0 && c <= 0xD5) ||
             c == 0xE2 || c == 0xE3 || c == 0xA5 || c == 0xCA);
}

static void WordAt(DocPtr d, long pos, long *a, long *b)
{
    unsigned char *t = (unsigned char *)*d->text;
    long ps, i, ws = -1;
    ParaInfo pi;
    *a = *b = pos;
    /* walk the paragraph from its first text byte: records and picture
       names look like text, run changes may sit inside a word */
    for (ps = pos; ps > 0 && t[ps - 1] != '\r'; ps--)
        ;
    i = ParsePara(t, ps, d->len, &pi);
    while (i < d->len && t[i] != '\r') {
        long cl = docx_ctrl_len(t, i, d->len);
        if (cl && t[i] != DOCX_PIC) {
            i += cl;
            continue;
        }
        if (cl == 0 && IsWordChar(t[i])) {
            if (ws < 0)
                ws = i;
            i++;
            continue;
        }
        if (i >= pos)
            break;
        ws = -1;
        i += cl ? cl : 1;
    }
    if (ws >= 0 && ws <= pos) {
        *a = ws;
        *b = i;
    }
}

/* Change the selection, redrawing only the lines whose highlight changes. */
static void SetSel(WindowPtr w, long s, long e)
{
    DocPtr d = DocOf(w);
    long os, oe;
    if (d == 0)
        return;
    if (s > e) { long x = s; s = e; e = x; }
    if (s < 0) s = 0;
    if (e > d->len) e = d->len;
    os = d->selStart;
    oe = d->selEnd;
    if (s == os && e == oe)
        return;
    d->selStart = s;
    d->selEnd = e;
    if (os == oe && s == e)
        return;                                 /* nothing shown before or after */
    if (s == os && os != oe && s != e)
        DrawLines(w, e < oe ? e : oe, e < oe ? oe : e);
    else if (e == oe && os != oe && s != e)
        DrawLines(w, s < os ? s : os, s < os ? os : s);
    else
        DrawLines(w, s < os ? s : os, e > oe ? e : oe);
}

static void TrackSelection(WindowPtr w, EventRecord *ev, Point pt)
{
    static long lastWhen = 0;
    static Point lastPt;
    DocPtr d = DocOf(w);
    Rect r;
    long pos, a0, b0, a, b, lastScroll = 0;
    Boolean words;

    if (d == 0)
        return;
    ContentRect(w, &r);
    pos = PointToPos(w, pt);
    words = (ev->when - lastWhen <= GetDblTime()) &&
            pt.h - lastPt.h < 4 && lastPt.h - pt.h < 4 &&
            pt.v - lastPt.v < 4 && lastPt.v - pt.v < 4;
    lastWhen = ev->when;
    lastPt = pt;
    if (words) {
        WordAt(d, pos, &a0, &b0);
    } else if (ev->modifiers & shiftKey) {     /* extend from the far end */
        a0 = b0 = (pos < d->selStart) ? d->selEnd : d->selStart;
    } else {
        a0 = b0 = pos;
    }
    do {
        if (pt.v < r.top || pt.v > r.bottom) {
            if (TickCount() - lastScroll >= 4) {
                ScrollTo(w, d->top + (pt.v < r.top ? -1 : 1));
                lastScroll = TickCount();
            }
        }
        pos = PointToPos(w, pt);
        if (words) {
            WordAt(d, pos, &a, &b);
            SetSel(w, a < a0 ? a : a0, b > b0 ? b : b0);
        } else {
            SetSel(w, pos < a0 ? pos : a0, pos > b0 ? pos : b0);
        }
        GetMouse(&pt);
    } while (StillDown());
}

/* Edit > Copy: the selection as plain MacRoman text, like Save as Text. */
static void DoCopy(WindowPtr w)
{
    DocPtr d = DocOf(w);
    Handle h;
    long i, n = 0, need = 0;
    unsigned char *t, *o;
    if (d == 0 || d->selEnd <= d->selStart)
        return;
    t = (unsigned char *)*d->text;
    for (i = d->selStart; i < d->selEnd; )
        need += PlainBytes(t, d->len, &i, 0);
    h = NewHandle(need);
    if (h == 0) {
        SysBeep(1);
        return;
    }
    HLock(h);
    HLock(d->text);
    t = (unsigned char *)*d->text;
    o = (unsigned char *)*h;
    for (i = d->selStart; i < d->selEnd; )     /* the same walk as the count */
        n += PlainBytes(t, d->len, &i, o + n);
    HUnlock(d->text);
    ZeroScrap();
    if (PutScrap(n, 'TEXT', (Ptr)*h) != noErr)
        SysBeep(1);
    DisposeHandle(h);
}

static void AdjustCursor(Point where)
{
    WindowPtr w = FrontWindow();
    Rect r;
    if (DocOf(w)) {
        GrafPtr save;
        GetPort(&save);
        SetPort(w);
        GlobalToLocal(&where);
        ContentRect(w, &r);
        SetPort(save);
        if (PtInRect(where, &r)) {
            SetCursor(*GetCursor(iBeamCursor));
            return;
        }
    }
    InitCursor();
}

/* ------------------------------------------------------------ documents */

/* the sink: append to the document's text handle */
static long TextAppend(void *ctx, const wr_u8 *buf, long n)
{
    DocPtr d = (DocPtr)ctx;
    if (d->len + n > d->cap) {
        long ncap = d->cap * 2;
        while (ncap < d->len + n)
            ncap *= 2;
        SetHandleSize(d->text, ncap);
        if (MemError() != noErr) {
            /* try a modest step before giving up */
            ncap = d->len + n + 8192;
            SetHandleSize(d->text, ncap);
            if (MemError() != noErr) {
                d->truncated = 1;
                return 1;
            }
        }
        d->cap = ncap;
    }
    BlockMoveData(buf, *d->text + d->len, n);
    d->len += n;
    Breathe();
    return 0;
}

static void SizeToScreen(Rect *r)
{
    Rect s = qd.screenBits.bounds;
    s.top += GetMBarHeight() + 20;   /* menu bar + title bar */
    *r = s;
    InsetRect(r, 4, 4);
    r->top = s.top;
    if (r->right - r->left > 560)      /* a comfortable reading width */
        r->right = (short)(r->left + 560);
}

static void OpenDocFile(FSSpec *spec)
{
    DocPtr d;
    WindowPtr w;
    Rect r, sb;
    FileCtx fc;
    WrSource src;
    long eof;
    short err;
    OSErr oe;
    Str255 why;
    static short stagger = 0;

    oe = FSpOpenDF(spec, fsRdPerm, &fc.refNum);
    if (oe != noErr) {
        Str255 n;
        why[0] = 0;
        CCat(why, "The file could not be opened (error ");
        NumToPStr(oe, n);
        PCat(why, n);
        CCat(why, ").");
        ErrorAlert(spec->name, why);
        return;
    }
    GetEOF(fc.refNum, &eof);

    d = (DocPtr)NewPtrClear(sizeof(DocRec));
    if (d == 0) {
        FSClose(fc.refNum);
        ErrorText(ZIP_ERR_MEM, why);
        ErrorAlert(spec->name, why);
        return;
    }
    d->spec = *spec;
    d->cap = eof * 2 + 4096;               /* text is usually smaller than the zip */
    if (d->cap > 256L * 1024)
        d->cap = 256L * 1024;
    d->text = NewHandle(d->cap);
    d->lines = NewHandle(0);
    if (d->text == 0 || d->lines == 0) {
        FSClose(fc.refNum);
        if (d->text) DisposeHandle(d->text);
        if (d->lines) DisposeHandle(d->lines);
        DisposePtr((Ptr)d);
        ErrorText(ZIP_ERR_MEM, why);
        ErrorAlert(spec->name, why);
        return;
    }

    SetCursor(*GetCursor(watchCursor));
    src.readAt = FileReadAt;
    src.ctx = &fc;
    src.size = (wr_u32)eof;
    d->ticks = TickCount();
    err = docx_convert(&src, TextAppend, d, &d->st);
    d->ticks = TickCount() - d->ticks;
    FSClose(fc.refNum);
    InitCursor();

    if (err == ZIP_ERR_STOPPED && d->truncated)
        err = DOCX_OK;                      /* show what fitted, say so in Info */
    if (err != DOCX_OK) {
        DisposeHandle(d->text);
        DisposeHandle(d->lines);
        DisposePtr((Ptr)d);
        ErrorText(err, why);
        ErrorAlert(spec->name, why);
        return;
    }
    SetHandleSize(d->text, d->len > 0 ? d->len : 1);   /* give back the slack */
    d->cap = d->len > 0 ? d->len : 1;

    SizeToScreen(&r);
    OffsetRect(&r, (short)(stagger * 12), (short)(stagger * 12));
    stagger = (short)((stagger + 1) % 5);
    if (gHasCQD)
        w = NewCWindow(0, &r, spec->name, false, zoomDocProc, (WindowPtr)-1L, true, (long)d);
    else
        w = NewWindow(0, &r, spec->name, false, zoomDocProc, (WindowPtr)-1L, true, (long)d);
    if (w == 0) {
        DisposeHandle(d->text);
        DisposeHandle(d->lines);
        DisposePtr((Ptr)d);
        return;
    }
    SetPort(w);
    sb = w->portRect;
    sb.left = (short)(sb.right - kSBarWidth);
    sb.right += 1;
    sb.top -= 1;
    sb.bottom -= (kSBarWidth - 1);
    d->vbar = NewControl(w, &sb, "\p", true, 0, 0, 0, kScrollBarDef, 0);
    Layout(w);
    ShowWindow(w);
    if (d->truncated) {
        Str255 m;
        m[0] = 0;
        CCat(m, "Not enough memory for the whole document; showing the first ");
        {
            Str255 n;
            NumToPStrSep(d->len / 1024, n);
            PCat(m, n);
        }
        CCat(m, " KB. Give WordReader more memory in the Finder's Get Info.");
        ErrorAlert(spec->name, m);
    }
}

static void CloseDoc(WindowPtr w)
{
    DocPtr d = DocOf(w);
    if (d == 0) {
        if (w && ((WindowPeek)w)->windowKind < 0)
            CloseDeskAcc(((WindowPeek)w)->windowKind);
        return;
    }
    DisposeWindow(w);
    FreePictures(d);
    DisposeHandle(d->text);
    DisposeHandle(d->lines);
    DisposePtr((Ptr)d);
}

static void DoOpen(void)
{
    StandardFileReply reply;
    /* every file: a .docx copied over a network arrives with any type */
    StandardGetFile(0, -1, 0, &reply);
    if (reply.sfGood)
        OpenDocFile(&reply.sfFile);
}

/* Save as Text: MacRoman, CR, no style markers - a TeachText/SimpleText file. */
static void DoSaveText(WindowPtr w)
{
    DocPtr d = DocOf(w);
    StandardFileReply reply;
    Str255 name;
    short ref, i;
    OSErr err;
    long p, n;
    char buf[1024];

    if (d == 0)
        return;
    name[0] = 0;
    for (i = 1; i <= d->spec.name[0] && name[0] < 26; i++) {
        if (d->spec.name[i] == '.')
            break;
        name[++name[0]] = d->spec.name[i];
    }
    CCat(name, ".txt");
    StandardPutFile("\pSave the text as:", name, &reply);
    if (!reply.sfGood)
        return;
    if (reply.sfReplacing)
        FSpDelete(&reply.sfFile);
    err = FSpCreate(&reply.sfFile, 'ttxt', 'TEXT', reply.sfScript);
    if (err == noErr)
        err = FSpOpenDF(&reply.sfFile, fsWrPerm, &ref);
    if (err != noErr) {
        Str255 m, num;
        m[0] = 0;
        CCat(m, "The text could not be saved (error ");
        NumToPStr(err, num);
        PCat(m, num);
        CCat(m, ").");
        ErrorAlert(reply.sfFile.name, m);
        return;
    }
    SetCursor(*GetCursor(watchCursor));
    HLock(d->text);
    p = 0;
    while (p < d->len && err == noErr) {
        n = 0;
        while (p < d->len && n < (long)sizeof(buf) - 2)     /* room for "EUR" */
            n += PlainBytes((unsigned char *)*d->text, d->len, &p, (unsigned char *)buf + n);
        err = FSWrite(ref, &n, buf);
    }
    HUnlock(d->text);
    FSClose(ref);
    FlushVol(0, reply.sfFile.vRefNum);
    InitCursor();
}

static void DoInfo(WindowPtr w)
{
    DocPtr d = DocOf(w);
    Str255 a, b, c, n;
    if (d == 0)
        return;
    a[0] = 0;
    CCat(a, d->st.format == DOCX_FMT_FLATXML ? "Word XML (single file)" :
            d->st.format == DOCX_FMT_ODT ? "OpenDocument text" :
            d->st.format == DOCX_FMT_FLATODT ? "OpenDocument flat XML" : "Office Open XML package");
    CCat(a, ", part ");
    CCat(a, d->st.part[0] ? d->st.part : "-");
    CCat(a, ". ");
    NumToPStrSep((long)(d->st.xmlBytes / 1024), n);
    PCat(a, n);
    CCat(a, " KB XML -> ");
    NumToPStrSep(d->len, n);
    PCat(a, n);
    CCat(a, " characters in ");
    NumToPStrSep(d->ticks / 60, n);
    PCat(a, n);
    CCat(a, ".");
    NumToPStrSep((d->ticks % 60) * 10 / 60, n);
    PCat(a, n);
    CCat(a, " s.");

    b[0] = 0;
    NumToPStrSep(d->st.paragraphs, n); PCat(b, n); CCat(b, " paragraphs, ");
    NumToPStrSep(d->st.tables, n);     PCat(b, n); CCat(b, " tables, ");
    NumToPStrSep(d->st.pictures, n);   PCat(b, n); CCat(b, " pictures, ");
    if (d->st.footnotes + d->st.endnotes > 0) {
        NumToPStrSep(d->st.footnotes, n); PCat(b, n); CCat(b, " footnotes, ");
        NumToPStrSep(d->st.endnotes, n);  PCat(b, n); CCat(b, " endnotes, ");
    }
    NumToPStrSep(d->nLines, n);        PCat(b, n); CCat(b, " lines.");
    if (d->picCount > 0) {
        CCat(b, " Pictures decoded so far: ");
        NumToPStrSep(d->picCount, n);       PCat(b, n); CCat(b, ", in ");
        NumToPStrSep(d->picTicks / 60, n);  PCat(b, n); CCat(b, ".");
        NumToPStrSep((d->picTicks % 60) * 10 / 60, n); PCat(b, n);
        CCat(b, " s (slowest ");
        NumToPStrSep(d->picSlowest / 60, n); PCat(b, n); CCat(b, ".");
        NumToPStrSep((d->picSlowest % 60) * 10 / 60, n); PCat(b, n);
        CCat(b, " s).");
    }
    if (d->truncated)
        CCat(b, " Truncated: not enough memory.");

    c[0] = 0;
    CCat(c, "Unicode -> Mac OS Roman: ");
    NumToPStrSep(d->st.euros, n);
    PCat(c, n);
    CCat(c, " euro signs drawn by WordReader, ");
    NumToPStrSep(d->st.substituted, n);
    PCat(c, n);
    CCat(c, " spelt with stand-ins (1/2, plain L for Polish L...), ");
    NumToPStrSep(d->st.lossy, n);
    PCat(c, n);
    CCat(c, " not representable, shown as '?'.");
    ParamText(d->spec.name, a, b, c);
    InitCursor();
    NoteAlert(rAlertInfo, 0);
}

/* ---------------------------------------------------------------- menus */

static void AdjustMenus(void)
{
    WindowPtr w = FrontWindow();
    MenuHandle file = GetMenuHandle(mFile), edit = GetMenuHandle(mEdit);
    Boolean isDoc = DocOf(w) != 0;
    Boolean isDA = w != 0 && ((WindowPeek)w)->windowKind < 0;
    if (w) EnableItem(file, iClose); else DisableItem(file, iClose);
    if (isDoc) { EnableItem(file, iSaveText); EnableItem(file, iInfo); }
    else { DisableItem(file, iSaveText); DisableItem(file, iInfo); }
    if (isDA) {
        short i;
        EnableItem(edit, 0);
        for (i = iUndo; i <= iSelectAll; i++)
            EnableItem(edit, i);
    } else if (isDoc) {
        DocPtr d = DocOf(w);
        EnableItem(edit, 0);
        DisableItem(edit, iUndo);
        DisableItem(edit, iCut);
        DisableItem(edit, iPaste);              /* a reader: nothing to paste into */
        DisableItem(edit, iClear);
        if (d->selEnd > d->selStart) EnableItem(edit, iCopy); else DisableItem(edit, iCopy);
        EnableItem(edit, iSelectAll);
    } else
        DisableItem(edit, 0);
    DrawMenuBar();
}

static void SetTextSize(short item)
{
    static const short kSizes[5] = { 9, 10, 12, 14, 18 };
    MenuHandle view = GetMenuHandle(mView);
    WindowPtr w;
    short i;
    if (item < 1 || item > 5)
        return;
    for (i = 1; i <= 5; i++)
        CheckItem(view, i, i == item);
    SetBodySize(kSizes[item - 1]);
    for (w = FrontWindow(); w; w = (WindowPtr)((WindowPeek)w)->nextWindow) {
        DocPtr d = DocOf(w);
        if (d) {
            /* keep the first visible paragraph in view */
            long anchor = d->top < d->nLines ? ((LineRec *)*d->lines)[d->top].start : 0, k;
            Layout(w);
            for (k = 0; k < d->nLines && ((LineRec *)*d->lines)[k].start < anchor; k++)
                ;
            d->top = k > 0 ? k - 1 : 0;
            if (d->top > d->maxTop)
                d->top = d->maxTop;
            SetControlValue(d->vbar, (short)(d->top / d->scale));
            SetPort(w);
            InvalRect(&w->portRect);
        }
    }
}

static void DoMenu(long mr)
{
    short menu = HiWord(mr), item = LoWord(mr);
    Str255 name;
    switch (menu) {
    case mApple:
        if (item == iAbout) {
            InitCursor();
            NoteAlert(rAlertAbout, 0);
        } else {
            GetMenuItemText(GetMenuHandle(mApple), item, name);
            OpenDeskAcc(name);
        }
        break;
    case mFile:
        switch (item) {
        case iOpen:     DoOpen(); break;
        case iClose:    CloseDoc(FrontWindow()); break;
        case iSaveText: DoSaveText(FrontWindow()); break;
        case iInfo:     DoInfo(FrontWindow()); break;
        case iQuit:     gQuit = 1; break;
        }
        break;
    case mEdit:
        if (!SystemEdit((short)(item - 1))) {
            WindowPtr w = FrontWindow();
            DocPtr d = DocOf(w);
            if (d && item == iCopy)
                DoCopy(w);
            else if (d && item == iSelectAll)
                SetSel(w, 0, d->len);
        }
        break;
    case mView:
        SetTextSize(item);
        break;
    }
    HiliteMenu(0);
}

/* --------------------------------------------------------------- events */

static void Resize(WindowPtr w, short wd, short ht)
{
    DocPtr d = DocOf(w);
    SetPort(w);
    SizeWindow(w, wd, ht, true);
    if (d) {
        MoveControl(d->vbar, (short)(w->portRect.right - kSBarWidth), -1);
        SizeControl(d->vbar, kSBarWidth + 1,
                    (short)(w->portRect.bottom - w->portRect.top - kSBarWidth + 2));
        Layout(w);
    }
    InvalRect(&w->portRect);
}

static void DoMouseDown(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    switch (part) {
    case inMenuBar:
        AdjustMenus();
        DoMenu(MenuSelect(ev->where));
        break;
    case inSysWindow:
        SystemClick(ev, w);
        break;
    case inDrag:
        DragWindow(w, ev->where, &qd.screenBits.bounds);
        break;
    case inGoAway:
        if (TrackGoAway(w, ev->where))
            CloseDoc(w);
        break;
    case inGrow: {
        Rect lim;
        long sz;
        SetRect(&lim, 160, 100, 2000, 2000);
        sz = GrowWindow(w, ev->where, &lim);
        if (sz)
            Resize(w, LoWord(sz), HiWord(sz));
        break;
    }
    case inZoomIn:
    case inZoomOut:
        if (TrackBox(w, ev->where, part)) {
            SetPort(w);
            EraseRect(&w->portRect);
            ZoomWindow(w, part, true);
            Resize(w, (short)(w->portRect.right - w->portRect.left),
                   (short)(w->portRect.bottom - w->portRect.top));
        }
        break;
    case inContent:
        if (w != FrontWindow()) {
            SelectWindow(w);
        } else {
            DocPtr d = DocOf(w);
            ControlHandle c;
            Point pt = ev->where;
            short cpart;
            SetPort(w);
            GlobalToLocal(&pt);
            cpart = FindControl(pt, w, &c);
            if (d && c == d->vbar) {
                if (cpart == kPartThumb) {
                    TrackControl(c, pt, 0);
                    ScrollTo(w, (long)GetControlValue(c) * d->scale);
                } else if (cpart) {
                    TrackControl(c, pt, gScrollUPP);
                }
            } else if (d) {
                TrackSelection(w, ev, pt);
            }
        }
        break;
    }
}

static void DoKey(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    WindowPtr w = FrontWindow();
    DocPtr d = DocOf(w);
    if (ev->modifiers & cmdKey) {
        AdjustMenus();
        DoMenu(MenuKey(ch));
        return;
    }
    if (d == 0)
        return;
    switch (ch) {
    case 0x1E: ScrollTo(w, d->top - 1); break;                 /* up */
    case 0x1F: ScrollTo(w, d->top + 1); break;                 /* down */
    case 0x0B: ScrollTo(w, d->top - PageLines(w, -1)); break;  /* page up */
    case 0x0C: case ' ': ScrollTo(w, d->top + PageLines(w, 1)); break;
    case 0x01: ScrollTo(w, 0); break;                          /* home */
    case 0x04: ScrollTo(w, d->maxTop); break;                  /* end */
    }
}

static void DoActivate(WindowPtr w, Boolean on)
{
    DocPtr d = DocOf(w);
    SetPort(w);
    if (d) {
        HiliteControl(d->vbar, on ? 0 : 255);
        if (d->active != on) {
            d->active = on;
            if (d->selEnd > d->selStart)
                DrawLines(w, d->selStart, d->selEnd);
        }
    }
    DrawGrowIcon(w);
}

/* --------------------------------------------------------- Apple events */

static pascal OSErr AEOpenApp(const AppleEvent *e, AppleEvent *r, long refcon)
{
#pragma unused(e, r, refcon)
    return noErr;
}

static pascal OSErr AEOpenDocs(const AppleEvent *e, AppleEvent *r, long refcon)
{
#pragma unused(r, refcon)
    AEDescList list;
    long n, i, size;
    AEKeyword kw;
    DescType type;
    FSSpec spec;
    OSErr err = AEGetParamDesc(e, keyDirectObject, typeAEList, &list);
    if (err != noErr)
        return err;
    err = AECountItems(&list, &n);
    for (i = 1; err == noErr && i <= n; i++) {
        err = AEGetNthPtr(&list, i, typeFSS, &kw, &type, (Ptr)&spec, sizeof(spec), &size);
        if (err == noErr)
            OpenDocFile(&spec);
    }
    AEDisposeDesc(&list);
    return err;
}

static pascal OSErr AEQuitApp(const AppleEvent *e, AppleEvent *r, long refcon)
{
#pragma unused(e, r, refcon)
    gQuit = 1;
    return noErr;
}

static void InstallAEs(void)
{
    long resp;
    if (Gestalt(gestaltAppleEventsAttr, &resp) != noErr)
        return;
    gHasAE = true;
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
                          NewAEEventHandlerUPP(AEOpenApp), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                          NewAEEventHandlerUPP(AEOpenDocs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,
                          NewAEEventHandlerUPP(AEOpenDocs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                          NewAEEventHandlerUPP(AEQuitApp), 0, false);
}

/* ----------------------------------------------------------------- main */

static void SetupMenus(void)
{
    Handle bar = GetNewMBar(rMenuBar);
    SetMenuBar(bar);
    AppendResMenu(GetMenuHandle(mApple), 'DRVR');
    CheckItem(GetMenuHandle(mView), 3, true);     /* 12 point */
    DrawMenuBar();
}

int main(void)
{
    EventRecord ev;
    WindowPtr w;

    MaxApplZone();
    MoreMasters();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    FlushEvents(everyEvent, 0);
    InitCursor();

    gScrollUPP = NewControlActionUPP(ScrollAction);
    {
        long qdv;
        if (Gestalt(gestaltQuickdrawVersion, &qdv) == noErr && qdv >= gestalt8BitQD) {
            short i;
            gGreyCT = (CTabHandle)NewHandleClear(sizeof(ColorTable) + 15 * sizeof(ColorSpec));
            if (gGreyCT) {                      /* 16 greys, index 0 white */
                gHasCQD = true;
                (**gGreyCT).ctSeed = GetCTSeed();
                (**gGreyCT).ctSize = 15;
                for (i = 0; i < 16; i++) {
                    ColorSpec *c = &(**gGreyCT).ctTable[i];
                    c->value = i;
                    c->rgb.red = c->rgb.green = c->rgb.blue = (unsigned short)(65535L - i * 4369L);
                }
                for (i = 0; i < 16; i++) {      /* ordered dither, no divide per pixel */
                    long g;
                    for (g = 0; g < 256; g++)
                        gDither[i][g] = (unsigned char)(15 - (g * 240 + i * 255 + 127) / 4080);
                }
            }
        }
    }
    SetupMenus();
    SetBodySize(12);
    InstallAEs();

    while (!gQuit) {
        if (!WaitNextEvent(everyEvent, &ev, 10, 0)) {
            AdjustCursor(ev.where);
            continue;
        }
        switch (ev.what) {
        case mouseDown:
            DoMouseDown(&ev);
            break;
        case keyDown:
        case autoKey:
            DoKey(&ev);
            break;
        case updateEvt:
            w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (DocOf(w))
                DrawWindow(w);
            EndUpdate(w);
            break;
        case activateEvt:
            DoActivate((WindowPtr)ev.message, (ev.modifiers & activeFlag) != 0);
            break;
        case kHighLevelEvent:
            if (gHasAE)
                AEProcessAppleEvent(&ev);
            break;
        case osEvt:
            if (((ev.message >> 24) & 0xFF) == suspendResumeMessage) {
                w = FrontWindow();
                if (w)
                    DoActivate(w, (ev.message & resumeFlag) != 0);
            }
            break;
        }
    }
    while ((w = FrontWindow()) != 0 && DocOf(w))
        CloseDoc(w);
    return 0;
}
