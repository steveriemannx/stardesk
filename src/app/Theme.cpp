#include "Theme.h"

#include "dui/Utils/StringConvert.h"

#include <cstdio>

namespace sdk {

bool Theme::s_isDark = false; // light by default

const Palette& Theme::Dark()
{
    static const Palette p = {
        ui::UiColor(0xFF14171C), // windowBg
        ui::UiColor(0xFF1B1F26), // panelBg
        ui::UiColor(0xFF22272F), // inputBg
        ui::UiColor(0xFF262C36), // hoverBg
        ui::UiColor(0xFF2F3642), // pressedBg
        ui::UiColor(0xFF2E3540), // border
        ui::UiColor(0xFFE6E9EE), // textMain
        ui::UiColor(0xFF8A93A3), // textSub
        ui::UiColor(0xFFFFFFFF), // textInvert
        ui::UiColor(0xFFA9B2BF), // prompt (brighter on dark)
        ui::UiColor(0xFF4F7CF7), // accent
        ui::UiColor(0xFF6B91F9), // accentHover
        ui::UiColor(0xFF3D67E8), // accentPressed
        ui::UiColor(0xFF34C77C), // success
        ui::UiColor(0xFFE5504D), // danger
        ui::UiColor(0xFF2E3540), // progressBg
        ui::UiColor(0xFF4F7CF7), // progressFg
        ui::UiColor(0xFF232932), // listHover
        ui::UiColor(0xFF2A3240), // listSelected
        ui::UiColor(0xFF14171C), // captionBg
    };
    return p;
}

const Palette& Theme::Light()
{
    static const Palette p = {
        ui::UiColor(0xFFF5F6F8), // windowBg
        ui::UiColor(0xFFFFFFFF), // panelBg
        ui::UiColor(0xFFFFFFFF), // inputBg
        ui::UiColor(0xFFECEEF2), // hoverBg
        ui::UiColor(0xFFE0E4EA), // pressedBg
        ui::UiColor(0xFFD9DEE6), // border
        ui::UiColor(0xFF1F2329), // textMain
        ui::UiColor(0xFF6B7280), // textSub
        ui::UiColor(0xFFFFFFFF), // textInvert
        ui::UiColor(0xFF9AA1AB), // prompt
        ui::UiColor(0xFF2E6BFF), // accent
        ui::UiColor(0xFF4D82FF), // accentHover
        ui::UiColor(0xFF1F5AE6), // accentPressed
        ui::UiColor(0xFF16A34A), // success
        ui::UiColor(0xFFDC2626), // danger
        ui::UiColor(0xFFE5E8ED), // progressBg
        ui::UiColor(0xFF2E6BFF), // progressFg
        ui::UiColor(0xFFF0F2F5), // listHover
        ui::UiColor(0xFFE3E9F5), // listSelected
        ui::UiColor(0xFFF5F6F8), // captionBg
    };
    return p;
}

// The palette values are the actual colours: dui renders them as declared on
// every platform. (An earlier revision swapped R and B on non-Windows builds
// to compensate for a BGR capture path; that workaround turned the blue accent
// orange and is gone.)

const Palette& Theme::Get()
{
    return s_isDark ? Dark() : Light();
}

void Theme::Set(bool dark)
{
    s_isDark = dark;
}

std::string Theme::Hex(ui::UiColor c)
{
    // UiColor stores 0xAARRGGBB; produce the "#AARRGGBB" string dui parses
    char buf[16];
    snprintf(buf, sizeof(buf), "#%08X", (unsigned int)c.GetARGB());
    return ui::StringConvert::UTF8ToT(buf);
}

} // namespace sdk
