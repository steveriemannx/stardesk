#ifndef STARDESK_PLATFORM_INPUT_INJECT_H_
#define STARDESK_PLATFORM_INPUT_INJECT_H_

#include <cstdint>
#include <string>

namespace sdk {

/** Remote input injection, platform implementation in
 *  src/platform/<os>/InputInject_*.{mm,cpp}:
 *    - macOS: CGEventPost (requires Accessibility permission)
 *    - Windows: SendInput
 *    - X11: XTest (XTestFakeMotionEvent/ButtonEvent/KeyEvent)
 */
class InputInjector {
public:
    /** Move the pointer to normalized coordinates (0..1) within the shared
     *  logical screen (screenW x screenH). */
    static bool MoveTo(double nx, double ny, int screenW, int screenH);
    /** Press/release a mouse button: 0=left, 1=right, 2=middle. */
    static bool Button(bool down, int button);
    /** Apply an explicit pointer delta (Wayland/EIS only). Used before a
     *  button so the click lands at the exact client-reported coordinates
     *  even while the relative-motion queue is still draining (the real
     *  pointer lags the tracked target; the delta bridges the gap and the
     *  in-order click fires right after the pointer settles). */
    static bool MoveDelta(int dx, int dy);
    /** Last position WE commanded (the relative-motion tracker's position,
     *  in the same physical coordinates the injector maps into). Only the
     *  Wayland/EIS path tracks a position; other platforms return false. */
    static bool GetTrackedPos(int& x, int& y);
    /** Scroll wheel; positive deltaLines scrolls up. */
    static bool Wheel(int deltaLines);
    /** Key down/up by dui VirtualKeyCode (kVK_*). */
    static bool Key(uint16_t vkCode, bool down);

    /** Current pointer position in physical screen coordinates. */
    static bool GetCursorPos(int& x, int& y);

    /** Forget the tracked position (no pointer motion); the next
     *  ResetCursorTo() re-calibrates it against the real cursor. Call when a
     *  new client connects so a stale tracked position from a previous
     *  session cannot shift the pointer. */
    static void ResetCursor();

    /** Calibrate the injector's tracked position to the compositor's REAL
     *  pointer position (no pointer motion). Before this the tracked position
     *  is a guess and every relative motion would land offset by the
     *  difference. Called by the host with the capture's cursor metadata. */
    static void ResetCursorTo(int x, int y);

    /** True when injection is permitted (macOS Accessibility etc.). */
    static bool PermissionGranted();
    /** Human-readable permission hint for the UI. */
    static std::string PermissionHint();

    /** True when a StarDesk window (any instance) is under the physical
     *  point (x, y). The host skips injection there to break the
     *  same-machine feedback loop: a click aimed into the remote-view
     *  window would land on it and re-trigger an input event. Other
     *  platforms: false. */
    static bool IsOwnWindowAt(double x, double y);
    /** True when a StarDesk app is the focused/frontmost app (keys would
     *  echo back through the remote-view window). Other platforms: false. */
    static bool IsOwnAppFocused();
};

} // namespace sdk

#endif // STARDESK_PLATFORM_INPUT_INJECT_H_
