/*
 * docx.c - WordprocessingML -> MacRoman text, streaming.
 *
 * The XML is never held in memory: bytes arrive from the inflater in 32 KB
 * pieces and run through a small state machine (text / tag / comment / CDATA),
 * which reacts to the handful of WordprocessingML elements that carry text
 * or structure. Element names are matched by local name, so the prefix does
 * not matter (w: in Transitional, the same in Strict, wx: and friends in
 * Word 2003 XML).
 *
 *   w:p           paragraph          -> CR (in a table cell: a space)
 *   w:pStyle      Heading n / Title  -> a style marker byte
 *   w:numPr/ilvl  list item          -> indent + bullet
 *   w:t           text run           -> the text, UTF-8 -> MacRoman
 *   w:tab w:br    in a run           -> tab, CR
 *   w:tc w:tr     table cell / row   -> tab, CR
 *   pic:pic       picture            -> "[Picture]"
 *   mc:Fallback   duplicate content  -> skipped
 */
#include "docx.h"
#include "zipdir.h"
#include "macroman.h"

#define TAGMAX 600          /* ODF style tags carry many attributes */

enum { M_TEXT, M_TAG, M_COMMENT, M_CDATA };

struct OdfCtx;

typedef struct {
    WrWriteFn  wr;
    void      *wctx;
    DocxStats *st;
    short      stopped;

    wr_u8  out[512];
    short  outLen;

    /* lexer */
    short  mode;
    char   tag[TAGMAX + 1];
    short  tagLen;
    char   quote;
    char   tail[3];         /* last chars inside a comment / CDATA */
    short  inEnt;
    char   ent[12];
    short  entLen;
    wr_u32 cp;              /* UTF-8 in progress */
    short  need;

    /* document state */
    short  inT;             /* inside a text element */
    short  runDepth;
    short  skipDepth;       /* inside mc:Fallback */
    short  tblDepth;
    short  inPara;
    short  paraStarted;     /* prefix written for this paragraph */
    wr_u8  marker;
    short  isList, listLevel;
    short  cellSep;         /* a cell's earlier paragraph wants a space */
    short  pendTab;         /* a closed cell wants a tab before more text */

    /* formatting: defaults from styles.xml, lists from numbering.xml */
    short  inPPr, inRPr, inDocDef, inDefStyle;
    short  defSz;           /* body text size, half-points */
    long   defBefore, defAfter;          /* twips */
    long   before, after, left, first;   /* this paragraph, twips */
    short  align, hasInd, numId, paraRec;
    short  runFace, runSz;  /* the current run */
    short  emFace, emStep;  /* what the output says right now */
    short  curAbs, curLvl, curNum;
    short  nLvl, nNum;
    struct { short abs, lvl, left, hang; } lvls[64];
    struct { short num, abs; } nums[64];

    struct OdfCtx *odf;     /* set while reading OpenDocument */
} Parser;

/* ---------- OpenDocument: styles resolved through their parents ---------- */

#define OH_BEFORE 1
#define OH_AFTER  2
#define OH_LEFT   4
#define OH_FIRST  8
#define OH_ALIGN  16
#define OH_SZ     32
#define ODF_MAXSTYLES 160

typedef struct {
    char   name[32], parent[32];
    char   family;                  /* 'p' paragraph, 't' text */
    wr_u8  marker;                  /* heading look from the style's name */
    short  has;                     /* OH_* */
    long   before, after, left, first;     /* twips */
    short  align;
    short  faceMask, faceVal;
    short  sz;                      /* half-points; < 0: percent of the parent */
} OdfStyle;

typedef struct OdfCtx {
    OdfStyle s[ODF_MAXSTYLES];
    short  n;
    short  cur;                     /* style being read; -2: the default style; -1: none */
    short  listDepth, itemFresh;
    short  spanDepth;
    short  stackFace[8], stackSz[8];
    short  frameLevel;              /* skipDepth inside the current draw:frame */
    short  space;                   /* last output was collapsible white space */
} OdfCtx;

static short AttrVal(const char *tag, const char *attr, char *val, short max);
static short Eq(const char *a, const char *b);

/* ---------- output ---------- */

static void Flush(Parser *p, short keepLast)
{
    short n = (short)(keepLast && p->outLen > 0 ? p->outLen - 1 : p->outLen);
    if (n > 0 && !p->stopped) {
        if (p->wr(p->wctx, p->out, n) != 0)
            p->stopped = 1;
    }
    if (n > 0 && n < p->outLen)
        p->out[0] = p->out[n];
    p->outLen = (short)(p->outLen - n);
}

static void Emit(Parser *p, wr_u8 b)
{
    if (p->outLen == (short)sizeof(p->out))
        Flush(p, 1);       /* keep the last byte so a combining mark can still fix it */
    p->out[p->outLen++] = b;
    p->st->chars++;
}

static void EmitStr(Parser *p, const char *s)
{
    while (*s)
        Emit(p, (wr_u8)*s++);
}

static wr_u8 Clamp200(long v)
{
    return (wr_u8)(0x20 + (v < 0 ? 0 : v > 200 ? 200 : v));
}

/* The paragraph record: spacing, indents, alignment (see docx.h). */
static void EmitParaRec(Parser *p)
{
    long left = p->left, first = p->first;
    short i;
    if (p->paraRec || p->tblDepth > 0)
        return;
    p->paraRec = 1;
    if (p->isList && !p->hasInd) {
        left = 720L * (p->listLevel + 1);     /* Word's default list indents */
        first = -360;
        for (i = 0; i < p->nNum; i++)
            if (p->nums[i].num == p->numId) {
                short k;
                for (k = 0; k < p->nLvl; k++)
                    if (p->lvls[k].abs == p->nums[i].abs && p->lvls[k].lvl == p->listLevel) {
                        left = p->lvls[k].left;
                        first = -p->lvls[k].hang;
                    }
                break;
            }
    }
    Emit(p, DOCX_PARA);
    Emit(p, Clamp200(p->before / 20));
    Emit(p, Clamp200(p->after / 20));
    Emit(p, Clamp200(left / 40));
    Emit(p, Clamp200(first / 40 + 100));
    Emit(p, (wr_u8)(0x20 + p->align));
    p->st->chars -= DOCX_PARA_LEN;
}

static short SizeStep(short sz, short def)
{
    long r;
    if (sz <= 0 || def <= 0)
        return 0;
    r = (long)sz * 100 / def;
    return (short)(r < 80 ? -2 : r < 95 ? -1 : r < 112 ? 0 : r < 130 ? 1 :
                   r < 155 ? 2 : r < 180 ? 3 : r < 220 ? 4 : 5);
}

/* Bring the output's run face and size in line with the current run.
   Headings keep their style's look. */
static void ApplyRun(Parser *p)
{
    short step;
    if (p->marker && p->tblDepth == 0)
        return;
    step = SizeStep(p->runSz, p->defSz);
    if (p->runFace != p->emFace) {
        Emit(p, (wr_u8)(DOCX_FACE0 + p->runFace));
        p->emFace = p->runFace;
        p->st->chars--;
    }
    if (step != p->emStep) {
        Emit(p, (wr_u8)(DOCX_SIZE0 + step + 2));
        p->emStep = step;
        p->st->chars--;
    }
}

/* A CR: a new paragraph for the viewer, which starts plain again. */
static void EmitCR(Parser *p)
{
    Emit(p, '\r');
    p->emFace = 0;
    p->emStep = 0;
}

/* Called before any visible content of a paragraph. */
static void StartContent(Parser *p)
{
    short i;
    if (p->tblDepth > 0) {
        if (p->pendTab) {
            Emit(p, '\t');
            p->pendTab = 0;
            p->cellSep = 0;
        }
        if (p->cellSep) {
            Emit(p, ' ');
            p->cellSep = 0;
        }
    }
    if (!p->paraStarted) {
        p->paraStarted = 1;
        EmitParaRec(p);
        if (p->marker && p->tblDepth == 0)
            Emit(p, p->marker);
        if (p->isList) {
            if (p->tblDepth > 0)
                for (i = 0; i < p->listLevel && i < 8; i++)
                    EmitStr(p, "   ");
            Emit(p, 0xA5);        /* MacRoman bullet, then a tab to the hanging indent */
            Emit(p, (wr_u8)(p->tblDepth > 0 ? ' ' : '\t'));
        }
    }
    ApplyRun(p);
}

static void EmitCodePoint(Parser *p, wr_u32 cp)
{
    wr_u8 buf[8];
    short n, i;
    long lossyBefore;

    if (p->skipDepth > 0 || !p->inT)
        return;
    if (p->odf) {                       /* ODF: runs of white space are one space */
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            if (p->odf->space)
                return;
            p->odf->space = 1;
            cp = ' ';
        } else
            p->odf->space = 0;
    }
    if (cp < 0x20) {
        if (cp == 0x09)
            cp = '\t';
        else if (cp == 0x0A || cp == 0x0D)
            cp = ' ';
        else
            return;
    }
    if (cp >= 0x0300 && cp <= 0x036F && p->outLen > 0) {
        wr_u8 c = mr_compose(p->out[p->outLen - 1], cp);
        if (c != 0) {
            p->out[p->outLen - 1] = c;
            return;
        }
    }
    if (cp == 0x20AC) {                 /* €: drawn by the viewer */
        p->st->euros++;
        StartContent(p);
        Emit(p, DOCX_CH_EURO);
        return;
    }
    lossyBefore = p->st->lossy;
    n = mr_from_unicode(cp, buf, &p->st->lossy);
    if (n == 0)
        return;
    if (cp >= 0x80 && p->st->lossy == lossyBefore && (n != 1 || buf[0] < 0x80))
        p->st->substituted++;
    StartContent(p);
    for (i = 0; i < n; i++)
        Emit(p, buf[i]);
}

/* ---------- tags ---------- */

/* Local name of the tag into name[]; returns 1 for an end tag. */
static short TagName(const char *tag, char *name, short max)
{
    short i = 0, n = 0, start, closing = 0;
    if (tag[0] == '/') {
        closing = 1;
        i = 1;
    }
    start = i;
    while (tag[i] && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\r' &&
           tag[i] != '\n' && tag[i] != '/' && tag[i] != '>') {
        if (tag[i] == ':')
            start = (short)(i + 1);
        i++;
    }
    for (i = start; tag[i] && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\r' &&
                    tag[i] != '\n' && tag[i] != '/' && n < max - 1; i++)
        name[n++] = tag[i];
    name[n] = 0;
    return closing;
}

static short Eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Value of the attribute whose local name is `attr` (e.g. "val"). */
static short AttrVal(const char *tag, const char *attr, char *val, short max)
{
    short i, j, k, n;
    for (i = 0; tag[i]; i++) {
        if (tag[i] != '=')
            continue;
        /* walk back over the attribute name */
        j = (short)(i - 1);
        while (j >= 0 && tag[j] != ' ' && tag[j] != ':' && tag[j] != '\t' &&
               tag[j] != '\r' && tag[j] != '\n')
            j--;
        for (k = 0; attr[k] && j + 1 + k < i && tag[j + 1 + k] == attr[k]; k++)
            ;
        if (attr[k] == 0 && j + 1 + k == i && (tag[i + 1] == '"' || tag[i + 1] == '\'')) {
            char q = tag[i + 1];
            n = 0;
            for (k = (short)(i + 2); tag[k] && tag[k] != q && n < max - 1; k++)
                val[n++] = tag[k];
            val[n] = 0;
            return 1;
        }
    }
    val[0] = 0;
    return 0;
}

/* Paragraph style id -> marker. Style ids are not localised names but are
   often derived from them: "Heading1", "berschrift1" (Überschrift 1),
   "Titre1", "Kop1", "Titel", "Title". */
static wr_u8 StyleMarker(const char *val)
{
    char s[48];
    short i, n = 0, digit = -1;
    for (i = 0; val[i] && n < 47; i++) {
        char c = val[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            s[n++] = c;
    }
    s[n] = 0;
    if (n > 0 && s[n - 1] >= '1' && s[n - 1] <= '9')
        digit = (short)(s[n - 1] - '0');
    if (Eq(s, "title") || Eq(s, "titel") || Eq(s, "titre") || Eq(s, "titolo") ||
        Eq(s, "ttulo"))
        return DOCX_MK_TITLE;
    if (digit > 0) {
        short isHeading = 0;
        for (i = 0; s[i]; i++) {
            const char *keys[6];
            short k;
            keys[0] = "heading"; keys[1] = "berschrift"; keys[2] = "titre";
            keys[3] = "kop"; keys[4] = "rubrik"; keys[5] = "titolo";
            for (k = 0; k < 6; k++) {
                short m = 0;
                while (keys[k][m] && s[i + m] == keys[k][m])
                    m++;
                if (keys[k][m] == 0)
                    isHeading = 1;
            }
        }
        if (isHeading)
            return (wr_u8)(digit == 1 ? DOCX_MK_H1 : digit == 2 ? DOCX_MK_H2 : DOCX_MK_H3);
    }
    if (Eq(s, "subtitle") || Eq(s, "untertitel"))
        return DOCX_MK_H2;
    return 0;
}

/* Word's built-in list styles carry their numbering in styles.xml, not in the
   paragraph: "ListBullet", "ListBullet2", "ListNumber3", and the German
   "Aufzhlungszeichen" (Aufzählungszeichen) / "Listennummer". */
static void ListStyle(Parser *p, const char *val)
{
    static const char *kKeys[4] = { "listbullet", "listnumber", "aufzhlungszeichen", "listennummer" };
    char s[48];
    short i, k, n = 0;
    for (i = 0; val[i] && n < 47; i++) {
        char c = val[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            s[n++] = c;
    }
    s[n] = 0;
    for (k = 0; k < 4; k++) {
        for (i = 0; kKeys[k][i] && s[i] == kKeys[k][i]; i++)
            ;
        if (kKeys[k][i] == 0) {
            p->isList = 1;
            if (s[i] >= '2' && s[i] <= '9')
                p->listLevel = (short)(s[i] - '1');
            return;
        }
    }
}

static void EndParagraph(Parser *p)
{
    if (p->skipDepth > 0)
        return;
    p->st->paragraphs++;
    if (p->tblDepth > 0) {
        if (p->paraStarted)
            p->cellSep = 1;
    } else {
        EmitParaRec(p);
        EmitCR(p);
    }
    p->inPara = 0;
    p->paraStarted = 0;
}

static long AttrNum(const char *tag, const char *attr, long def)
{
    char v[24];
    long n = 0;
    short i = 0, neg = 0;
    if (!AttrVal(tag, attr, v, sizeof(v)) || v[0] == 0)
        return def;
    if (v[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (v[i] < '0' || v[i] > '9')
        return def;
    for (; v[i] >= '0' && v[i] <= '9'; i++)
        n = n * 10 + (v[i] - '0');
    return neg ? -n : n;
}

/* On/off properties: <w:b/>, <w:b w:val="0"/>, <w:u w:val="none"/> */
static short AttrOn(const char *tag)
{
    char v[16];
    if (!AttrVal(tag, "val", v, sizeof(v)))
        return 1;
    return !(Eq(v, "0") || Eq(v, "false") || Eq(v, "off") || Eq(v, "none"));
}

static short Contains(const char *s, const char *key)
{
    short i, k;
    for (i = 0; s[i]; i++) {
        for (k = 0; key[k]; k++) {
            char c = s[i + k];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            if (c != key[k])
                break;
        }
        if (key[k] == 0)
            return 1;
    }
    return 0;
}

/* Paragraph and run properties, document defaults (styles.xml) and list
   indents (numbering.xml). Returns 1 when the tag was one of them. */
static short HandleFormat(Parser *p, const char *name, short closing, short selfClose)
{
    char v[48];
    long n;
    short bit = 0;

    if (Eq(name, "pPr")) { if (!selfClose) p->inPPr = (short)!closing; return 1; }
    if (Eq(name, "rPr")) { if (!selfClose) p->inRPr = (short)!closing; return 1; }
    if (Eq(name, "docDefaults")) { if (!selfClose) p->inDocDef = (short)!closing; return 1; }
    if (Eq(name, "style")) {
        if (closing)
            p->inDefStyle = 0;
        else if (!selfClose) {
            short para;
            AttrVal(p->tag, "type", v, sizeof(v));
            para = Eq(v, "paragraph");
            AttrVal(p->tag, "default", v, sizeof(v));
            p->inDefStyle = (short)(para && (Eq(v, "1") || Eq(v, "true") || Eq(v, "on")));
        }
        return 1;
    }
    if (Eq(name, "abstractNum")) {
        p->curAbs = (short)(closing || selfClose ? -1 : AttrNum(p->tag, "abstractNumId", -1));
        return 1;
    }
    if (Eq(name, "lvl")) {
        p->curLvl = (short)(closing || selfClose ? -1 : AttrNum(p->tag, "ilvl", -1));
        return 1;
    }
    if (Eq(name, "num")) {
        p->curNum = (short)(closing || selfClose ? -1 : AttrNum(p->tag, "numId", -1));
        return 1;
    }
    if (closing)
        return 0;
    if (Eq(name, "abstractNumId") && p->curNum >= 0) {
        if (p->nNum < 64) {
            p->nums[p->nNum].num = p->curNum;
            p->nums[p->nNum].abs = (short)AttrNum(p->tag, "val", -1);
            p->nNum++;
        }
        return 1;
    }
    if (Eq(name, "spacing")) {
        if (p->inRPr)
            return 1;                       /* character spacing */
        if (p->inPara && p->inPPr) {
            p->before = AttrNum(p->tag, "before", p->before);
            p->after = AttrNum(p->tag, "after", p->after);
        } else if (p->inDocDef || p->inDefStyle) {
            p->defBefore = AttrNum(p->tag, "before", p->defBefore);
            p->defAfter = AttrNum(p->tag, "after", p->defAfter);
        }
        return 1;
    }
    if (Eq(name, "ind")) {
        long left = AttrNum(p->tag, "left", AttrNum(p->tag, "start", -1));
        long hang = AttrNum(p->tag, "hanging", -1), fl = AttrNum(p->tag, "firstLine", -1);
        if (p->inRPr)
            return 1;
        if (p->curAbs >= 0 && p->curLvl >= 0) {
            if (p->nLvl < 64) {
                p->lvls[p->nLvl].abs = p->curAbs;
                p->lvls[p->nLvl].lvl = p->curLvl;
                p->lvls[p->nLvl].left = (short)(left < 0 ? 0 : left > 20000 ? 20000 : left);
                p->lvls[p->nLvl].hang = (short)(hang > 0 && hang < 20000 ? hang : 0);
                p->nLvl++;
            }
        } else if (p->inPara && p->inPPr) {
            if (left >= 0)
                p->left = left;
            if (hang >= 0)
                p->first = -hang;
            else if (fl >= 0)
                p->first = fl;
            p->hasInd = 1;
        }
        return 1;
    }
    if (Eq(name, "jc")) {
        if (p->inPara && p->inPPr && !p->inRPr) {
            AttrVal(p->tag, "val", v, sizeof(v));
            p->align = (short)(Eq(v, "center") ? DOCX_AL_CENTER :
                               (Eq(v, "right") || Eq(v, "end")) ? DOCX_AL_RIGHT :
                               (Eq(v, "both") || Eq(v, "distribute")) ? DOCX_AL_JUSTIFY : DOCX_AL_LEFT);
        }
        return 1;
    }
    if (Eq(name, "numId")) {
        if (p->inPara && p->inPPr)
            p->numId = (short)AttrNum(p->tag, "val", -1);
        p->isList = 1;
        return 1;
    }
    if (Eq(name, "sz")) {
        n = AttrNum(p->tag, "val", 0);
        if (p->inRPr && p->runDepth > 0)
            p->runSz = (short)n;
        else if (p->inRPr && (p->inDocDef || p->inDefStyle) && n > 0)
            p->defSz = (short)n;
        return 1;
    }
    if (!(p->inRPr && p->runDepth > 0))
        return 0;
    if (Eq(name, "b")) bit = 1;
    else if (Eq(name, "i")) bit = 2;
    else if (Eq(name, "u")) bit = 4;
    else if (Eq(name, "rStyle")) {
        AttrVal(p->tag, "val", v, sizeof(v));
        if (Contains(v, "strong") || Contains(v, "fett"))
            p->runFace |= 1;
        if (Contains(v, "emphasis") || Contains(v, "hervorhebung"))
            p->runFace |= 2;
        return 1;
    } else
        return 0;
    if (AttrOn(p->tag))
        p->runFace |= bit;
    else
        p->runFace &= ~bit;
    return 1;
}

/* An ODF length ("0.423cm", "12pt", "0.5in", "2mm") in twips; 0 if none. */
static long OdfLen(const char *v)
{
    long whole = 0, frac = 0, div = 1, num, f;
    short i = 0, neg = 0;
    if (v[0] == '-') { neg = 1; i = 1; }
    for (; v[i] >= '0' && v[i] <= '9'; i++)
        whole = whole * 10 + (v[i] - '0');
    if (v[i] == '.')
        for (i++; v[i] >= '0' && v[i] <= '9'; i++)
            if (div < 10000) { frac = frac * 10 + (v[i] - '0'); div *= 10; }
    num = whole * 10000 + frac * (10000 / div);        /* value x 10000 */
    if (v[i] == 'c' && v[i + 1] == 'm') f = 5670;      /* twips x 10 per unit */
    else if (v[i] == 'm' && v[i + 1] == 'm') f = 567;
    else if (v[i] == 'i' && v[i + 1] == 'n') f = 14400;
    else if (v[i] == 'p' && v[i + 1] == 't') f = 200;
    else if (v[i] == 'p' && v[i + 1] == 'c') f = 2400;
    else if (v[i] == 'p' && v[i + 1] == 'x') f = 150;
    else return 0;
    num = num / 100 * f / 1000;
    return neg ? -num : num;
}

static short OdfFind(OdfCtx *o, const char *name, char family)
{
    short i;
    for (i = 0; i < o->n; i++)
        if (o->s[i].family == family && Eq(o->s[i].name, name))
            return i;
    return -1;
}

static void CopyName(char *d, const char *s)
{
    short i;
    for (i = 0; s[i] && i < 31; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* Apply the style `name` and its ancestors, the root first, so the style
   itself wins. para: also the paragraph's spacing, indents and heading look. */
static void OdfApply(Parser *p, const char *name, char family, short para, short *face, short *sz)
{
    OdfCtx *o = p->odf;
    short chain[8], n = 0, i, k;
    const char *nm = name;
    while (nm[0] && n < 8) {
        k = OdfFind(o, nm, family);
        if (k < 0)
            break;
        chain[n++] = k;
        nm = o->s[k].parent;
    }
    for (i = (short)(n - 1); i >= 0; i--) {
        OdfStyle *s = &o->s[chain[i]];
        if (para) {
            if (s->has & OH_BEFORE) p->before = s->before;
            if (s->has & OH_AFTER)  p->after = s->after;
            if (s->has & OH_LEFT)   p->left = s->left;
            if (s->has & OH_FIRST)  p->first = s->first;
            if (s->has & OH_ALIGN)  p->align = s->align;
            if (s->marker)          p->marker = s->marker;
        }
        *face = (short)((*face & ~s->faceMask) | (s->faceVal & s->faceMask));
        if (s->has & OH_SZ) {
            short base = (short)(*sz > 0 ? *sz : p->defSz);
            *sz = (short)(s->sz >= 0 ? s->sz : (long)base * -s->sz / 100);
        }
    }
}

static void OdfParaProps(Parser *p, OdfStyle *s)
{
    char v[32];
    long n;
    if (AttrVal(p->tag, "margin-top", v, sizeof(v))) {
        n = OdfLen(v);
        if (s) { s->before = n; s->has |= OH_BEFORE; } else p->defBefore = n;
    }
    if (AttrVal(p->tag, "margin-bottom", v, sizeof(v))) {
        n = OdfLen(v);
        if (s) { s->after = n; s->has |= OH_AFTER; } else p->defAfter = n;
    }
    if (!s)
        return;
    if (AttrVal(p->tag, "margin-left", v, sizeof(v)) || AttrVal(p->tag, "margin-start", v, sizeof(v))) {
        s->left = OdfLen(v);
        s->has |= OH_LEFT;
    }
    if (AttrVal(p->tag, "text-indent", v, sizeof(v))) {
        s->first = OdfLen(v);
        s->has |= OH_FIRST;
    }
    if (AttrVal(p->tag, "text-align", v, sizeof(v))) {
        s->align = (short)(Eq(v, "center") ? DOCX_AL_CENTER :
                           (Eq(v, "end") || Eq(v, "right")) ? DOCX_AL_RIGHT :
                           Eq(v, "justify") ? DOCX_AL_JUSTIFY : DOCX_AL_LEFT);
        s->has |= OH_ALIGN;
    }
}

static void OdfTextProps(Parser *p, OdfStyle *s)
{
    char v[32];
    short i, pct;
    if (AttrVal(p->tag, "font-size", v, sizeof(v))) {
        for (i = 0; v[i] && v[i] != '%'; i++)
            ;
        if (v[i] == '%') {
            for (pct = 0, i = 0; v[i] >= '0' && v[i] <= '9'; i++)
                pct = (short)(pct * 10 + (v[i] - '0'));
            if (s && pct > 0) { s->sz = (short)-pct; s->has |= OH_SZ; }
        } else {
            long hp = OdfLen(v) / 10;                    /* twips -> half-points */
            if (hp > 0 && hp < 2000) {
                if (s) { s->sz = (short)hp; s->has |= OH_SZ; } else p->defSz = (short)hp;
            }
        }
    }
    if (!s)
        return;
    if (AttrVal(p->tag, "font-weight", v, sizeof(v))) {
        s->faceMask |= 1;
        if (Eq(v, "bold") || (v[0] >= '6' && v[0] <= '9' && v[1] == '0' && v[2] == '0'))
            s->faceVal |= 1;
        else
            s->faceVal &= ~1;
    }
    if (AttrVal(p->tag, "font-style", v, sizeof(v))) {
        s->faceMask |= 2;
        if (Eq(v, "italic") || Eq(v, "oblique"))
            s->faceVal |= 2;
        else
            s->faceVal &= ~2;
    }
    if (AttrVal(p->tag, "text-underline-style", v, sizeof(v))) {
        s->faceMask |= 4;
        if (!Eq(v, "none"))
            s->faceVal |= 4;
        else
            s->faceVal &= ~4;
    }
}

static void OdfStartPara(Parser *p, short heading)
{
    OdfCtx *o = p->odf;
    char v[48];
    short face = 0, sz = 0;
    p->inPara = 1;
    p->paraStarted = 0;
    p->marker = 0;
    p->isList = 0;
    p->listLevel = 0;
    p->before = p->defBefore;
    p->after = p->defAfter;
    p->left = p->first = 0;
    p->align = DOCX_AL_LEFT;
    p->hasInd = 0;
    p->numId = -1;
    p->paraRec = 0;
    p->inT = 1;
    o->space = 1;                       /* leading white space goes */
    o->spanDepth = 0;
    if (AttrVal(p->tag, "style-name", v, sizeof(v)))
        OdfApply(p, v, 'p', 1, &face, &sz);
    if (heading) {
        long lv = AttrNum(p->tag, "outline-level", 1);
        p->marker = (wr_u8)(lv <= 1 ? DOCX_MK_H1 : lv == 2 ? DOCX_MK_H2 : DOCX_MK_H3);
    }
    if (o->listDepth > 0) {
        p->hasInd = 1;
        p->left = 360L * o->listDepth;  /* LibreOffice's default: 0.635 cm a level */
        p->first = 0;
        if (o->itemFresh) {
            p->isList = 1;
            p->listLevel = (short)(o->listDepth - 1);
            p->first = -360;
            o->itemFresh = 0;
        }
    }
    p->runFace = face;
    p->runSz = sz;
}

static void OdfSkip(Parser *p, short closing, short selfClose)
{
    if (closing)
        p->skipDepth--;
    else if (!selfClose)
        p->skipDepth++;
}

static void OdfTag(Parser *p, const char *name, short closing, short selfClose)
{
    OdfCtx *o = p->odf;
    char v[48], par[48];
    short k;

    /* not shown: notes, comments, deleted text; frames show as [Picture] */
    if (Eq(name, "note-body") || Eq(name, "annotation") || Eq(name, "tracked-changes")) {
        OdfSkip(p, closing, selfClose);
        return;
    }
    if (Eq(name, "frame")) {
        OdfSkip(p, closing, selfClose);
        if (!closing && !selfClose)
            o->frameLevel = p->skipDepth;
        return;
    }
    if (Eq(name, "image")) {
        if (!closing && p->skipDepth == o->frameLevel && p->skipDepth > 0 && p->inT) {
            p->st->pictures++;
            p->skipDepth = 0;            /* briefly: StartContent writes */
            StartContent(p);
            EmitStr(p, "[Picture]");
            p->skipDepth = o->frameLevel;
            o->frameLevel = -1;          /* one marker per frame */
        }
        return;
    }
    if (p->skipDepth > 0)
        return;

    /* styles (styles.xml, then content.xml's automatic styles) */
    if (Eq(name, "style")) {
        if (closing || selfClose) {
            o->cur = -1;
            if (!selfClose)
                return;
        }
        if (closing)
            return;
        AttrVal(p->tag, "name", v, sizeof(v));
        AttrVal(p->tag, "family", par, sizeof(par));
        {
            char fam = (char)(Eq(par, "paragraph") ? 'p' : Eq(par, "text") ? 't' : 0);
            if (fam == 0)
                return;
            k = OdfFind(o, v, fam);       /* content.xml's P1 replaces styles.xml's */
            if (k < 0) {
                if (o->n >= ODF_MAXSTYLES)
                    return;
                k = o->n++;
            }
            {
                OdfStyle *s = &o->s[k];
                short i;
                for (i = 0; i < (short)sizeof(OdfStyle); i++)
                    ((char *)s)[i] = 0;
                CopyName(s->name, v);
                s->family = fam;
                if (AttrVal(p->tag, "parent-style-name", par, sizeof(par)))
                    CopyName(s->parent, par);
                if (fam == 'p')
                    s->marker = StyleMarker(v);
            }
            if (!selfClose)
                o->cur = k;
        }
        return;
    }
    if (Eq(name, "default-style")) {
        if (closing || selfClose)
            o->cur = -1;
        else {
            AttrVal(p->tag, "family", par, sizeof(par));
            o->cur = (short)(Eq(par, "paragraph") ? -2 : -1);
        }
        return;
    }
    if (Eq(name, "paragraph-properties")) {
        if (!closing && o->cur != -1)
            OdfParaProps(p, o->cur >= 0 ? &o->s[o->cur] : 0);
        return;
    }
    if (Eq(name, "text-properties")) {
        if (!closing && o->cur != -1)
            OdfTextProps(p, o->cur >= 0 ? &o->s[o->cur] : 0);
        return;
    }

    /* the body */
    if (Eq(name, "list")) {
        if (closing) { if (o->listDepth > 0) o->listDepth--; }
        else if (!selfClose) o->listDepth++;
        return;
    }
    if (Eq(name, "list-item") || Eq(name, "list-header")) {
        if (!closing)
            o->itemFresh = (short)Eq(name, "list-item");
        return;
    }
    if (Eq(name, "p") || Eq(name, "h")) {
        if (closing) {
            EndParagraph(p);
            p->inT = 0;
            return;
        }
        OdfStartPara(p, (short)(name[0] == 'h'));
        if (selfClose) {
            EndParagraph(p);
            p->inT = 0;
        }
        return;
    }
    if (Eq(name, "span") || Eq(name, "a")) {
        if (closing) {
            if (o->spanDepth > 0) {
                o->spanDepth--;
                if (o->spanDepth < 8) {
                    p->runFace = o->stackFace[o->spanDepth];
                    p->runSz = o->stackSz[o->spanDepth];
                }
            }
        } else if (!selfClose) {
            if (o->spanDepth < 8) {
                o->stackFace[o->spanDepth] = p->runFace;
                o->stackSz[o->spanDepth] = p->runSz;
            }
            o->spanDepth++;
            if (AttrVal(p->tag, "style-name", v, sizeof(v)))
                OdfApply(p, v, 't', 0, &p->runFace, &p->runSz);
        }
        return;
    }
    if (!p->inT || closing)
        ;
    else if (Eq(name, "tab")) {
        StartContent(p);
        Emit(p, '\t');
        o->space = 0;
        return;
    } else if (Eq(name, "line-break")) {
        StartContent(p);
        Emit(p, (wr_u8)(p->tblDepth > 0 ? ' ' : DOCX_CH_LINEBREAK));
        o->space = 1;
        return;
    } else if (Eq(name, "s")) {
        long c = AttrNum(p->tag, "c", 1);
        StartContent(p);
        while (c-- > 0 && c < 200)
            Emit(p, ' ');
        o->space = 1;
        return;
    }
    if (Eq(name, "table")) {
        if (closing) { if (p->tblDepth > 0) p->tblDepth--; }
        else if (!selfClose) {
            p->tblDepth++;
            p->st->tables++;
            p->pendTab = 0;
            p->cellSep = 0;
        }
    } else if (Eq(name, "table-row")) {
        p->pendTab = 0;
        p->cellSep = 0;
        if (closing)
            EmitCR(p);
    } else if (Eq(name, "table-cell") || Eq(name, "covered-table-cell")) {
        if (closing || selfClose) {
            p->pendTab = 1;
            p->cellSep = 0;
        }
    }
}

static void HandleTag(Parser *p)
{
    char name[24], val[48];
    short closing, selfClose;

    p->tag[p->tagLen] = 0;
    if (p->tagLen == 0 || p->tag[0] == '?' || p->tag[0] == '!')
        return;
    selfClose = (short)(p->tag[p->tagLen - 1] == '/');
    closing = TagName(p->tag, name, sizeof(name));
    if (p->odf) {
        OdfTag(p, name, closing, selfClose);
        return;
    }

    if (Eq(name, "Fallback")) {
        if (closing)
            p->skipDepth--;
        else if (!selfClose)
            p->skipDepth++;
        return;
    }
    if (p->skipDepth > 0)
        return;

    if (closing && HandleFormat(p, name, closing, selfClose))
        return;
    if (Eq(name, "p")) {
        if (closing) {
            EndParagraph(p);
        } else {
            p->inPara = 1;
            p->paraStarted = 0;
            p->marker = 0;
            p->isList = 0;
            p->listLevel = 0;
            p->before = p->defBefore;
            p->after = p->defAfter;
            p->left = p->first = 0;
            p->align = DOCX_AL_LEFT;
            p->hasInd = 0;
            p->numId = -1;
            p->paraRec = 0;
            p->inPPr = p->inRPr = 0;
            if (selfClose)
                EndParagraph(p);
        }
    } else if (Eq(name, "t")) {
        p->inT = (short)(!closing && !selfClose);
    } else if (Eq(name, "r")) {
        if (closing)
            p->runDepth--;
        else if (!selfClose) {
            p->runDepth++;
            p->runFace = 0;
            p->runSz = 0;
            p->inRPr = 0;
        }
    } else if (closing) {
        if (Eq(name, "tc")) {
            p->pendTab = 1;
            p->cellSep = 0;
        } else if (Eq(name, "tr")) {
            p->pendTab = 0;
            p->cellSep = 0;
            EmitCR(p);
        } else if (Eq(name, "tbl")) {
            if (p->tblDepth > 0)
                p->tblDepth--;
        }
    } else if (Eq(name, "pStyle")) {
        if (AttrVal(p->tag, "val", val, sizeof(val))) {
            p->marker = StyleMarker(val);
            ListStyle(p, val);
        }
    } else if (HandleFormat(p, name, closing, selfClose)) {
        ;
    } else if (Eq(name, "numPr")) {
        p->isList = 1;
    } else if (Eq(name, "ilvl")) {
        if (AttrVal(p->tag, "val", val, sizeof(val)) && val[0] >= '0' && val[0] <= '9')
            p->listLevel = (short)(val[0] - '0');
    } else if (Eq(name, "tab") || Eq(name, "ptab")) {
        if (p->runDepth > 0) {
            StartContent(p);
            Emit(p, '\t');
        }
    } else if (Eq(name, "br") || Eq(name, "cr")) {
        if (p->runDepth > 0) {
            StartContent(p);
            Emit(p, (wr_u8)(p->tblDepth > 0 ? ' ' : DOCX_CH_LINEBREAK));
        }
    } else if (Eq(name, "noBreakHyphen")) {
        StartContent(p);
        Emit(p, '-');
    } else if (Eq(name, "sym")) {
        if (AttrVal(p->tag, "char", val, sizeof(val))) {
            StartContent(p);
            if (Eq(val, "F0B7") || Eq(val, "F0A7") || Eq(val, "F06C") || Eq(val, "F0A8"))
                Emit(p, 0xA5);
            else {
                Emit(p, '?');
                p->st->lossy++;
            }
        }
    } else if (Eq(name, "pic") || Eq(name, "imagedata")) {
        p->st->pictures++;
        StartContent(p);
        EmitStr(p, "[Picture]");
    } else if (Eq(name, "tbl")) {
        p->tblDepth++;
        p->st->tables++;
        p->pendTab = 0;
        p->cellSep = 0;
    } else if (Eq(name, "tr")) {
        p->pendTab = 0;
        p->cellSep = 0;
    }
}

/* ---------- text and entities ---------- */

static void TextByte(Parser *p, wr_u8 c)
{
    if (p->need > 0) {
        if ((c & 0xC0) == 0x80) {
            p->cp = (p->cp << 6) | (c & 0x3F);
            if (--p->need == 0)
                EmitCodePoint(p, p->cp);
            return;
        }
        p->need = 0;                 /* broken sequence */
        if (p->inT) {
            p->st->lossy++;
            EmitCodePoint(p, '?');
        }
    }
    if (c < 0x80)
        EmitCodePoint(p, c);
    else if ((c & 0xE0) == 0xC0) { p->cp = c & 0x1F; p->need = 1; }
    else if ((c & 0xF0) == 0xE0) { p->cp = c & 0x0F; p->need = 2; }
    else if ((c & 0xF8) == 0xF0) { p->cp = c & 0x07; p->need = 3; }
    else if (p->inT) {
        p->st->lossy++;
        EmitCodePoint(p, '?');
    }
}

static void EndEntity(Parser *p)
{
    wr_u32 cp = 0;
    short i;
    p->ent[p->entLen] = 0;
    if (Eq(p->ent, "amp")) cp = '&';
    else if (Eq(p->ent, "lt")) cp = '<';
    else if (Eq(p->ent, "gt")) cp = '>';
    else if (Eq(p->ent, "quot")) cp = '"';
    else if (Eq(p->ent, "apos")) cp = '\'';
    else if (p->ent[0] == '#' && (p->ent[1] == 'x' || p->ent[1] == 'X')) {
        for (i = 2; p->ent[i]; i++) {
            char c = p->ent[i];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (wr_u32)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (wr_u32)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (wr_u32)(c - 'A' + 10);
        }
    } else if (p->ent[0] == '#') {
        for (i = 1; p->ent[i] >= '0' && p->ent[i] <= '9'; i++)
            cp = cp * 10 + (wr_u32)(p->ent[i] - '0');
    } else {
        EmitCodePoint(p, '&');
        for (i = 0; p->ent[i]; i++)
            EmitCodePoint(p, (wr_u8)p->ent[i]);
        EmitCodePoint(p, ';');
        return;
    }
    EmitCodePoint(p, cp);
}

/* ---------- the byte pump ---------- */

static long XmlFeed(void *ctx, const wr_u8 *buf, long n)
{
    Parser *p = (Parser *)ctx;
    long i;
    p->st->xmlBytes += (wr_u32)n;
    for (i = 0; i < n && !p->stopped; i++) {
        wr_u8 c = buf[i];
        switch (p->mode) {
        case M_TEXT:
            if (c == '<') {
                p->mode = M_TAG;
                p->tagLen = 0;
                p->quote = 0;
                p->inEnt = 0;
            } else if (p->inEnt) {
                if (c == ';') {
                    p->inEnt = 0;
                    EndEntity(p);
                } else if (p->entLen < 10) {
                    p->ent[p->entLen++] = (char)c;
                }
            } else if (c == '&') {
                p->inEnt = 1;
                p->entLen = 0;
            } else if (p->inT) {
                TextByte(p, c);
            }
            break;
        case M_TAG:
            if (p->quote) {
                if (c == (wr_u8)p->quote)
                    p->quote = 0;
            } else if (c == '"' || c == '\'') {
                p->quote = (char)c;
            } else if (c == '>') {
                p->mode = M_TEXT;
                HandleTag(p);
                break;
            }
            if (p->tagLen < TAGMAX)
                p->tag[p->tagLen++] = (char)c;
            if (p->tagLen == 3 && p->tag[0] == '!' && p->tag[1] == '-' && p->tag[2] == '-') {
                p->mode = M_COMMENT;
                p->tail[0] = p->tail[1] = 0;
            } else if (p->tagLen == 8 && p->tag[0] == '!' && p->tag[1] == '[' &&
                       p->tag[2] == 'C') {
                p->mode = M_CDATA;
                p->tail[0] = p->tail[1] = 0;
            }
            break;
        case M_COMMENT:
        case M_CDATA:
            if (c == '>' && p->tail[0] == (p->mode == M_COMMENT ? '-' : ']') &&
                p->tail[1] == p->tail[0]) {
                p->mode = M_TEXT;
            } else if (p->mode == M_CDATA && p->inT && c != ']') {
                if (p->tail[1] == ']')       /* a lone ']' was text after all */
                    TextByte(p, ']');
                TextByte(p, c);
            }
            p->tail[0] = p->tail[1];
            p->tail[1] = (char)c;
            break;
        }
    }
    return p->stopped;
}

/* ---------- package handling ---------- */

typedef struct {
    char buf[8192];
    long len;
} MemSink;

static long MemWrite(void *ctx, const wr_u8 *b, long n)
{
    MemSink *m = (MemSink *)ctx;
    long room = (long)sizeof(m->buf) - 1 - m->len;
    if (n > room)
        n = room;
    WR_MEMCPY(m->buf + m->len, b, n);
    m->len += n;
    m->buf[m->len] = 0;
    return 0;                         /* just keep the start */
}

/* Target of the officeDocument relationship in _rels/.rels, or "". */
static void MainPartFromRels(const char *rels, char *part, short max)
{
    const char *r = rels;
    part[0] = 0;
    while (*r) {
        const char *open = r, *end, *q;
        short isMain = 0, n = 0;
        char tag[600];
        while (*open && !(open[0] == '<' && open[1] == 'R'))
            open++;
        if (!*open)
            return;
        end = open;
        while (*end && *end != '>')
            end++;
        if (!*end)
            return;
        while (open < end && n < (short)sizeof(tag) - 1)
            tag[n++] = *open++;
        tag[n] = 0;
        r = end + 1;
        /* Type ends in "/officeDocument" (Transitional and Strict) */
        for (q = tag; *q; q++) {
            const char *key = "/officeDocument";
            short k = 0;
            while (key[k] && q[k] == key[k])
                k++;
            if (key[k] == 0 && (q[k] == '"' || q[k] == '\''))
                isMain = 1;
        }
        if (isMain && AttrVal(tag, "Target", part, max)) {
            if (part[0] == '/') {
                short i;
                for (i = 0; part[i]; i++)
                    part[i] = part[i + 1];
            }
            return;
        }
    }
}

static void InitParser(Parser *p, WrWriteFn wr, void *wctx, DocxStats *st)
{
    p->wr = wr;
    p->wctx = wctx;
    p->st = st;
    p->mode = M_TEXT;
    p->defSz = 22;                  /* 11 pt, Word's default */
    p->curAbs = p->curLvl = p->curNum = -1;
}

static short Finish(Parser *p, short err)
{
    Flush(p, 0);
    if (err == ZIP_ERR_STOPPED || p->stopped)
        return ZIP_ERR_STOPPED;
    return err;
}

/* OpenDocument text: styles.xml (named styles, defaults), then content.xml
   (its automatic styles, then the body), through the same parser. */
static short ReadOdf(WrSource *src, Parser *p, DocxStats *st)
{
    ZipEntry e;
    short err, i;
    const char *part = "content.xml";
    st->format = DOCX_FMT_ODT;
    for (i = 0; part[i]; i++)
        st->part[i] = part[i];
    st->part[i] = 0;
    p->odf = (OdfCtx *)WR_ALLOC(sizeof(OdfCtx));
    if (p->odf == 0)
        return ZIP_ERR_MEM;
    p->odf->cur = p->odf->frameLevel = -1;
    p->defSz = 24;                      /* 12 pt, LibreOffice's default */
    if (zip_find(src, "styles.xml", &e) == ZIP_OK) {
        zip_extract(src, &e, XmlFeed, p);
        p->mode = M_TEXT;
        p->inT = 0;
        p->skipDepth = 0;
        p->odf->cur = -1;
    }
    st->xmlBytes = 0;
    err = zip_find(src, part, &e);
    if (err == ZIP_OK)
        err = zip_extract(src, &e, XmlFeed, p);
    else
        err = DOCX_ERR_FORMAT;
    err = Finish(p, err);
    WR_FREE(p->odf);
    p->odf = 0;
    return err;
}

short docx_convert(WrSource *src, WrWriteFn wr, void *wctx, DocxStats *st)
{
    wr_u8 head[8];
    Parser *p;
    short err;
    long n, i;

    for (n = 0; n < (long)sizeof(DocxStats); n++)
        ((char *)st)[n] = 0;
    n = src->size < 8 ? (long)src->size : 8;
    if (n < 4 || src->readAt(src->ctx, 0, head, n) != n)
        return DOCX_ERR_FORMAT;

    if (head[0] == 0xD0 && head[1] == 0xCF && head[2] == 0x11 && head[3] == 0xE0)
        return DOCX_ERR_OLE;
    if (head[0] == '{' && head[1] == '\\' && head[2] == 'r' && head[3] == 't')
        return DOCX_ERR_RTF;
    if ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF))
        return DOCX_ERR_UTF16;

    p = (Parser *)WR_ALLOC(sizeof(Parser));
    if (p == 0)
        return ZIP_ERR_MEM;
    InitParser(p, wr, wctx, st);

    if (head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) {
        ZipEntry e;
        MemSink *rels = (MemSink *)WR_ALLOC(sizeof(MemSink));
        st->format = DOCX_FMT_OOXML;
        if (rels == 0) {
            WR_FREE(p);
            return ZIP_ERR_MEM;
        }
        /* OpenDocument: the first member, "mimetype", says what it is */
        if (zip_find(src, "mimetype", &e) == ZIP_OK && e.size < 200 &&
            zip_extract(src, &e, MemWrite, rels) == ZIP_OK &&
            Contains(rels->buf, "opendocument")) {
            short text = Contains(rels->buf, "opendocument.text");
            WR_FREE(rels);
            if (!text) {                     /* .ods, .odp, ... */
                WR_FREE(p);
                return DOCX_ERR_NOTWORD;
            }
            err = ReadOdf(src, p, st);
            WR_FREE(p);
            return err;
        }
        rels->len = 0;
        rels->buf[0] = 0;
        if (zip_find(src, "_rels/.rels", &e) == ZIP_OK &&
            zip_extract(src, &e, MemWrite, rels) == ZIP_OK)
            MainPartFromRels(rels->buf, st->part, sizeof(st->part));
        WR_FREE(rels);
        if (st->part[0] == 0) {
            const char *def = "word/document.xml";
            for (i = 0; def[i]; i++)
                st->part[i] = def[i];
            st->part[i] = 0;
        }
        if (!(st->part[0] == 'w' && st->part[1] == 'o' && st->part[2] == 'r' &&
              st->part[3] == 'd' && st->part[4] == '/')) {
            /* xl/workbook.xml, ppt/presentation.xml, ... */
            err = (zip_find(src, "word/document.xml", &e) == ZIP_OK) ? ZIP_OK : DOCX_ERR_NOTWORD;
            if (err == ZIP_OK) {
                const char *def = "word/document.xml";
                for (i = 0; def[i]; i++)
                    st->part[i] = def[i];
                st->part[i] = 0;
            }
        } else {
            err = zip_find(src, st->part, &e);
        }
        if (err == ZIP_OK) {
            /* defaults and list indents first: same parser, no text in them */
            ZipEntry aux;
            static const char *kAux[2] = { "word/styles.xml", "word/numbering.xml" };
            short k;
            for (k = 0; k < 2 && !p->stopped; k++)
                if (zip_find(src, kAux[k], &aux) == ZIP_OK) {
                    zip_extract(src, &aux, XmlFeed, p);
                    p->mode = M_TEXT;
                    p->inT = 0;
                    p->inDocDef = p->inDefStyle = p->inPPr = p->inRPr = 0;
                }
            st->xmlBytes = 0;
            err = zip_extract(src, &e, XmlFeed, p);
        }
        err = Finish(p, err);
        WR_FREE(p);
        return err;
    }

    /* a single XML file: skip a UTF-8 byte order mark and whitespace */
    i = 0;
    if (head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
        i = 3;
    while (i < n && (head[i] == ' ' || head[i] == '\r' || head[i] == '\n' || head[i] == '\t'))
        i++;
    if (i < n && head[i] == '<') {
        wr_u8 buf[1024];
        wr_u32 off = (wr_u32)i;
        st->format = DOCX_FMT_FLATXML;
        err = DOCX_OK;
        {   /* flat OpenDocument (.fodt)? its namespace shows up early */
            n = src->readAt(src->ctx, off, buf, (long)(src->size - off < sizeof(buf) - 1 ? src->size - off : sizeof(buf) - 1));
            if (n > 0) {
                buf[n] = 0;
                for (i = 0; i < n; i++)
                    if (buf[i] == 0) buf[i] = ' ';
                if (Contains((char *)buf, "urn:oasis:names:tc:opendocument")) {
                    p->odf = (OdfCtx *)WR_ALLOC(sizeof(OdfCtx));
                    if (p->odf == 0) {
                        WR_FREE(p);
                        return ZIP_ERR_MEM;
                    }
                    p->odf->cur = p->odf->frameLevel = -1;
                    st->format = DOCX_FMT_FLATODT;
                }
            }
        }
        while (off < src->size && err == DOCX_OK && !p->stopped) {
            long want = (long)(src->size - off);
            if (want > (long)sizeof(buf))
                want = sizeof(buf);
            n = src->readAt(src->ctx, off, buf, want);
            if (n <= 0)
                err = ZIP_ERR_READ;
            else {
                XmlFeed(p, buf, n);
                off += (wr_u32)n;
            }
        }
        if (err == DOCX_OK && st->paragraphs == 0)
            err = DOCX_ERR_FORMAT;     /* XML, but no WordprocessingML in it */
        err = Finish(p, err);
        if (p->odf)
            WR_FREE(p->odf);
        WR_FREE(p);
        return err;
    }

    WR_FREE(p);
    return DOCX_ERR_FORMAT;
}

long docx_ctrl_len(const wr_u8 *t, long i, long len)
{
    wr_u8 c = t[i];
    if (DOCX_IS_MARKER(c) || DOCX_IS_FACE(c) || DOCX_IS_SIZE(c))
        return 1;
    if (c == DOCX_PARA)
        return (i + DOCX_PARA_LEN <= len) ? DOCX_PARA_LEN : len - i;
    return 0;
}
