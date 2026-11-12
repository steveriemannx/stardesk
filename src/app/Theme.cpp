#include "Theme.h"

#include "dui/Utils/StringConvert.h"
#include "dui/Core/GlobalManager.h"

#include <cstdio>

namespace sdk {

bool Theme::s_isDark = false; // light by default

const Palette& Theme::Dark()
{
    static const Palette p = {
        ui::UiColor(0xFF0F1115), // windowBg
        ui::UiColor(0xFF171A21), // panelBg
        ui::UiColor(0xFF1F232B), // inputBg
        ui::UiColor(0xFF242A34), // hoverBg
        ui::UiColor(0xFF2B323E), // pressedBg
        ui::UiColor(0xFF2C333E), // border
        ui::UiColor(0xFFE8EAED), // textMain
        ui::UiColor(0xFF9AA3AF), // textSub
        ui::UiColor(0xFFFFFFFF), // textInvert
        ui::UiColor(0xFF7D8694), // prompt
        ui::UiColor(0xFF4F7CF7), // accent
        ui::UiColor(0xFF6B93FF), // accentHover
        ui::UiColor(0xFF3D67E8), // accentPressed
        ui::UiColor(0xFF34D399), // success
        ui::UiColor(0xFFF87171), // danger
        ui::UiColor(0xFF2C333E), // progressBg
        ui::UiColor(0xFF4F7CF7), // progressFg
        ui::UiColor(0xFF1F242C), // listHover
        ui::UiColor(0xFF232B38), // listSelected
        ui::UiColor(0xFF14171C), // captionBg
    };
    return p;
}

const Palette& Theme::Light()
{
    static const Palette p = {
        ui::UiColor(0xFFF4F5F7), // windowBg
        ui::UiColor(0xFFFFFFFF), // panelBg
        ui::UiColor(0xFFFFFFFF), // inputBg
        ui::UiColor(0xFFF1F3F6), // hoverBg
        ui::UiColor(0xFFE6E9EF), // pressedBg
        ui::UiColor(0xFFE3E6EB), // border
        ui::UiColor(0xFF111827), // textMain
        ui::UiColor(0xFF6B7280), // textSub
        ui::UiColor(0xFFFFFFFF), // textInvert
        ui::UiColor(0xFF9CA3AF), // prompt
        ui::UiColor(0xFF2563EB), // accent
        ui::UiColor(0xFF3B82F6), // accentHover
        ui::UiColor(0xFF1D4ED8), // accentPressed
        ui::UiColor(0xFF10B981), // success
        ui::UiColor(0xFFEF4444), // danger
        ui::UiColor(0xFFE5E7EB), // progressBg
        ui::UiColor(0xFF2563EB), // progressFg
        ui::UiColor(0xFFF3F4F6), // listHover
        ui::UiColor(0xFFE8F0FE), // listSelected
        ui::UiColor(0xFFFFFFFF), // captionBg
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

void Theme::SyncGlobalColors()
{
    const Palette& p = Get();
    auto& colorMgr = ui::GlobalManager::Instance().Color();
    struct Entry {
        const char* name;
        ui::UiColor color;
    };
    const Entry entries[] = {
        {"white", ui::UiColor(0xFFFFFFFF)},
        {"black", ui::UiColor(0xFF000000)},
        {"darkcolor", ui::UiColor(0xFF333333)},
        {"lightcolor", ui::UiColor(0xFF888888)},
        {"sd_text_main", p.textMain},
        {"sd_text_sub", p.textSub},
        {"sd_text_invert", p.textInvert},
        {"sd_accent", p.accent},
        {"sd_success", p.success},
        {"sd_danger", p.danger},
    };
    // re-registering the whole map keeps the names stable while the values
    // follow the theme (ColorManager has no single-entry remove)
    colorMgr.RemoveAllColors();
    for (const Entry& entry : entries) {
        colorMgr.AddColor(entry.name, entry.color);
    }
    colorMgr.SetDefaultTextColor("darkcolor");
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
