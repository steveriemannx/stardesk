// macOS input injection via CGEventPost (HID event tap). Requires the
// Accessibility permission; without it events are silently dropped.

#import <CoreGraphics/CoreGraphics.h>
#import <Cocoa/Cocoa.h>

#include "../InputInject.h"
#include "../KeyMap.h"

namespace sdk {

namespace {

// Union of all screens in AppKit coordinates (bottom-left origin, main
// display at the bottom). CGEvent coordinates are CG (top-left) points, so
// the vertical axis must be converted.
void ScreenUnion(double& minX, double& minY, double& maxX, double& maxY)
{
    bool first = true;
    for (NSScreen* s in [NSScreen screens]) {
        const NSRect f = [s frame];
        if (first) {
            minX = f.origin.x; minY = f.origin.y;
            maxX = f.origin.x + f.size.width;
            maxY = f.origin.y + f.size.height;
            first = false;
        }
        else {
            minX = MIN(minX, f.origin.x);
            minY = MIN(minY, f.origin.y);
            maxX = MAX(maxX, f.origin.x + f.size.width);
            maxY = MAX(maxY, f.origin.y + f.size.height);
        }
    }
}

// The physical (point) geometry of the shared screen in CG top-left
// coordinates. The stream/logical size must NOT be used here: it is the
// captured pixel size (e.g. 2x Retina) while CGEvent positions are points.
void PhysicalScreenRect(double& x, double& y, double& w, double& h)
{
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    ScreenUnion(minX, minY, maxX, maxY);
    const double mainH = [[NSScreen mainScreen] frame].size.height;
    x = minX;                       // x is the same in both spaces
    y = mainH - maxY;               // top of the union in CG coordinates
    w = maxX - minX;
    h = maxY - minY;
}

} // namespace

bool InputInjector::MoveTo(double nx, double ny, int /*screenW*/, int /*screenH*/)
{
    double x0 = 0, y0 = 0, w = 0, h = 0;
    PhysicalScreenRect(x0, y0, w, h);
    const double x = x0 + nx * w;
    const double y = y0 + ny * h;
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                            CGPointMake(x, y), kCGMouseButtonLeft);
    if (ev == nullptr) {
        return false;
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    return true;
}

bool InputInjector::Button(bool down, int button)
{
    CGEventType type;
    CGMouseButton mb = kCGMouseButtonLeft;
    switch (button) {
    case 1: mb = kCGMouseButtonRight; break;
    case 2: mb = kCGMouseButtonCenter; break;
    default: mb = kCGMouseButtonLeft; break;
    }
    type = down ? (button == 1 ? kCGEventRightMouseDown
                               : (button == 2 ? kCGEventOtherMouseDown
                                              : kCGEventLeftMouseDown))
                : (button == 1 ? kCGEventRightMouseUp
                               : (button == 2 ? kCGEventOtherMouseUp
                                              : kCGEventLeftMouseUp));
    // position: the current cursor location. CGEventGetLocation spuriously
    // returns (0,0) on macOS 26 (clicks would land in the corner); NSEvent
    // is reliable. The move is injected before the button, so the cursor is
    // already at the target position.
    const NSPoint p = [NSEvent mouseLocation];
    const double mainH = [[NSScreen mainScreen] frame].size.height;
    const CGPoint loc = CGPointMake(p.x, mainH - p.y);
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, type, loc, mb);
    if (ev == nullptr) {
        return false;
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    return true;
}

bool InputInjector::MoveDelta(int, int) { return false; } // CGEventPost is absolute, no delta needed
bool InputInjector::GetTrackedPos(int&, int&) { return false; } // absolute positioning, no tracker
void InputInjector::ResetCursorTo(int, int) {} // absolute positioning, no tracker

bool InputInjector::Wheel(int deltaLines)
{
    CGEventRef ev = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1,
                                                  -deltaLines);
    if (ev == nullptr) {
        return false;
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    return true;
}

bool InputInjector::Key(uint16_t vkCode, bool down)
{
    const uint16_t macCode = KeyMap::ToMacVirtualKey(vkCode);
    if (macCode == 0xFFFF) {
        return false;
    }
    CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)macCode, down);
    if (ev == nullptr) {
        return false;
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    return true;
}

bool InputInjector::GetCursorPos(int& x, int& y)
{
    // NSEvent.mouseLocation is reliable; CGEventGetLocation spuriously
    // returns (0,0) on macOS 26 for this process (which would park the
    // reported cursor in the corner)
    const NSPoint p = [NSEvent mouseLocation];
    const double mainH = [[NSScreen mainScreen] frame].size.height;
    x = (int)p.x;
    y = (int)(mainH - p.y); // AppKit bottom-left -> CG top-left
    return true;
}

void InputInjector::ResetCursor()
{
    // macOS uses CGEvent absolute positions; there is no relative cursor
    // tracker to re-anchor on the client side.
}

bool InputInjector::PermissionGranted()
{
    return CGPreflightListenEventAccess() && CGPreflightPostEventAccess();
}

bool InputInjector::IsOwnWindowAt(double x, double y)
{
    // any StarDesk window (this or another instance) under the point - the
    // topmost check is unreliable because the Dock's desktop backdrop
    // reports a fullscreen window at the dock level
    CFArrayRef wins = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID);
    if (wins == NULL) {
        return false;
    }
    const CFIndex n = CFArrayGetCount(wins);
    bool ours = false;
    for (CFIndex i = 0; i < n; ++i) {
        NSDictionary* d = (NSDictionary*)CFArrayGetValueAtIndex(wins, i);
        CGRect b;
        if (!CGRectMakeWithDictionaryRepresentation(
                (CFDictionaryRef)d[(NSString*)kCGWindowBounds], &b)) {
            continue;
        }
        if (!CGRectContainsPoint(b, CGPointMake(x, y))) {
            continue;
        }
        NSString* o = d[(NSString*)kCGWindowOwnerName] ?: @"";
        if ([o localizedCaseInsensitiveCompare:@"stardesk"] == NSOrderedSame ||
            [o localizedCaseInsensitiveCompare:@"StarDesk"] == NSOrderedSame) {
            ours = true;
            break;
        }
    }
    CFRelease(wins);
    return ours;
}

bool InputInjector::IsOwnAppFocused()
{
    NSRunningApplication* front =
        [[NSWorkspace sharedWorkspace] frontmostApplication];
    if (front == nil) {
        return false;
    }
    NSString* n = front.localizedName ?: front.bundleIdentifier ?: @"";
    return [n.lowercaseString containsString:@"stardesk"];
}

std::string InputInjector::PermissionHint()
{
    return "accessibility permission not granted";
}

} // namespace sdk
