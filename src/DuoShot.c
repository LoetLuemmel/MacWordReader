/*
 * DuoShot - write the main screen to "DuoShot.raw" next to the app, then quit.
 *
 * A helper for recording the PowerBook Duo: the bridge daemon's screenshot
 * reads the frame buffer directly, which on a real Duo with dock video comes
 * back as noise. CopyBits knows how to reach the frame buffer (32-bit
 * addressing included), so this copies the screen into an 8-bit GWorld first.
 *
 * File: 'DSHT', width, height (shorts), 256 x RGB (6 bytes each), then
 * width x height bytes of pixels (colour-table indices).
 */
#include <Types.h>
#include <MacMemory.h>
#include <QuickDraw.h>
#include <QDOffscreen.h>
#include <Fonts.h>
#include <Windows.h>
#include <Files.h>
#include <Events.h>

QDGlobals qd;

#define kBand 32

static OSErr WriteAll(short ref, const void *p, long n)
{
    return FSWrite(ref, &n, p);
}

int main(void)
{
    GDHandle gd;
    PixMapHandle spm, dpm;
    GWorldPtr gw = 0;
    CGrafPtr savePort;
    GDHandle saveGD;
    Rect r, band;
    short y0;
    QDErr qerr;
    FSSpec tmp, fin;
    short ref, w, h, y, i;
    long rb;
    Ptr row;
    char head[8];
    unsigned char rgb[256 * 6];
    CTabHandle ct;
    OSErr err;

    /* QuickDraw only: a background-only application must not touch the
       Window Manager - InitWindows in one hung on the Duo (2026-11-23). */
    InitGraf(&qd.thePort);

    gd = GetMainDevice();
    spm = (*gd)->gdPMap;
    r = (*gd)->gdRect;
    w = (short)(r.right - r.left);
    h = (short)(r.bottom - r.top);

    /* 8-bit, the screen's own colour table when it is 8-bit (a plain copy).
       Banded: a 1152 x 870 screen is 1 MB, a real Duo has not that much free. */
    ct = ((*spm)->pixelSize == 8) ? (*spm)->pmTable : 0;
    SetRect(&band, 0, 0, w, kBand);
    qerr = NewGWorld(&gw, 8, &band, ct, 0, 0);
    if (qerr != noErr || gw == 0)
        return 1;
    dpm = GetGWorldPixMap(gw);

    FSMakeFSSpec(0, 0, "\pDuoShot.tmp", &tmp);
    FSMakeFSSpec(0, 0, "\pDuoShot.raw", &fin);
    FSpDelete(&tmp);
    FSpDelete(&fin);
    err = FSpCreate(&tmp, 'DSht', 'DSHT', 0);
    if (err == noErr)
        err = FSpOpenDF(&tmp, fsWrPerm, &ref);
    if (err != noErr)
        return 1;

    head[0] = 'D'; head[1] = 'S'; head[2] = 'H'; head[3] = 'T';
    head[4] = (char)(w >> 8); head[5] = (char)w;
    head[6] = (char)(h >> 8); head[7] = (char)h;
    WriteAll(ref, head, 8);

    ct = (*dpm)->pmTable;
    for (i = 0; i < 256 * 6; i++)
        rgb[i] = 0;
    for (i = 0; i <= (*ct)->ctSize && i < 256; i++) {
        ColorSpec *cs = &(*ct)->ctTable[i];
        short v = (short)((*ct)->ctFlags & 0x8000 ? i : cs->value);
        if (v < 0 || v > 255)
            continue;
        rgb[v * 6 + 0] = (unsigned char)(cs->rgb.red >> 8);
        rgb[v * 6 + 1] = (unsigned char)(cs->rgb.red);
        rgb[v * 6 + 2] = (unsigned char)(cs->rgb.green >> 8);
        rgb[v * 6 + 3] = (unsigned char)(cs->rgb.green);
        rgb[v * 6 + 4] = (unsigned char)(cs->rgb.blue >> 8);
        rgb[v * 6 + 5] = (unsigned char)(cs->rgb.blue);
    }
    WriteAll(ref, rgb, sizeof(rgb));

    row = NewPtr(w);
    if (row) {
        for (y0 = 0; y0 < h; y0 = (short)(y0 + kBand)) {
            short bh = (short)(h - y0 < kBand ? h - y0 : kBand);
            Rect src, dst;
            Ptr base;
            Boolean is32;
            SetRect(&src, r.left, (short)(r.top + y0), r.right, (short)(r.top + y0 + bh));
            SetRect(&dst, 0, 0, w, bh);
            LockPixels(dpm);
            GetGWorld(&savePort, &saveGD);
            SetGWorld(gw, 0);
            HLock((Handle)spm);
            CopyBits((BitMap *)*spm, (BitMap *)*dpm, &src, &dst, srcCopy, 0);
            HUnlock((Handle)spm);
            SetGWorld(savePort, saveGD);
            rb = (*dpm)->rowBytes & 0x3FFF;
            base = GetPixBaseAddr(dpm);
            is32 = PixMap32Bit(dpm);
            for (y = 0; y < bh; y++) {
                SInt8 mode = true32b;
                if (is32)
                    SwapMMUMode(&mode);
                BlockMoveData(base + (long)y * rb, row, w);
                if (is32)
                    SwapMMUMode(&mode);
                WriteAll(ref, row, w);
            }
            UnlockPixels(dpm);
        }
        DisposePtr(row);
    }
    FSClose(ref);
    DisposeGWorld(gw);
    FSpRename(&tmp, "\pDuoShot.raw");
    FlushVol(0, tmp.vRefNum);
    return 0;
}
