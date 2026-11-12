#ifndef STARDESK_PLATFORM_CAPTURER_H_
#define STARDESK_PLATFORM_CAPTURER_H_

#include "dui/dui.h"

#include <memory>
#include <string>
#include <vector>

namespace sdk {

/** One captured frame: RGBA (top-left origin, 4 bytes/pixel). */
struct CaptureFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

/** Primary-display screen capture, platform implementation in
 *  src/platform/<os>/Capturer_*.{mm,cpp}:
 *    - macOS: ScreenCaptureKit (SCScreenshotManager, macOS 14+; requires
 *      Screen Recording permission)
 *    - Windows: GDI BitBlt from the desktop DC
 *    - X11: XGetImage from the root window
 */
class Capturer {
public:
    Capturer();
    ~Capturer();

    /** Open the capture source. mode: false = mirror primary display,
     *  true = span all displays into one canvas. Returns false when the
     *  source is unavailable (e.g. missing permission); GetError() explains.
     */
    bool Start(bool extend);
    /** Try the portal ScreenCast stream (fast path); Start() falls back to
     *  portal Screenshot polling when this fails. */
    bool StartScreenCast(bool extend);
    /** GNOME/mutter direct ScreenCast (no portal, no dialog). */
    bool StartMetaCast(bool extend);
    /** Attach the pipewire consumer to m_impl->nodeId (shared setup). */
    bool SetupPipewire();
    /** Portal-Screenshot fallback capture (one PNG round-trip per frame). */
    bool ScreenshotCapture(CaptureFrame& out);
    /** Decode a portal screenshot file:// URI into an RGBA frame. */
    bool DecodeScreenshotFile(const std::string& uri, CaptureFrame& out);
    void Stop();

    /** Request an output resolution (0x0 = native). On macOS the capture
     *  switches to an SCStream configured to the largest aspect-preserving
     *  size that fits the box; used to capture at 720p/1080p directly
     *  instead of downscaling a full-resolution frame. */
    void SetTargetResolution(int width, int height);

    /** Grab one frame; returns false on transient failure (caller retries).
     *  On success `out` holds RGBA of GetWidth()xGetHeight(). */
    bool Capture(CaptureFrame& out);

    int GetWidth() const;
    int GetHeight() const;
    /** Origin of the captured area in physical screen coordinates
     *  (0,0 for the primary display; negative on multi-display layouts). */
    int GetOriginX() const;
    int GetOriginY() const;

    bool IsAvailable() const;
    /** Human-readable failure reason (permission hint etc.). */
    std::string GetError() const;

    /** True when the platform supports capturing the screen at all. */
    static bool PlatformSupported();

    // Platform implementation state (defined in the platform .mm/.cpp).
    // Public so the platform implementation can define and use it.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

/** Synthetic moving pattern used when the real screen cannot be captured
 *  (missing permission / unsupported platform), so the stream pipeline can
 *  still be exercised end to end.
 */
class TestPatternSource {
public:
    TestPatternSource();
    /** Returns a frame of the requested size; animates on every call. */
    bool Capture(CaptureFrame& out, int width, int height);

private:
    uint64_t m_tick = 0;
};

} // namespace sdk

#endif // STARDESK_PLATFORM_CAPTURER_H_
