/*
 * WordReader.r - menus, alerts, SIZE, Finder bundle, icons.
 * Rez WordReader.r -a -o WordReader   (after every link)
 */
#include "Types.r"
#include "SysTypes.r"
#include "icons.r"

resource 'vers' (1) {
    0x01, 0x10, release, 0x00, verGermany,
    "1.1",
    "1.1, Word (.docx) and OpenDocument (.odt) reader for 68k Macs"
};

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    doesActivateOnFGSwitch, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    768 * 1024,      /* preferred (raise in Get Info for book-length documents) */
    384 * 1024       /* minimum: an 8 MB Duo running the bridge has little to spare */
};

resource 'MBAR' (128) { { 128, 129, 130, 131 } };

resource 'MENU' (128) {
    128, textMenuProc, 0x7FFFFFFD, enabled, apple,
    { "About WordReader…", noIcon, noKey, noMark, plain;
      "-", noIcon, noKey, noMark, plain }
};

resource 'MENU' (129) {
    129, textMenuProc, allEnabled, enabled, "File",
    { "Open…", noIcon, "O", noMark, plain;
      "Close", noIcon, "W", noMark, plain;
      "-", noIcon, noKey, noMark, plain;
      "Save as Text…", noIcon, "S", noMark, plain;
      "-", noIcon, noKey, noMark, plain;
      "Document Info…", noIcon, "I", noMark, plain;
      "-", noIcon, noKey, noMark, plain;
      "Quit", noIcon, "Q", noMark, plain }
};

resource 'MENU' (130) {
    130, textMenuProc, 0, disabled, "Edit",
    { "Undo", noIcon, "Z", noMark, plain;
      "-", noIcon, noKey, noMark, plain;
      "Cut", noIcon, "X", noMark, plain;
      "Copy", noIcon, "C", noMark, plain;
      "Paste", noIcon, "V", noMark, plain;
      "Clear", noIcon, noKey, noMark, plain;
      "-", noIcon, noKey, noMark, plain;
      "Select All", noIcon, "A", noMark, plain }
};

resource 'MENU' (131) {
    131, textMenuProc, allEnabled, enabled, "View",
    { "9 Point", noIcon, noKey, noMark, plain;
      "10 Point", noIcon, noKey, noMark, plain;
      "12 Point", noIcon, noKey, noMark, plain;
      "14 Point", noIcon, noKey, noMark, plain;
      "18 Point", noIcon, noKey, noMark, plain }
};

/* ---- alerts (sized for a 640 x 400 PowerBook screen) ---- */

resource 'ALRT' (128, "Error") {
    {80, 100, 200, 500}, 128,
    { OK, visible, sound1; OK, visible, sound1; OK, visible, sound1; OK, visible, sound1 },
    alertPositionMainScreen
};
resource 'DITL' (128) {
    {
        {88, 318, 108, 386}, Button { enabled, "OK" };
        {10, 60, 28, 390},   StaticText { disabled, "“^0”" };
        {30, 60, 82, 390},   StaticText { disabled, "^1" }
    }
};

resource 'ALRT' (129, "About") {
    {60, 90, 240, 510}, 129,
    { OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
    alertPositionMainScreen
};
resource 'DITL' (129) {
    {
        {148, 338, 168, 406}, Button { enabled, "OK" };
        {10, 60, 28, 410},    StaticText { disabled, "WordReader 1.1" };
        {32, 60, 140, 410},   StaticText { disabled,
            "Reads Word (.docx, .docm, .dotx, Word XML) and OpenDocument (.odt, .fodt) "
            "on a 68k Mac, with spacing, indents, bold and italic. Unicode becomes "
            "Mac OS Roman; what it lacks is spelt out or shown as ?." }
    }
};

resource 'ALRT' (130, "Info") {
    {40, 60, 300, 540}, 130,
    { OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
    alertPositionMainScreen
};
resource 'DITL' (130) {
    {
        {228, 398, 248, 466}, Button { enabled, "OK" };
        {10, 60, 28, 470},    StaticText { disabled, "^0" };
        {34, 60, 94, 470},    StaticText { disabled, "^1" };
        {100, 60, 150, 470},  StaticText { disabled, "^2" };
        {156, 60, 220, 470},  StaticText { disabled, "^3" }
    }
};

/* ---- Finder bundle ---- */

type 'WRdr' as 'STR ';
resource 'WRdr' (0, "Owner resource") { "WordReader 1.0 - Word documents on 68k Macs" };

resource 'BNDL' (128) {
    'WRdr', 0,
    {
        'ICN#', { 0, 128; 1, 129 };
        'FREF', { 0, 128; 1, 129; 2, 130; 3, 131 }
    }
};

resource 'FREF' (128) { 'APPL', 0, "" };
resource 'FREF' (129) { 'WXBN', 1, "" };   /* Word 2004+ .docx file type */
resource 'FREF' (130) { 'TEXT', 1, "" };   /* Word XML arriving as text */
resource 'FREF' (131) { '****', 1, "" };   /* anything dropped on the icon */
