#ifndef STARDESK_PLATFORM_KEY_MAP_H_
#define STARDESK_PLATFORM_KEY_MAP_H_

#include <cstdint>

namespace sdk {

/** Wire-format keys are dui's VirtualKeyCode (kVK_*, Windows VK values - see
 *  dui/Core/Keycode.h). This maps them to the platform's native key codes:
 *  Windows is a passthrough; macOS uses the HID virtual keycodes; X11 uses
 *  X keysyms (converted to keycodes at injection time by the display);
 *  Wayland/Linux uses the kernel input-event codes (KEY_*).
 */
class KeyMap {
public:
    /** macOS HID virtual keycode (CGEvent), 0xFFFF when unmapped. */
    static uint16_t ToMacVirtualKey(uint16_t vkCode);

    /** X11 keysym (XK_*), 0 when unmapped. */
    static uint32_t ToX11KeySym(uint16_t vkCode);

    /** Linux input-event keycode (KEY_* from linux/input-event-codes.h),
     *  -1 when unmapped. Used by the Wayland uinput path. */
    static int ToLinuxKeyCode(uint16_t vkCode);
};

} // namespace sdk

#endif // STARDESK_PLATFORM_KEY_MAP_H_
