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

#define TAGMAX 200

enum { M_TEXT, M_TAG, M_COMMENT, M_CDATA };

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
} Parser;

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
    if (p->paraStarted)
        return;
    p->paraStarted = 1;
    if (p->marker && p->tblDepth == 0)
        Emit(p, p->marker);
    if (p->isList) {
        for (i = 0; i < p->listLevel && i < 8; i++)
            EmitStr(p, "   ");
        Emit(p, 0xA5);        /* MacRoman bullet */
        Emit(p, ' ');
    }
}

static void EmitCodePoint(Parser *p, wr_u32 cp)
{
    wr_u8 buf[8];
    short n, i;
    long lossyBefore;

    if (p->skipDepth > 0 || !p->inT)
        return;
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
        Emit(p, '\r');
    }
    p->inPara = 0;
    p->paraStarted = 0;
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

    if (Eq(name, "Fallback")) {
        if (closing)
            p->skipDepth--;
        else if (!selfClose)
            p->skipDepth++;
        return;
    }
    if (p->skipDepth > 0)
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
            if (selfClose)
                EndParagraph(p);
        }
    } else if (Eq(name, "t")) {
        p->inT = (short)(!closing && !selfClose);
    } else if (Eq(name, "r")) {
        if (closing)
            p->runDepth--;
        else if (!selfClose)
            p->runDepth++;
    } else if (closing) {
        if (Eq(name, "tc")) {
            p->pendTab = 1;
            p->cellSep = 0;
        } else if (Eq(name, "tr")) {
            p->pendTab = 0;
            p->cellSep = 0;
            Emit(p, '\r');
        } else if (Eq(name, "tbl")) {
            if (p->tblDepth > 0)
                p->tblDepth--;
        }
    } else if (Eq(name, "pStyle")) {
        if (AttrVal(p->tag, "val", val, sizeof(val))) {
            p->marker = StyleMarker(val);
            ListStyle(p, val);
        }
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
            Emit(p, (wr_u8)(p->tblDepth > 0 ? ' ' : '\r'));
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
}

static short Finish(Parser *p, short err)
{
    Flush(p, 0);
    if (err == ZIP_ERR_STOPPED || p->stopped)
        return ZIP_ERR_STOPPED;
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
        if (err == ZIP_OK)
            err = zip_extract(src, &e, XmlFeed, p);
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
        WR_FREE(p);
        return err;
    }

    WR_FREE(p);
    return DOCX_ERR_FORMAT;
}
