/* wrcat - host test harness: run the Mac reader's core on a file.
   wrcat [-raw] file   ->  UTF-8 rendering (headings as "#"), stats on stderr
   -raw writes the exact MacRoman bytes the Mac would display. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/docx.h"
#include "../src/macroman.h"
#include "../src/zipdir.h"
#include "../src/image.h"

static long ReadAt(void *ctx, wr_u32 off, wr_u8 *buf, long n)
{
    FILE *f = (FILE *)ctx;
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return (long)fread(buf, 1, (size_t)n, f);
}

static int raw = 0, fmt = 0;
static long need = 0;             /* bytes of a paragraph record / picture item still to come */
static long got = 0;
static wr_u8 rec[DOCX_PIC_HEAD + 256];
static long Write(void *ctx, const wr_u8 *b, long n)
{
    long i;
    (void)ctx;
    for (i = 0; i < n; i++) {
        wr_u8 c = b[i];
        if (raw) { putchar(c); continue; }
        if (need) {                               /* an item may straddle two writes */
            rec[got++] = c;
            if (rec[0] == DOCX_PIC && got == DOCX_PIC_HEAD)
                need += c - 0x20;
            if (--need == 0) {
                if (rec[0] == DOCX_PARA && fmt)
                    printf("{P b%d a%d l%d f%d j%d}", rec[1] - 32, rec[2] - 32, (rec[3] - 32) * 2,
                           (rec[4] - 32 - 100) * 2, rec[5] - 32);
                if (rec[0] == DOCX_PIC) {
                    short w, h;
                    char name[DOCX_PIC_NAMEMAX + 1];
                    if (!docx_pic_info(rec, 0, got, &w, &h, name))
                        printf("[Picture: bad item]");
                    else if (fmt)
                        printf("[Picture %dx%dpt %s]", w, h, name);
                    else
                        printf("[Picture]");
                }
            }
            continue;
        }
        if (c == DOCX_PARA || c == DOCX_PIC) { need = c == DOCX_PARA ? DOCX_PARA_LEN - 1 : DOCX_PIC_HEAD - 1; got = 1; rec[0] = c; continue; }
        if (DOCX_IS_FACE(c)) { if (fmt) printf("{%s%s%s%s}", c == 0x10 ? "plain" : "", c & 1 ? "B" : "", c & 2 ? "I" : "", c & 4 ? "U" : ""); continue; }
        if (DOCX_IS_SIZE(c)) { if (fmt) printf("{s%+d}", c - DOCX_SIZE0 - 2); continue; }
        if (DOCX_IS_SUP(c)) { putchar(c == DOCX_SUP_ON ? '[' : ']'); continue; }
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

/* -img / -pic: decode a picture to a PGM file */
static FILE *gPgm;
static long PgmRow(void *ctx, short y, const wr_u8 *g, short w)
{
    (void)ctx; (void)y;
    fwrite(g, 1, (size_t)w, gPgm);
    return 0;
}

static long TmpWrite(void *ctx, const wr_u8 *b, long n)
{
    return fwrite(b, 1, (size_t)n, (FILE *)ctx) == (size_t)n ? 0 : 1;
}

typedef struct { WrSource *base; wr_u32 off; } Sub;
static long SubReadAt(void *ctx, wr_u32 off, wr_u8 *buf, long n)
{
    Sub *s = (Sub *)ctx;
    return s->base->readAt(s->base->ctx, s->off + off, buf, n);
}

static int Picture(WrSource *src, const char *member, const char *out, int w, int h)
{
    WrSource pic = *src;
    Sub sub;
    FILE *tmp = 0;
    ImgInfo info;
    short err;
    if (member) {                     /* a member of the package: stored or inflated to a file */
        ZipEntry e;
        err = zip_find(src, member, &e);
        if (err) return err;
        if (e.method == 0) {
            sub.base = src;
            sub.off = e.dataOff;
            pic.readAt = SubReadAt;
            pic.ctx = &sub;
            pic.size = e.size;
        } else {
            tmp = tmpfile();
            err = zip_extract(src, &e, TmpWrite, tmp);
            if (err) return err;
            pic.readAt = ReadAt;
            pic.ctx = tmp;
            pic.size = e.size;
        }
    }
    err = img_probe(&pic, &info);
    fprintf(stderr, "probe=%d type=%s %ldx%ld progressive=%d interlaced=%d\n", err,
            img_type_name(info.type), info.width, info.height, info.progressive, info.interlaced);
    gPgm = fopen(out, "wb");
    fprintf(gPgm, "P5\n%d %d\n255\n", w, h);
    err = img_decode(&pic, (short)w, (short)h, PgmRow, 0);
    fclose(gPgm);
    if (tmp) fclose(tmp);
    fprintf(stderr, "decode=%d\n", err);
    return err;
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
    if (argc == 6 && strcmp(argv[1], "-img") == 0) {    /* wrcat -img out.pgm W H file */
        err = (short)Picture(&src, 0, argv[2], atoi(argv[3]), atoi(argv[4]));
        fclose(f);
        return err;
    }
    if (argc == 7 && strcmp(argv[1], "-pic") == 0) {    /* wrcat -pic member out.pgm W H file */
        err = (short)Picture(&src, argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
        fclose(f);
        return err;
    }
    if (argc > 3 && strcmp(argv[1], "-member") == 0) {   /* wrcat -member name file: raw member bytes */
        ZipEntry e;
        raw = 1;
        err = zip_find(&src, argv[2], &e);
        if (err == 0) err = zip_extract(&src, &e, Write, 0);
        fclose(f);
        return err;
    }
    err = docx_convert(&src, Write, 0, &st);
    fprintf(stderr, "err=%d fmt=%d part=%s xml=%lu chars=%ld paras=%ld tables=%ld pics=%ld fn=%ld en=%ld lossy=%ld subst=%ld euro=%ld\n",
            err, st.format, st.part, (unsigned long)st.xmlBytes, st.chars, st.paragraphs,
            st.tables, st.pictures, st.footnotes, st.endnotes, st.lossy, st.substituted, st.euros);
    fclose(f);
    return err;
}
