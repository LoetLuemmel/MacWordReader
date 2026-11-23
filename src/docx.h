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

enum {
    DOCX_FMT_UNKNOWN = 0,
    DOCX_FMT_OOXML,        /* .docx family, zipped */
    DOCX_FMT_FLATXML       /* Word 2003 XML / Flat OPC */
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

short docx_convert(WrSource *src, WrWriteFn wr, void *wctx, DocxStats *st);

#endif
