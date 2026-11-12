#ifndef STARDESK_APP_THEME_H_
#define STARDESK_APP_THEME_H_

#include "dui/dui.h"

namespace sdk {

/** One named palette slot. Every color a window paints goes through a slot,
 *  so switching theme = swapping the palette and re-applying colors.
 */
struct Palette {
    ui::UiColor windowBg;      // window background
    ui::UiColor panelBg;       // sidebar / cards / panels
    ui::UiColor inputBg;       // edit box background
    ui::UiColor hoverBg;       // hover highlight
    ui::UiColor pressedBg;     // pressed highlight
    ui::UiColor border;        // control borders / dividers
    ui::UiColor textMain;      // primary text
    ui::UiColor textSub;       // secondary text
    ui::UiColor textInvert;    // text drawn on accent
    ui::UiColor prompt;        // placeholder text inside inputs
    ui::UiColor accent;        // brand color (primary buttons, selection)
    ui::UiColor accentHover;
    ui::UiColor accentPressed;
    ui::UiColor success;       // online / done
    ui::UiColor danger;        // errors / reject
    ui::UiColor progressBg;
    ui::UiColor progressFg;
    ui::UiColor listHover;
    ui::UiColor listSelected;
    ui::UiColor captionBg;     // title bar
};

/** Light/dark theme manager. All UI code reads the palette through
 *  Theme::Get(); after Set() each open window re-applies its colors.
 */
class Theme {
public:
    static const Palette& Get();
    static bool IsDark() { return s_isDark; }
    static void Set(bool dark);

    /** "#AARRGGBB" string form, for SetAttribute("bkcolor", ...). */
    static std::string Hex(ui::UiColor c);

private:
    static const Palette& Dark();
    static const Palette& Light();
    static bool s_isDark;
};

} // namespace sdk

#endif // STARDESK_APP_THEME_H_
