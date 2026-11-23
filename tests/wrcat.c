/* wrcat - host test harness: run the Mac reader's core on a file.
   wrcat [-raw] file   ->  UTF-8 rendering (headings as "#"), stats on stderr
   -raw writes the exact MacRoman bytes the Mac would display. */
#include <stdio.h>
#include <string.h>
#include "../src/docx.h"
#include "../src/macroman.h"
#include "../src/zipdir.h"

static long ReadAt(void *ctx, wr_u32 off, wr_u8 *buf, long n)
{
    FILE *f = (FILE *)ctx;
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return (long)fread(buf, 1, (size_t)n, f);
}

static int raw = 0, fmt = 0;
static int skip = 0;              /* paragraph record bytes still to come */
static wr_u8 rec[DOCX_PARA_LEN];
static long Write(void *ctx, const wr_u8 *b, long n)
{
    long i;
    (void)ctx;
    for (i = 0; i < n; i++) {
        wr_u8 c = b[i];
        if (raw) { putchar(c); continue; }
        if (skip) {                               /* a record may straddle two writes */
            rec[DOCX_PARA_LEN - skip] = c;
            if (--skip == 0 && fmt)
                printf("{P b%d a%d l%d f%d j%d}", rec[1] - 32, rec[2] - 32, (rec[3] - 32) * 2,
                       (rec[4] - 32 - 100) * 2, rec[5] - 32);
            continue;
        }
        if (c == DOCX_PARA) { skip = DOCX_PARA_LEN - 1; rec[0] = c; continue; }
        if (DOCX_IS_FACE(c)) { if (fmt) printf("{%s%s%s%s}", c == 0x10 ? "plain" : "", c & 1 ? "B" : "", c & 2 ? "I" : "", c & 4 ? "U" : ""); continue; }
        if (DOCX_IS_SIZE(c)) { if (fmt) printf("{s%+d}", c - DOCX_SIZE0 - 2); continue; }
        if (c == '\r' || c == DOCX_CH_LINEBREAK) { putchar('\n'); continue; }
        if (c == DOCX_CH_EURO) { fputs("\xE2\x82\xAC", stdout); continue; }
        if (DOCX_IS_MARKER(c)) { fputs(c == 1 ? "#T " : c == 2 ? "# " : c == 3 ? "## " : "### ", stdout); continue; }
        {
            unsigned long u = mr_to_unicode(c);
            if (u < 0x80) putchar((int)u);
            else if (u < 0x800) { putchar(0xC0 | (u >> 6)); putchar(0x80 | (u & 0x3F)); }
            else { putchar(0xE0 | (u >> 12)); putchar(0x80 | ((u >> 6) & 0x3F)); putchar(0x80 | (u & 0x3F)); }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    WrSource src;
    DocxStats st;
    short err;
    const char *path = argv[argc - 1];
    if (argc > 2 && strcmp(argv[1], "-raw") == 0) raw = 1;
    if (argc > 2 && strcmp(argv[1], "-fmt") == 0) fmt = 1;
    f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END);
    src.size = (wr_u32)ftell(f);
    src.readAt = ReadAt;
    src.ctx = f;
    if (argc > 3 && strcmp(argv[1], "-member") == 0) {   /* wrcat -member name file: raw member bytes */
        ZipEntry e;
        raw = 1;
        err = zip_find(&src, argv[2], &e);
        if (err == 0) err = zip_extract(&src, &e, Write, 0);
        fclose(f);
        return err;
    }
    err = docx_convert(&src, Write, 0, &st);
    fprintf(stderr, "err=%d fmt=%d part=%s xml=%lu chars=%ld paras=%ld tables=%ld pics=%ld lossy=%ld subst=%ld euro=%ld\n",
            err, st.format, st.part, (unsigned long)st.xmlBytes, st.chars, st.paragraphs,
            st.tables, st.pictures, st.lossy, st.substituted, st.euros);
    fclose(f);
    return err;
}
