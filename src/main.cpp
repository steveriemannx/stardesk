#include "dui/dui.h"
#include "MainThread.h"
#include "dui/Utils/AppEntry.h"

/** StarDesk entry point.
 *
 *  The whole UI is built in pure code (no XML layouts, no resource files):
 *  fonts/colors are registered at startup by InitThemeCompileTime(), and every
 *  window builds its controls in OnInitWindow(). The runtime only needs the
 *  resource directory for dui's internal startup; if it is missing the app
 *  still works because no theme resource is actually loaded.
 */
DUI_APP_ENTRY(sdk::MainThread)
