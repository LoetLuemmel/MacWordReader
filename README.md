# WordReader — Word (.docx) and OpenDocument (.odt) on a 68k Macintosh

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
tab-separated, rows per line); tabs and line breaks; a `[Picture]` marker for
images. Footnote bodies and comments are left out. File ▸ Save as Text writes
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
Partition 768 KB preferred, 384 KB minimum.

## Layout

```
src/wrplat.h      platform seam (allocation, I/O callbacks) — C89, int-free
src/macroman.c    Unicode → Mac OS Roman, composition, stand-ins
src/inflate.c     streaming RFC 1951 decoder with 9-bit fast tables
src/zipdir.c      ZIP central directory, member streaming
src/docx.c        WordprocessingML and OpenDocument → MacRoman text + formatting bytes
src/main.c        the Mac application (menus, window, scrolling, AE, files)
src/WordReader.r  menus, alerts, SIZE, BNDL/FREF, icons (src/icons.r generated)
src/DuoShot.c     helper: screen → file via CopyBits (for the making-of GIF)
tests/            host harness (wrcat), verification, fuzzing
tools/            build on dev2019, deploy to the Duo, capture, GIF
```

## Build and test

```bash
# host: the portable core under clang, ASan, and against real documents
cc -DWR_HOST -std=c89 -pedantic -Wall -Wextra -o tests/wrcat tests/wrcat.c src/{docx,zipdir,inflate,macroman}.c
python3 tests/verify.py ~/Downloads/*.docx     # inflate byte-exact on every member, text matches
python3 tests/fuzz.py 3000 samples/*.docx samples/*.odt   # needs tests/wrcat_asan (-fsanitize=address,undefined)
tests/wrcat -fmt samples/Duo-Demo.odt         # text with the formatting bytes spelt out

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
