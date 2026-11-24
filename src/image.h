/*
 * image.h - PNG and JPEG pictures, decoded to grey at the size they are shown.
 *
 * A picture is never held at full size: rows stream through a box-filter
 * scaler, so a 4000 x 3000 photo costs a few rows of memory, not 12 MB.
 * Output is 8-bit grey, 0 black .. 255 white, transparency laid over white.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include "wrplat.h"

enum {
    IMG_NONE = 0, IMG_PNG, IMG_JPEG, IMG_GIF, IMG_BMP, IMG_TIFF,
    IMG_EMF, IMG_WMF, IMG_SVG
};

typedef struct {
    short type;            /* IMG_* */
    long  width, height;   /* pixels, 0 if unknown */
    short progressive;     /* JPEG: progressive (shown from its first scan) */
    short interlaced;      /* PNG: Adam7 */
} ImgInfo;

enum {
    IMG_OK = 0,
    IMG_ERR_FORMAT = 30,   /* a picture format WordReader does not draw */
    IMG_ERR_DATA = 31,     /* damaged */
    IMG_ERR_MEM = 32,
    IMG_ERR_READ = 33,
    IMG_ERR_STOPPED = 34,  /* the row sink asked to stop */
    IMG_ERR_SIZE = 35      /* too large to decode in this memory */
};

/* One output row y of w grey pixels; non-zero stops the decoder. */
typedef long (*ImgRowFn)(void *ctx, short y, const wr_u8 *grey, short w);

/* What the picture is, from its first bytes. */
short img_probe(WrSource *s, ImgInfo *info);

/* Decode to outW x outH grey pixels, row by row from the top. */
short img_decode(WrSource *s, short outW, short outH, ImgRowFn row, void *ctx);

/* "PNG", "JPEG", "GIF", ... */
const char *img_type_name(short type);

#endif
