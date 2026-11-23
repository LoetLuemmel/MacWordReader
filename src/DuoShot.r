/* DuoShot.r - a quiet helper: no menus, no windows, never the front app
   (onlyBackground), so the screen it records stays as it was. */
#include "Types.r"
resource 'SIZE' (-1) {
    reserved, ignoreSuspendResumeEvents, reserved, canBackground,
    needsActivateOnFGSwitch, onlyBackground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, notHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    192 * 1024, 128 * 1024
};
