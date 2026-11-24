# WordReader — Word (.docx) and OpenDocument (.odt), with pictures, on a 68k Macintosh

A System 7 application that opens today's Microsoft Word documents on a
PowerBook Duo 230 (68030, 33 MHz, 8 MB). Built with MPW (SC 8.9, Universal
Interfaces 3.4) on the dev2019 guest, deployed to the Duo over AppleBridge.

## What it reads

| Format | How it is recognised |
|---|---|
| `.docx` `.docm` `.dotx` `.dotm` (Office Open XML, Transitional and Strict) | `PK\3\4`; main part found via `_rels/.rels` |
| Word 2003 XML, Flat OPC `.xml` | starts with `<` (UTF-8, optional BOM) |
| old binary `.doc`, password-protected `.docx` (OLE) | `D0 CF 11 E0` → a clear message, not garbage |
| `.odt` `.ott` (OpenDocument text, ISO/IEC 26300) | ZIP whose `mimetype` says `opendocument.text`; `styles.xml` then `content.xml` |
| `.fodt` (flat OpenDocument) | XML with the OpenDocument namespace |
| `.xlsx` `.pptx` `.ods` `.odp` | an office package without a text part → "not a text document" |

Shown: paragraphs with their spacing before and after, left and hanging
indents, centred and right-aligned text; **bold**, *italic*, underline and
text size per run (snapped to Geneva's bitmap sizes, so it stays crisp);
headings (Title, Heading 1–3, German *Überschrift*, ODF `text:h`); bullets
with hanging indents (list indents from `numbering.xml`); tables (cells
tab-separated, rows per line); tabs and line breaks; **pictures** (PNG and
JPEG, in 16 greys — see below; other formats as a labelled box). Footnote
bodies and comments are left out. File ▸ Save as Text writes
a TeachText file (MacRoman, CR). Text can be selected (drag, Shift-click,
double-click for words, ⌘A) and copied (⌘C) to the clipboard as plain MacRoman
text. File ▸ Document Info reports what the conversion had to do.

### One formatting model for both formats

Both readers write the same stream: MacRoman text plus a few control bytes
(`src/docx.h`). A paragraph starts with a 6-byte record (space before/after,
left indent, first-line indent, alignment); `0x10+face` and `0x18+size step`
switch the run's look; `0x0B` is a line break inside a paragraph. The viewer
lays that out; Copy and Save as Text drop it.

| | Word (OOXML) | OpenDocument |
|---|---|---|
| paragraph | `w:pPr`: `w:spacing`, `w:ind`, `w:jc`; defaults from `styles.xml` `w:docDefaults` | `style:paragraph-properties` `fo:margin-*`, `fo:text-indent`, `fo:text-align`, through `parent-style-name` |
| run | `w:rPr`: `w:b`, `w:i`, `w:u`, `w:sz` | `text:span` → `style:text-properties` `fo:font-weight`, `fo:font-style`, `fo:font-size` (pt or %) |
| list | `w:numPr` → `numbering.xml` indents | nested `text:list` |

### Pictures

The converter writes a picture item into the stream (`DOCX_PIC`: size in
points and the package member, found through `word/_rels/document.xml.rels`
for `a:blip r:embed` / `v:imagedata r:id`, or `draw:image xlink:href` in
ODF). The viewer gives each picture a line of its own, shrunk to fit the
column and the window, and decodes it the first time it scrolls into view:

- `src/image.c` decodes **PNG** (all colour types and bit depths, palette and
  `tRNS` transparency over white, Adam7 up to 200,000 pixels) and **baseline
  JPEG** (4:4:4, 4:2:2, 4:2:0, greyscale, restart markers). A progressive JPEG
  is shown from its first scan, at 1/8 resolution.
- Rows stream through a box-filter scaler, so a picture never exists at full
  size. Only luminance is needed for grey: JPEG colour blocks are entropy-decoded
  and dropped, and big photos are reduced inside the transform (1/8 from the DC
  value alone, 1/4 and 1/2 with a 2×2 / 4×4 inverse DCT; a reduction is taken
  while it still gives 7/8 of the shown width, and the picture is enlarged
  the rest of the way).
- A stored member (LibreOffice) is read in place; a deflated one (Word) is
  inflated to a file in Temporary Items first, since the decoders read at will.
- The result is ordered-dithered to 16 greys (1 bit without Color QuickDraw)
  and kept in a purgeable handle: when memory runs short the Memory Manager
  takes it back and the picture is decoded again when next drawn.
- Copy and Save as Text write `[Picture]`. Document Info shows how long the
  pictures took.

Speed on the Duo 230 (`samples/Pictures-Demo.docx`): the 1600 × 1000 photo
shown 432 px wide takes about 15 s, all four pictures about 30 s, the text
1.5 s. The 68000 code that SC generates has no 32-bit multiply or divide, so
the hot loops stay with 16-bit multiplies (the 4-point IDCT is a butterfly
whose 1/(2√2) scale per pass becomes a shift), lookup tables (Huffman codes
with their value, dither levels, reciprocals in the scaler) and locals for
the bit buffers. Unused `w:style` entries in `styles.xml` are skipped, and
reading stops after the default paragraph style: python-docx writes 340 KB of
table styles there.

Checked on the host against Pillow/libjpeg: PNG decodes byte-exact; JPEG at full
size byte-exact, reduced sizes within one grey level of the block average;
fuzzed under ASan/UBSan.

## The code-table problem, three layers deep

1. **Encoding** — Word writes UTF-8 (Unicode); the Mac draws Mac OS Roman.
   `src/macroman.c` maps every Unicode character that MacRoman has (ä ß é «
   „ – — … • ° ± ≤ ≠ π Ω …) to its byte, spells out what it lacks
   (½ → `1/2`, ² → `2`, × → `x`, Ł → `L`, Š → `S`) and shows the
   rest (Greek, Cyrillic, CJK, emoji) as `?`, counted in Document Info.
   System 7 detail: byte `$DB` is still ¤ — Apple made it € only in Mac OS 8.5,
   so € is never mapped to `$DB` here. In a document, € becomes the internal byte
   `$05`. The view draws it with QuickDraw (an arc and two bars, as wide as a
   digit); Save as Text writes `EUR`.
2. **Normalisation** — documents written on macOS may contain decomposed
   accents (`e` + U+0301). They are recomposed to the MacRoman letter (é).
3. **Fonts** — some MacRoman bytes exist in the code table but not in the
   bitmap font: Geneva has no glyph for „ ‚ ‰ ﬁ ﬂ. WordReader asks the
   Font Manager (`FMSwapFont`) for the strike in use, reads its offset/width
   table (−1 = no glyph) and draws a stand-in (`,,` for „). Save as Text keeps
   the real byte.

Line ends: Word paragraphs become CR, the Mac convention.

## Memory

The archive is never unpacked as a whole: `src/inflate.c` streams DEFLATE
through a 32 KB window straight into the XML state machine (`src/docx.c`),
so document.xml can be any size. Only the resulting MacRoman text is kept.
The view draws its own wrapped lines (no TextEdit 32 KB limit).
Font metrics are built per face and size when first needed (~1 KB each).
Pictures: a few source rows while decoding (plus the 32 KB inflate window for
PNG), then w × h / 2 bytes each, purgeable. Partition 1 MB preferred, 384 KB
minimum.

## Layout

```
src/wrplat.h      platform seam (allocation, I/O callbacks) — C89, int-free
src/macroman.c    Unicode → Mac OS Roman, composition, stand-ins
src/inflate.c     streaming RFC 1951 decoder with 9-bit fast tables
src/zipdir.c      ZIP central directory, member streaming
src/docx.c        WordprocessingML and OpenDocument → MacRoman text + formatting bytes
src/image.c       PNG and JPEG → grey rows at the shown size (box-filter scaler)
src/main.c        the Mac application (menus, window, scrolling, AE, files)
src/WordReader.r  menus, alerts, SIZE, BNDL/FREF, icons (src/icons.r generated)
src/DuoShot.c     helper: screen → file via CopyBits (for the making-of GIF)
tests/            host harness (wrcat), verification, fuzzing
tools/            build on dev2019, deploy to the Duo, capture, GIF
```

## Build and test

```bash
# host: the portable core under clang, ASan, and against real documents
cc -DWR_HOST -std=c89 -pedantic -Wall -Wextra -o tests/wrcat tests/wrcat.c src/{docx,zipdir,inflate,macroman,image}.c
python3 tests/verify.py ~/Downloads/*.docx     # inflate byte-exact on every member, text matches
python3 tests/fuzz.py 3000 samples/*.docx samples/*.odt   # needs tests/wrcat_asan (-fsanitize=address,undefined)
tests/wrcat -fmt samples/Duo-Demo.odt         # text with the formatting bytes spelt out
tests/wrcat -pic word/media/image2.jpg out.pgm 432 270 samples/Pictures-Demo.docx   # decode one picture
python3 tests/fuzz.py 3000 pic.png photo.jpg  # pictures go through the decoder at random sizes
uv run --with python-docx --with odfpy --with pillow --with numpy python tools/make_sample_pictures.py

# Mac: compile/link/Rez on dev2019, then install on the Duo
uv run python tools/build_mac.py            # WordReader
uv run python tools/build_mac.py duoshot    # the capture helper
tools/deploy_duo.sh [WordReader] [DuoShot]
```

MPW notes learned here: `Fixed` is a Toolbox type (don't name a function
that); UI 3.4 moved control part codes and `scrollBarProc` to
ControlDefinitions.h; Rez Pascal strings stop at 255 characters; a running app
holds its file — quit it (`aevt/quit`) before relinking.

## License

MIT — see [LICENSE](LICENSE). © 2026 !Beaker (pitfo(AT)hotmail.de)
