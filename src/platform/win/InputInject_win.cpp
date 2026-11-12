// Windows input injection via SendInput.

#include <windows.h>

#include "../InputInject.h"

namespace sdk {

bool InputInjector::MoveTo(double nx, double ny, int screenW, int screenH)
{
    // SendInput absolute coordinates are normalized to 0..65535 across the
    // whole virtual screen; map the logical screen into it
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        return false;
    }
    const long x = (long)(nx * screenW * 65535.0 / vw);
    const long y = (long)(ny * screenH * 65535.0 / vh);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = x;
    in.mi.dy = y;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool InputInjector::Button(bool down, int button)
{
    DWORD flags = 0;
    switch (button) {
    case 1: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 2: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    default: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    }
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool InputInjector::MoveDelta(int, int) { return false; } // SendInput is absolute, no delta needed
bool InputInjector::GetTrackedPos(int&, int&) { return false; } // absolute positioning, no tracker
void InputInjector::ResetCursorTo(int, int) {} // absolute positioning, no tracker

bool InputInjector::Wheel(int deltaLines)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(deltaLines * WHEEL_DELTA);
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool InputInjector::Key(uint16_t vkCode, bool down)
{
    // use scan codes for layout-independent injection
    const UINT scan = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vkCode;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (!down) {
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool InputInjector::GetCursorPos(int& x, int& y)
{
    POINT pt;
    if (!::GetCursorPos(&pt)) {
        return false;
    }
    x = pt.x;
    y = pt.y;
    return true;
}

void InputInjector::ResetCursor()
{
}

bool InputInjector::PermissionGranted()
{
    return true;
}

bool InputInjector::IsOwnWindowAt(double, double) { return false; }
bool InputInjector::IsOwnAppFocused() { return false; }

std::string InputInjector::PermissionHint()
{
    return std::string();
}

} // namespace sdk
