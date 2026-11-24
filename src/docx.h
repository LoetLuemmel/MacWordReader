/*
 * docx.h - Word document (Office Open XML) -> MacRoman text.
 *
 * Reads .docx / .docm / .dotx / .dotm (a ZIP package; the main part is found
 * through _rels/.rels, so Strict OOXML and renamed parts work too) and the
 * single-file XML flavours (Word 2003 XML, Flat OPC .xml).
 *
 * Output is MacRoman with CR line ends, ready for QuickDraw and TeachText.
 * A paragraph may start with one marker byte giving its style - see DOCX_MK_*.
 * The viewer draws those as headings; Save as Text drops them.
 */
#ifndef DOCX_H
#define DOCX_H

#include "wrplat.h"

/* paragraph style markers (never otherwise emitted: control bytes) */
#define DOCX_MK_TITLE 0x01
#define DOCX_MK_H1    0x02
#define DOCX_MK_H2    0x03
#define DOCX_MK_H3    0x04
#define DOCX_IS_MARKER(c) ((c) >= 0x01 && (c) <= 0x04)

/* The euro sign, inside the text. Mac OS Roman under System 7 has no €
   ($DB is still the currency sign ¤ - it became € only with Mac OS 8.5),
   so the converter emits this control byte and the viewer draws the glyph
   itself. Anything that leaves the program spells it "EUR". */
#define DOCX_CH_EURO  0x05

/* Formatting, carried in the text as control bytes (never CR, never >= 0x20
   on their own), the same for every input format:

   DOCX_PARA + 5 bytes   at a paragraph's start, each byte 0x20 + value:
                         space before, space after (points), left indent,
                         first-line indent + 100 (both in 2-point units;
                         negative = hanging), alignment (DOCX_AL_*)
   0x10 + face           run face from here on: bold 1, italic 2, underline 4
   0x18 + step + 2       run size from here on, in steps relative to body
                         text (-2 .. +5); the viewer maps steps to real sizes
   DOCX_CH_LINEBREAK     a line break inside a paragraph (keeps its indent)

   DOCX_PIC + 5 bytes + name
                         a picture, alone on its line in the viewer: width
                         and height in points (two bytes each, 0x20 + six
                         bits, high first; 0 = unknown), the name's length
                         (0x20 + n), then the package member holding it
                         (printable ASCII, e.g. word/media/image1.png)

   Run face and size start at plain body text after every CR. */
#define DOCX_PARA         0x06
#define DOCX_PARA_LEN     6
#define DOCX_FACE0        0x10
#define DOCX_SIZE0        0x18
#define DOCX_CH_LINEBREAK 0x0B
#define DOCX_PIC          0x07
#define DOCX_PIC_HEAD     6
#define DOCX_PIC_NAMEMAX  90
#define DOCX_IS_FACE(c)   ((c) >= 0x10 && (c) <= 0x17)
#define DOCX_IS_SIZE(c)   ((c) >= 0x18 && (c) <= 0x1F)
enum { DOCX_AL_LEFT = 0, DOCX_AL_CENTER, DOCX_AL_RIGHT, DOCX_AL_JUSTIFY };

enum {
    DOCX_FMT_UNKNOWN = 0,
    DOCX_FMT_OOXML,        /* .docx family, zipped */
    DOCX_FMT_FLATXML,      /* Word 2003 XML / Flat OPC */
    DOCX_FMT_ODT,          /* OpenDocument text (.odt/.ott), zipped */
    DOCX_FMT_FLATODT       /* OpenDocument flat XML (.fodt) */
};

typedef struct {
    short  format;         /* DOCX_FMT_* */
    char   part[64];       /* main part inside the package */
    wr_u32 xmlBytes;       /* uncompressed XML seen */
    long   chars;          /* MacRoman bytes produced */
    long   paragraphs;
    long   tables;
    long   pictures;
    long   lossy;          /* characters shown as '?' */
    long   substituted;    /* characters spelt with a stand-in (1/2, L for Ł) */
    long   euros;          /* € signs (DOCX_CH_EURO) */
} DocxStats;

enum {
    DOCX_OK = 0,
    DOCX_ERR_FORMAT = 20,  /* not a Word document we know */
    DOCX_ERR_OLE = 21,     /* binary Office file: old .doc, or an encrypted .docx */
    DOCX_ERR_RTF = 22,
    DOCX_ERR_NOTWORD = 23, /* an OOXML package, but Excel / PowerPoint */
    DOCX_ERR_UTF16 = 24
    /* plus the ZIP_ERR_* codes from zipdir.h */
};

/* Length of the control item starting at t[i] (0: text). */
long docx_ctrl_len(const wr_u8 *t, long i, long len);

/* The picture item at t[i] (DOCX_PIC): size in points, member name as a
   C string (name must hold DOCX_PIC_NAMEMAX + 1). Returns 0 if malformed. */
short docx_pic_info(const wr_u8 *t, long i, long len, short *wPt, short *hPt, char *name);

short docx_convert(WrSource *src, WrWriteFn wr, void *wctx, DocxStats *st);

#endif
