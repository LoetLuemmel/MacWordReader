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

static int raw = 0;
static long Write(void *ctx, const wr_u8 *b, long n)
{
    long i;
    (void)ctx;
    for (i = 0; i < n; i++) {
        wr_u8 c = b[i];
        if (raw) { putchar(c); continue; }
        if (c == '\r') { putchar('\n'); continue; }
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
