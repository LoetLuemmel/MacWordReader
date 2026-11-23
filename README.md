# WordReader — Word documents (.docx) on a 68k Macintosh

A System 7 application that opens today's Microsoft Word documents on a
PowerBook Duo 230 (68030, 33 MHz, 8 MB). Built with MPW (SC 8.9, Universal
Interfaces 3.4) on the dev2019 guest, deployed to the Duo over AppleBridge.

## What it reads

| Format | How it is recognised |
|---|---|
| `.docx` `.docm` `.dotx` `.dotm` (Office Open XML, Transitional and Strict) | `PK\3\4`; main part found via `_rels/.rels` |
| Word 2003 XML, Flat OPC `.xml` | starts with `<` (UTF-8, optional BOM) |
| old binary `.doc`, password-protected `.docx` (OLE) | `D0 CF 11 E0` → a clear message, not garbage |
| `.xlsx` / `.pptx` | OOXML package without a Word part → "not a Word document" |

Shown: paragraphs, headings (Title, Heading 1–3, also German *Überschrift*),
bullets and list levels, tables (cells tab-separated, rows per line), tabs and
line breaks, a `[Picture]` marker for images. File ▸ Save as Text writes a
TeachText file (MacRoman, CR). File ▸ Document Info reports what the
conversion had to do.

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
Partition 768 KB preferred, 384 KB minimum.

## Layout

```
src/wrplat.h      platform seam (allocation, I/O callbacks) — C89, int-free
src/macroman.c    Unicode → Mac OS Roman, composition, stand-ins
src/inflate.c     streaming RFC 1951 decoder with 9-bit fast tables
src/zipdir.c      ZIP central directory, member streaming
src/docx.c        WordprocessingML → MacRoman text with style markers
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
python3 tests/fuzz.py 3000 samples/*.docx      # needs tests/wrcat_asan (-fsanitize=address,undefined)

# Mac: compile/link/Rez on dev2019, then install on the Duo
uv run python tools/build_mac.py            # WordReader
uv run python tools/build_mac.py duoshot    # the capture helper
tools/deploy_duo.sh [WordReader] [DuoShot]
```

MPW notes learned here: `Fixed` is a Toolbox type (don't name a function
that); UI 3.4 moved control part codes and `scrollBarProc` to
ControlDefinitions.h; Rez Pascal strings stop at 255 characters; a running app
holds its file — quit it (`aevt/quit`) before relinking.
