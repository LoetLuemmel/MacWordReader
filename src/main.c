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

#include "docx.h"
#include "zipdir.h"

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
#define iCopy         4
#define iSelectAll    7
#define rAlertError   128
#define rAlertAbout   129
#define rAlertInfo    130

enum { kStyleBody = 0, kStyleTitle, kStyleH1, kStyleH2, kStyleH3, kStyleCount };

typedef struct {
    long          start;     /* offset of the first byte in the text */
    short         len;       /* bytes on this line, without the CR */
    unsigned char style;
    unsigned char paraEnd;   /* last line of a paragraph: a little air below */
} LineRec;

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
} DocRec, *DocPtr;

/* per-style metrics for the current text size */
typedef struct {
    short font, size, face;
    short ascent, height;
    short widths[256];
    unsigned char missing[256];   /* the font has no glyph: draw a stand-in */
} StyleMetrics;

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

static StyleMetrics gMetrics[kStyleCount];
static short gBaseSize = 12;
static short gQuit = 0;
static long  gLastYield = 0;
static Boolean gHasAE = false;
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
        CCat(msg, "This is an Office document, but not a Word one (Excel or PowerPoint?).");
        break;
    case DOCX_ERR_UTF16:
        CCat(msg, "This XML file is UTF-16 encoded; only UTF-8 Word XML is supported.");
        break;
    case DOCX_ERR_FORMAT:
    case ZIP_ERR_NOTZIP:
        CCat(msg, "This is not a Word document (.docx, .docm, .dotx, Word XML).");
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

static void SetStyle(short style)
{
    TextFont(gMetrics[style].font);
    TextSize(gMetrics[style].size);
    TextFace(gMetrics[style].face);
}

static void FindMissingGlyphs(void);

static void ComputeMetrics(void)
{
    short s, c;
    FontInfo fi;
    GrafPtr save;
    GetPort(&save);
    SetPort((GrafPtr)FrontWindow() ? FrontWindow() : save);
    for (s = 0; s < kStyleCount; s++) {
        StyleMetrics *m = &gMetrics[s];
        m->font = applFont;                 /* Geneva: crisp on the Duo's LCD */
        m->face = (s == kStyleBody) ? 0 : bold;
        m->size = gBaseSize;
        if (s == kStyleTitle) m->size = (short)(gBaseSize + 8);
        if (s == kStyleH1)    m->size = (short)(gBaseSize + 6);
        if (s == kStyleH2)    m->size = (short)(gBaseSize + 2);
        SetStyle(s);
        GetFontInfo(&fi);
        m->ascent = fi.ascent;
        m->height = (short)(fi.ascent + fi.descent + fi.leading);
        if (s != kStyleBody)
            m->height += gBaseSize / 2;     /* air above a heading */
        for (c = 0; c < 256; c++)
            m->widths[c] = CharWidth((char)c);
        m->widths['\t'] = 0;
        m->widths['\r'] = 0;
    }
    SetStyle(kStyleBody);
    SetPort(save);
    FindMissingGlyphs();
}

/* Which MacRoman characters does the font in use actually have? FMSwapFont
   hands back the strike QuickDraw will draw with; in a bitmap font record
   ('FONT'/'NFNT') the offset/width table holds -1 for every absent glyph
   (Inside Macintosh: Text 4-71). A TrueType font answers with an 'sfnt'
   instead - left alone, it has the whole MacRoman set. */
static void FindMissingGlyphs(void)
{
    short s, c;
    for (s = 0; s < kStyleCount; s++) {
        StyleMetrics *m = &gMetrics[s];
        FMInput in;
        FMOutPtr out;
        FontRec *fr;
        short *owt;
        long off;

        for (c = 0; c < 256; c++)
            m->missing[c] = 0;
        in.family = m->font;
        in.size = m->size;
        in.face = (Style)m->face;
        in.needBits = true;
        in.device = 0;
        in.numer.h = in.numer.v = 1;
        in.denom.h = in.denom.v = 1;
        out = FMSwapFont(&in);
        if (out == 0 || out->fontHandle == 0 || *out->fontHandle == 0)
            continue;
        fr = (FontRec *)*out->fontHandle;
        if ((fr->fontType & 0x9000) != 0x9000)      /* not a bitmap strike */
            continue;
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
                SetStyle(s);
                m->widths[c] = TextWidth((Ptr)sub, 0, n);
            }
        }
    }
    for (s = 0; s < kStyleCount; s++) {
        gMetrics[s].missing[DOCX_CH_EURO] = 1;
        gMetrics[s].widths[DOCX_CH_EURO] = gMetrics[s].widths['0'];
    }
    SetStyle(kStyleBody);
}

/* ---------------------------------------------------------------- layout */

static void ContentRect(WindowPtr w, Rect *r)
{
    *r = w->portRect;
    r->right -= kSBarWidth;
}

/* Height of one line on screen: the style's line height, plus some air after
   a paragraph (Word's "space after"). */
static short LineHeight(const LineRec *lr)
{
    return (short)(gMetrics[lr->style].height + (lr->paraEnd ? gBaseSize / 3 : 0));
}

static Boolean AddLine(DocPtr d, long start, long len, short style)
{
    LineRec *lr;
    if (d->nLines >= d->linesCap) {
        long ncap = d->linesCap ? d->linesCap * 2 : 1024;
        SetHandleSize(d->lines, ncap * (long)sizeof(LineRec));
        if (MemError() != noErr)
            return false;
        d->linesCap = ncap;
    }
    lr = (LineRec *)*d->lines + d->nLines++;
    lr->start = start;
    lr->len = (short)len;
    lr->style = (unsigned char)style;
    lr->paraEnd = 0;
    return true;
}

static short NextTab(short x)
{
    return (short)((x / kTabWidth + 1) * kTabWidth);
}

static void Layout(WindowPtr w)
{
    DocPtr d = DocOf(w);
    Rect r;
    short maxW, x, style, cw;
    long i, p, end, lineStart, lastBreak;
    unsigned char *t;
    long h, avail;

    if (d == 0)
        return;
    ContentRect(w, &r);
    maxW = (short)(r.right - r.left - 2 * kMargin);
    if (maxW < 40)
        maxW = 40;
    d->nLines = 0;

    HLock(d->text);
    t = (unsigned char *)*d->text;
    p = 0;
    while (p < d->len) {
        end = p;
        while (end < d->len && t[end] != '\r')
            end++;
        style = kStyleBody;
        if (end > p && DOCX_IS_MARKER(t[p])) {
            style = t[p];
            p++;
        }
        lineStart = p;
        lastBreak = -1;
        x = 0;
        for (i = p; i < end; i++) {
            unsigned char c = t[i];
            cw = (c == '\t') ? (short)(NextTab(x) - x) : gMetrics[style].widths[c];
            if ((x + cw > maxW && i > lineStart) || i - lineStart > 2000) {
                long brk = (lastBreak > lineStart) ? lastBreak : i;
                long k;
                if (!AddLine(d, lineStart, brk - lineStart, style))
                    goto done;
                lineStart = brk;
                lastBreak = -1;
                x = 0;
                for (k = lineStart; k < i; k++)
                    x += (t[k] == '\t') ? (short)(NextTab(x) - x) : gMetrics[style].widths[t[k]];
                cw = (c == '\t') ? (short)(NextTab(x) - x) : gMetrics[style].widths[c];
            }
            x += cw;
            if (c == ' ' || c == '-' || c == '\t')
                lastBreak = i + 1;
        }
        if (!AddLine(d, lineStart, end - lineStart, style))
            goto done;
        ((LineRec *)*d->lines)[d->nLines - 1].paraEnd = 1;
        p = end + 1;
    }
done:
    HUnlock(d->text);

    /* the last top line that still fills the window */
    avail = r.bottom - r.top - kMargin;
    h = 0;
    for (i = d->nLines - 1; i >= 0; i--) {
        h += LineHeight((LineRec *)*d->lines + i);
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

static void DrawDoc(WindowPtr w)
{
    DocPtr d = DocOf(w);
    Rect r;
    RgnHandle saveClip;
    long i;
    short y;
    unsigned char *t;

    if (d == 0)
        return;
    SetPort(w);
    ContentRect(w, &r);
    saveClip = NewRgn();
    GetClip(saveClip);
    ClipRect(&r);
    EraseRect(&r);

    HLock(d->text);
    HLock(d->lines);
    t = (unsigned char *)*d->text;
    y = (short)(r.top + kMargin / 2);
    for (i = d->top; i < d->nLines && y < r.bottom; i++) {
        LineRec *lr = (LineRec *)*d->lines + i;
        StyleMetrics *m = &gMetrics[lr->style];
        short x = 0, segStart = 0, k;
        y += LineHeight(lr);
        SetStyle(lr->style);
        /* baseline: the line's top, the air above a heading, then the ascent */
        MoveTo((short)(r.left + kMargin),
               (short)(y - LineHeight(lr) + m->ascent + (lr->style != kStyleBody ? gBaseSize / 2 : 0)));
        for (k = 0; k <= lr->len; k++) {
            unsigned char c = (k < lr->len) ? t[lr->start + k] : 0;
            if (k == lr->len || c == '\t' || m->missing[c]) {
                if (k > segStart)
                    DrawText((Ptr)(t + lr->start), segStart, (short)(k - segStart));
                if (k < lr->len && c == '\t') {
                    short pen;
                    Point pt;
                    GetPen(&pt);
                    pen = (short)(pt.h - r.left - kMargin);
                    x = NextTab(pen);
                    MoveTo((short)(r.left + kMargin + x), pt.v);
                } else if (k < lr->len && c == DOCX_CH_EURO) {
                    DrawEuro(m->widths[c], m->ascent, (Boolean)(m->face != 0));
                } else if (k < lr->len) {
                    const char *sub = GlyphStandIn(c);
                    short n = 0;
                    while (sub[n])
                        n++;
                    DrawText((Ptr)sub, 0, n);
                }
                segStart = (short)(k + 1);
            }
        }
    }
    HUnlock(d->lines);
    HUnlock(d->text);
    SetStyle(kStyleBody);
    SetClip(saveClip);
    DisposeRgn(saveClip);
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

static long PageLines(WindowPtr w)
{
    Rect r;
    long n;
    ContentRect(w, &r);
    n = (r.bottom - r.top) / gMetrics[kStyleBody].height - 1;
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
    case kPartPageUp:     delta = -PageLines(w); break;
    case kPartPageDown:   delta = PageLines(w); break;
    default: return;
    }
    ScrollTo(w, d->top + delta);
}

/* ------------------------------------------------------------ documents */

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
        while (p < d->len && n < (long)sizeof(buf) - 2) {   /* room for "EUR" */
            unsigned char c = (unsigned char)(*d->text)[p++];
            if (c == DOCX_CH_EURO) {        /* TeachText under System 7 has no € */
                buf[n++] = 'E'; buf[n++] = 'U'; buf[n++] = 'R';
            } else if (!DOCX_IS_MARKER(c))
                buf[n++] = (char)c;
        }
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
    CCat(a, d->st.format == DOCX_FMT_FLATXML ? "Word XML (single file)" : "Office Open XML package");
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
    NumToPStrSep(d->st.pictures, n);   PCat(b, n); CCat(b, " pictures (not shown), ");
    NumToPStrSep(d->nLines, n);        PCat(b, n); CCat(b, " lines.");
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
    if (isDA) EnableItem(edit, 0); else DisableItem(edit, 0);
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
    gBaseSize = kSizes[item - 1];
    for (i = 1; i <= 5; i++)
        CheckItem(view, i, i == item);
    ComputeMetrics();
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
        SystemEdit((short)(item - 1));
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
    case 0x0B: ScrollTo(w, d->top - PageLines(w)); break;      /* page up */
    case 0x0C: case ' ': ScrollTo(w, d->top + PageLines(w)); break;
    case 0x01: ScrollTo(w, 0); break;                          /* home */
    case 0x04: ScrollTo(w, d->maxTop); break;                  /* end */
    }
}

static void DoActivate(WindowPtr w, Boolean on)
{
    DocPtr d = DocOf(w);
    SetPort(w);
    if (d)
        HiliteControl(d->vbar, on ? 0 : 255);
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
    SetupMenus();
    ComputeMetrics();
    InstallAEs();

    while (!gQuit) {
        if (!WaitNextEvent(everyEvent, &ev, 30, 0))
            continue;
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
