/*
 * macroman.h - Unicode -> Mac OS Roman, the way System 7 draws it.
 *
 * Word writes Unicode (UTF-8 in the XML); a 68k Mac draws 8-bit Mac OS Roman.
 * The two share only ASCII. Every other character either has a MacRoman slot
 * (ä, é, ß, the curly quotes, en/em dash, bullet, ...), has a readable
 * stand-in (Ł -> L, ½ -> 1/2, € -> EUR, × -> x), or cannot be shown and
 * becomes '?' and is counted as lossy.
 *
 * System 7 note: byte 0xDB is the general currency sign ¤. Apple re-assigned
 * it to the euro sign only with Mac OS 8.5, so on the Duo € is spelt "EUR".
 */
#ifndef MACROMAN_H
#define MACROMAN_H

#include "wrplat.h"

/* Converts one code point. Writes 0..6 bytes to out and returns the count.
   *lossy is incremented when the character had to be replaced by '?'. */
short mr_from_unicode(wr_u32 cp, wr_u8 *out, long *lossy);

/* A combining mark (U+0300..U+036F) after a base letter: macOS often writes
   "a" + U+0308 instead of "ä". Returns the MacRoman byte for the composed
   letter, or 0 when MacRoman has no such letter. */
wr_u8 mr_compose(wr_u8 base, wr_u32 mark);

/* MacRoman byte -> Unicode, for the host test harness and Save as Text. */
wr_u32 mr_to_unicode(wr_u8 b);

#endif
