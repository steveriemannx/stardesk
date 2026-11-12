#include "KeyMap.h"

#include "dui/Core/Keycode.h"

#if defined(__linux__)
#include <linux/input-event-codes.h>
#endif

namespace sdk {

// macOS HID virtual keycodes (physical key positions)
static const struct {
    uint16_t vk;     // dui ui::kVK_* (Windows VK)
    uint16_t mac;    // HID virtual keycode
} kMacMap[] = {
    { ui::kVK_BACK, 0x33 },       // Backspace -> ui::kVK_Delete
    { ui::kVK_TAB, 0x30 },
    { ui::kVK_RETURN, 0x24 },     // Enter -> ui::kVK_Return
    { ui::kVK_SHIFT, 0x38 },      // ui::kVK_Shift (left)
    { ui::kVK_CONTROL, 0x3B },
    { ui::kVK_MENU, 0x3A },       // Alt -> ui::kVK_Option
    { ui::kVK_PAUSE, 0x72 },      // Pause -> Help
    { ui::kVK_CAPITAL, 0x39 },    // CapsLock
    { ui::kVK_ESCAPE, 0x35 },
    { ui::kVK_SPACE, 0x31 },
    { ui::kVK_PRIOR, 0x74 },      // PageUp
    { ui::kVK_NEXT, 0x79 },       // PageDown
    { ui::kVK_END, 0x77 },
    { ui::kVK_HOME, 0x73 },
    { ui::kVK_LEFT, 0x7B },
    { ui::kVK_UP, 0x7E },
    { ui::kVK_RIGHT, 0x7C },
    { ui::kVK_DOWN, 0x7D },
    { ui::kVK_SNAPSHOT, 0x6C },   // PrintScreen (F13 area)
    { ui::kVK_INSERT, 0x72 },     // Insert -> Help
    { ui::kVK_DELETE, 0x75 },     // Delete -> ForwardDelete
    { ui::kVK_HELP, 0x72 },
    { ui::kVK_LWIN, 0x37 },       // Left Win -> Command
    { ui::kVK_RWIN, 0x37 },
    { ui::kVK_APPS, 0x36 },       // Menu key
    { ui::kVK_NUMPAD0, 0x52 }, { ui::kVK_NUMPAD1, 0x53 }, { ui::kVK_NUMPAD2, 0x54 },
    { ui::kVK_NUMPAD3, 0x55 }, { ui::kVK_NUMPAD4, 0x56 }, { ui::kVK_NUMPAD5, 0x57 },
    { ui::kVK_NUMPAD6, 0x58 }, { ui::kVK_NUMPAD7, 0x59 }, { ui::kVK_NUMPAD8, 0x5B },
    { ui::kVK_NUMPAD9, 0x5C },
    { ui::kVK_MULTIPLY, 0x43 },   // Keypad *
    { ui::kVK_ADD, 0x45 },        // Keypad +
    { ui::kVK_SUBTRACT, 0x4E },   // Keypad -
    { ui::kVK_DECIMAL, 0x41 },    // Keypad .
    { ui::kVK_DIVIDE, 0x4B },     // Keypad /
    { ui::kVK_NUMLOCK, 0x47 },    // Keypad Clear
    { ui::kVK_F1, 0x7A }, { ui::kVK_F2, 0x78 }, { ui::kVK_F3, 0x63 }, { ui::kVK_F4, 0x76 },
    { ui::kVK_F5, 0x60 }, { ui::kVK_F6, 0x61 }, { ui::kVK_F7, 0x62 }, { ui::kVK_F8, 0x64 },
    { ui::kVK_F9, 0x65 }, { ui::kVK_F10, 0x6D }, { ui::kVK_F11, 0x67 }, { ui::kVK_F12, 0x6F },
    { ui::kVK_F13, 0x69 }, { ui::kVK_F14, 0x6B }, { ui::kVK_F15, 0x71 }, { ui::kVK_F16, 0x6A },
    { ui::kVK_F17, 0x40 }, { ui::kVK_F18, 0x4F }, { ui::kVK_F19, 0x50 }, { ui::kVK_F20, 0x5A },
    { ui::kVK_F21, 0x5B }, { ui::kVK_F22, 0x5C }, { ui::kVK_F23, 0x5D }, { ui::kVK_F24, 0x5E },
    { ui::kVK_OEM_1, 0x29 },      // ;: -> ui::kVK_Semicolon
    { ui::kVK_OEM_PLUS, 0x18 },   // =+ -> ui::kVK_ANSI_Equal
    { ui::kVK_OEM_COMMA, 0x2B },  // ,<
    { ui::kVK_OEM_MINUS, 0x1B },  // -_
    { ui::kVK_OEM_PERIOD, 0x2F }, // .>
    { ui::kVK_OEM_2, 0x2C },      // /?
    { ui::kVK_OEM_3, 0x32 },      // `~
    { ui::kVK_OEM_4, 0x21 },      // [{
    { ui::kVK_OEM_5, 0x2A },      // \|
    { ui::kVK_OEM_6, 0x1E },      // ]}
    { ui::kVK_OEM_7, 0x27 },      // '"
};

// Letters are physical positions (QWERTY layout), not alphabetical order
static const uint16_t kLetterMac[26] = {
    /* A */ 0x00, /* B */ 0x0B, /* C */ 0x08, /* D */ 0x02, /* E */ 0x0E,
    /* F */ 0x03, /* G */ 0x05, /* H */ 0x04, /* I */ 0x22, /* J */ 0x26,
    /* K */ 0x28, /* L */ 0x25, /* M */ 0x2E, /* N */ 0x2D, /* O */ 0x1F,
    /* P */ 0x23, /* Q */ 0x0C, /* R */ 0x0F, /* S */ 0x01, /* T */ 0x11,
    /* U */ 0x20, /* V */ 0x09, /* W */ 0x0D, /* X */ 0x07, /* Y */ 0x10,
    /* Z */ 0x06,
};

static const uint16_t kDigitMac[10] = {
    /* 0 */ 0x1D, /* 1 */ 0x12, /* 2 */ 0x13, /* 3 */ 0x14, /* 4 */ 0x15,
    /* 5 */ 0x17, /* 6 */ 0x16, /* 7 */ 0x1A, /* 8 */ 0x1C, /* 9 */ 0x19,
};

uint16_t KeyMap::ToMacVirtualKey(uint16_t vkCode)
{
    if (vkCode >= ui::kVK_A && vkCode <= ui::kVK_Z) {
        return kLetterMac[vkCode - ui::kVK_A];
    }
    if (vkCode >= ui::kVK_0 && vkCode <= ui::kVK_9) {
        return kDigitMac[vkCode - ui::kVK_0];
    }
    for (const auto& e : kMacMap) {
        if (e.vk == vkCode) {
            return e.mac;
        }
    }
    return 0xFFFF;
}

uint32_t KeyMap::ToX11KeySym(uint16_t vkCode)
{
    if (vkCode >= ui::kVK_A && vkCode <= ui::kVK_Z) {
        return 0x61 + (vkCode - ui::kVK_A); // XK_a..
    }
    if (vkCode >= ui::kVK_0 && vkCode <= ui::kVK_9) {
        return 0x30 + (vkCode - ui::kVK_0); // XK_0..
    }
    switch (vkCode) {
    case ui::kVK_BACK: return 0xFF08;       // XK_BackSpace
    case ui::kVK_TAB: return 0xFF09;        // XK_Tab
    case ui::kVK_RETURN: return 0xFF0D;     // XK_Return
    case ui::kVK_SHIFT: return 0xFFE1;      // XK_Shift_L
    case ui::kVK_CONTROL: return 0xFFE3;    // XK_Control_L
    case ui::kVK_MENU: return 0xFFE9;       // XK_Alt_L
    case ui::kVK_LWIN: return 0xFFEB;       // XK_Super_L
    case ui::kVK_RWIN: return 0xFFEC;       // XK_Super_R
    case ui::kVK_CAPITAL: return 0xFFE5;    // XK_Caps_Lock
    case ui::kVK_ESCAPE: return 0xFF1B;     // XK_Escape
    case ui::kVK_SPACE: return 0x20;        // XK_space
    case ui::kVK_PRIOR: return 0xFF55;      // XK_Page_Up
    case ui::kVK_NEXT: return 0xFF56;       // XK_Page_Down
    case ui::kVK_END: return 0xFF57;        // XK_End
    case ui::kVK_HOME: return 0xFF50;       // XK_Home
    case ui::kVK_LEFT: return 0xFF51;       // XK_Left
    case ui::kVK_UP: return 0xFF52;         // XK_Up
    case ui::kVK_RIGHT: return 0xFF53;      // XK_Right
    case ui::kVK_DOWN: return 0xFF54;       // XK_Down
    case ui::kVK_INSERT: return 0xFF63;     // XK_Insert
    case ui::kVK_DELETE: return 0xFFFF;     // XK_Delete
    case ui::kVK_NUMPAD0: return 0xFFB0;
    case ui::kVK_NUMPAD1: return 0xFFB1;
    case ui::kVK_NUMPAD2: return 0xFFB2;
    case ui::kVK_NUMPAD3: return 0xFFB3;
    case ui::kVK_NUMPAD4: return 0xFFB4;
    case ui::kVK_NUMPAD5: return 0xFFB5;
    case ui::kVK_NUMPAD6: return 0xFFB6;
    case ui::kVK_NUMPAD7: return 0xFFB7;
    case ui::kVK_NUMPAD8: return 0xFFB8;
    case ui::kVK_NUMPAD9: return 0xFFB9;
    case ui::kVK_MULTIPLY: return 0xFFAA;   // XK_KP_Multiply
    case ui::kVK_ADD: return 0xFFAB;        // XK_KP_Add
    case ui::kVK_SUBTRACT: return 0xFFAD;   // XK_KP_Subtract
    case ui::kVK_DECIMAL: return 0xFFAE;    // XK_KP_Decimal
    case ui::kVK_DIVIDE: return 0xFFAF;     // XK_KP_Divide
    case ui::kVK_NUMLOCK: return 0xFF7F;    // XK_Num_Lock
    case ui::kVK_OEM_1: return 0x3B;        // ;
    case ui::kVK_OEM_PLUS: return 0x3D;     // =
    case ui::kVK_OEM_COMMA: return 0x2C;    // ,
    case ui::kVK_OEM_MINUS: return 0x2D;    // -
    case ui::kVK_OEM_PERIOD: return 0x2E;   // .
    case ui::kVK_OEM_2: return 0x2F;        // /
    case ui::kVK_OEM_3: return 0x60;        // `
    case ui::kVK_OEM_4: return 0x5B;        // [
    case ui::kVK_OEM_5: return 0x5C;        // backslash
    case ui::kVK_OEM_6: return 0x5D;        // ]
    case ui::kVK_OEM_7: return 0x27;        // '
    default: break;
    }
    if (vkCode >= ui::kVK_F1 && vkCode <= ui::kVK_F24) {
        return 0xFFBE + (vkCode - ui::kVK_F1); // XK_F1..XK_F24
    }
    return 0;
}

#if defined(__linux__)
int KeyMap::ToLinuxKeyCode(uint16_t vkCode)
{
    // NOTE: the Linux KEY_* codes are NOT alphabetical or sequential
    // (KEY_A=30, KEY_B=48, KEY_C=46, KEY_L=38 ...; digits too: KEY_0=11,
    // KEY_1=2). A table lookup is required — an offset like
    // KEY_A + (vkCode - kVK_A) produces wrong keys for every letter but A.
    static const int kLetterLinux[26] = {
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
        KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
        KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    };
    static const int kDigitLinux[10] = {
        KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    };
    // Letters (physical key positions)
    if (vkCode >= ui::kVK_A && vkCode <= ui::kVK_Z) {
        return kLetterLinux[vkCode - ui::kVK_A];
    }
    // Digits
    if (vkCode >= ui::kVK_0 && vkCode <= ui::kVK_9) {
        return kDigitLinux[vkCode - ui::kVK_0];
    }
    // Function keys
    if (vkCode >= ui::kVK_F1 && vkCode <= ui::kVK_F24) {
        return KEY_F1 + (vkCode - ui::kVK_F1);
    }
    // Numpad
    if (vkCode >= ui::kVK_NUMPAD0 && vkCode <= ui::kVK_NUMPAD9) {
        return KEY_KP0 + (vkCode - ui::kVK_NUMPAD0);
    }
    switch (vkCode) {
    case ui::kVK_BACK:       return KEY_BACKSPACE;
    case ui::kVK_TAB:        return KEY_TAB;
    case ui::kVK_RETURN:     return KEY_ENTER;
    case ui::kVK_SHIFT:      return KEY_LEFTSHIFT;
    case ui::kVK_CONTROL:    return KEY_LEFTCTRL;
    case ui::kVK_MENU:       return KEY_LEFTALT;
    case ui::kVK_LWIN:       return KEY_LEFTMETA;
    case ui::kVK_RWIN:       return KEY_RIGHTMETA;
    case ui::kVK_CAPITAL:    return KEY_CAPSLOCK;
    case ui::kVK_ESCAPE:     return KEY_ESC;
    case ui::kVK_SPACE:      return KEY_SPACE;
    case ui::kVK_PRIOR:      return KEY_PAGEUP;
    case ui::kVK_NEXT:       return KEY_PAGEDOWN;
    case ui::kVK_END:        return KEY_END;
    case ui::kVK_HOME:       return KEY_HOME;
    case ui::kVK_LEFT:       return KEY_LEFT;
    case ui::kVK_UP:         return KEY_UP;
    case ui::kVK_RIGHT:      return KEY_RIGHT;
    case ui::kVK_DOWN:       return KEY_DOWN;
    case ui::kVK_INSERT:     return KEY_INSERT;
    case ui::kVK_DELETE:     return KEY_DELETE;
    case ui::kVK_SNAPSHOT:   return KEY_SYSRQ;
    case ui::kVK_SCROLL:     return KEY_SCROLLLOCK;
    case ui::kVK_PAUSE:      return KEY_PAUSE;
    case ui::kVK_NUMLOCK:    return KEY_NUMLOCK;
    case ui::kVK_MULTIPLY:   return KEY_KPASTERISK;
    case ui::kVK_ADD:        return KEY_KPPLUS;
    case ui::kVK_SUBTRACT:   return KEY_KPMINUS;
    case ui::kVK_DECIMAL:    return KEY_KPDOT;
    case ui::kVK_DIVIDE:     return KEY_KPSLASH;
    // OEM punctuation: physical keys on a US QWERTY layout
    case ui::kVK_OEM_1:      return KEY_SEMICOLON;  // ;:
    case ui::kVK_OEM_PLUS:   return KEY_EQUAL;      // =+
    case ui::kVK_OEM_COMMA:  return KEY_COMMA;      // ,<
    case ui::kVK_OEM_MINUS:  return KEY_MINUS;      // -_
    case ui::kVK_OEM_PERIOD: return KEY_DOT;        // .>
    case ui::kVK_OEM_2:      return KEY_SLASH;      // /?
    case ui::kVK_OEM_3:      return KEY_GRAVE;      // `~
    case ui::kVK_OEM_4:      return KEY_LEFTBRACE;  // [{
    case ui::kVK_OEM_5:      return KEY_BACKSLASH;  // \|
    case ui::kVK_OEM_6:      return KEY_RIGHTBRACE; // ]}
    case ui::kVK_OEM_7:      return KEY_APOSTROPHE; // '"
    default: break;
    }
    return -1; // unmapped
}
#else
int KeyMap::ToLinuxKeyCode(uint16_t /*vkCode*/) { return -1; }
#endif

} // namespace sdk
