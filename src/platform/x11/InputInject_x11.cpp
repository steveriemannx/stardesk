// X11 input injection via the XTest extension (X11 session only).

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "../InputInject.h"
#include "../KeyMap.h"

namespace sdk {

namespace {

Display* GetDisplay()
{
    static Display* s_display = XOpenDisplay(nullptr);
    return s_display;
}

} // namespace

bool InputInjector::MoveTo(double nx, double ny, int screenW, int screenH)
{
    Display* d = GetDisplay();
    if (d == nullptr) {
        return false;
    }
    const int x = (int)(nx * screenW);
    const int y = (int)(ny * screenH);
    XTestFakeMotionEvent(d, DefaultScreen(d), x, y, CurrentTime);
    XFlush(d);
    return true;
}

bool InputInjector::Button(bool down, int button)
{
    Display* d = GetDisplay();
    if (d == nullptr) {
        return false;
    }
    unsigned int btn = 1; // left
    switch (button) {
    case 1: btn = 3; break; // right
    case 2: btn = 2; break; // middle
    default: break;
    }
    XTestFakeButtonEvent(d, btn, down ? True : False, CurrentTime);
    XFlush(d);
    return true;
}

bool InputInjector::MoveDelta(int, int) { return false; } // absolute motion, no delta needed
bool InputInjector::GetTrackedPos(int&, int&) { return false; } // absolute positioning, no tracker
void InputInjector::ResetCursorTo(int, int) {} // absolute positioning, no tracker

bool InputInjector::Wheel(int deltaLines)
{
    Display* d = GetDisplay();
    if (d == nullptr) {
        return false;
    }
    const unsigned int upBtn = 4;   // X11 wheel up
    const unsigned int downBtn = 5; // X11 wheel down
    const unsigned int btn = deltaLines >= 0 ? upBtn : downBtn;
    const int clicks = deltaLines >= 0 ? deltaLines : -deltaLines;
    for (int i = 0; i < clicks; ++i) {
        XTestFakeButtonEvent(d, btn, True, CurrentTime);
        XTestFakeButtonEvent(d, btn, False, CurrentTime);
    }
    XFlush(d);
    return true;
}

bool InputInjector::Key(uint16_t vkCode, bool down)
{
    Display* d = GetDisplay();
    if (d == nullptr) {
        return false;
    }
    const uint32_t sym = KeyMap::ToX11KeySym(vkCode);
    if (sym == 0) {
        return false;
    }
    const KeyCode code = XKeysymToKeycode(d, (KeySym)sym);
    if (code == 0) {
        return false;
    }
    XTestFakeKeyEvent(d, code, down ? True : False, CurrentTime);
    XFlush(d);
    return true;
}

bool InputInjector::GetCursorPos(int& x, int& y)
{
    Display* d = GetDisplay();
    if (d == nullptr) {
        return false;
    }
    Window root = DefaultRootWindow(d);
    Window child;
    int rx = 0, ry = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(d, root, &root, &child, &x, &y, &rx, &ry, &mask)) {
        return false;
    }
    return true;
}

void InputInjector::ResetCursor()
{
}

bool InputInjector::PermissionGranted()
{
    return GetDisplay() != nullptr;
}

bool InputInjector::IsOwnWindowAt(double, double) { return false; }
bool InputInjector::IsOwnAppFocused() { return false; }

std::string InputInjector::PermissionHint()
{
    return GetDisplay() == nullptr ? "no X display" : std::string();
}

} // namespace sdk
