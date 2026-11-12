// X11 screen capture via MIT-SHM (XShm) with coarse-hash change detection.
// (X11 session only; Wayland is not supported - see README.)
// Mirror mode: the primary screen area; extend mode: the root window, which
// under Xinerama already spans all monitors.
//
// Why XShm: the previous XGetImage + per-pixel XGetPixel path made a
// function call per pixel (~1.3M calls per 1440x900 frame) and shipped the
// whole framebuffer over the X protocol. XShmGetImage copies the root window
// straight into shared memory, and the pixels are read directly from memory.
//
// Change detection: an XDamage-based skip was tried first, but the Xfce
// compositor does not reliably deliver DamageNotify for window content
// changes on the root, so animated scenes were wrongly skipped (1fps).
// Instead, Capture() hashes a coarse stride-4 sample of the raw pixels
// (a few hundred microseconds) and skips the encode/send pipeline when the
// screen is unchanged. A 1s force-capture fallback catches sub-stride
// changes (blinking cursors etc.).

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <chrono>
#include <cstdint>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "../Capturer.h"

namespace sdk {

namespace {

// Top-down RGBA copy of an XImage, reading the pixel data directly from
// memory instead of calling XGetPixel per pixel (the previous path made a
// function call per pixel, ~1.3M per 1440x900 frame).
void ConvertPixels(XImage* img, uint8_t* rgba)
{
    const int w = img->width;
    const int h = img->height;
    const int bpp = img->bits_per_pixel;
    const size_t bpl = (size_t)img->bytes_per_line;
    const uint8_t* data = (const uint8_t*)img->data;
    const unsigned long rM = img->red_mask;
    const unsigned long gM = img->green_mask;
    const unsigned long bM = img->blue_mask;
    const int rs = __builtin_ctzl(rM);
    const int gs = __builtin_ctzl(gM);
    const int bs = __builtin_ctzl(bM);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = data + (size_t)y * bpl;
        uint8_t* dst = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            unsigned long px = 0;
            if (bpp == 32) {
                px = *(const uint32_t*)(row + (size_t)x * 4);
            }
            else if (bpp == 24) {
                const uint8_t* p = row + (size_t)x * 3;
                px = (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                     ((unsigned long)p[2] << 16);
            }
            else if (bpp == 16) {
                px = *(const uint16_t*)(row + (size_t)x * 2);
            }
            else {
                px = XGetPixel(img, x, y); // rare depths: fall back
            }
            dst[x * 4 + 0] = (uint8_t)((px & rM) >> rs);
            dst[x * 4 + 1] = (uint8_t)((px & gM) >> gs);
            dst[x * 4 + 2] = (uint8_t)((px & bM) >> bs);
            dst[x * 4 + 3] = 255;
        }
    }
}

// FNV-1a over a stride-4 sample of the raw pixel bytes. Cheap (~0.2ms for
// 1440x900) and reliable enough to detect any visible screen change.
uint64_t CoarseHashImage(const XImage* img)
{
    const int w = img->width;
    const int h = img->height;
    const size_t bpl = (size_t)img->bytes_per_line;
    const int bpp = img->bits_per_pixel / 8;
    const uint8_t* data = (const uint8_t*)img->data;
    const int step = 4;
    uint64_t hash = 1469598103934665603ull; // FNV-1a offset basis
    for (int y = 0; y < h; y += step) {
        const uint8_t* row = data + (size_t)y * bpl;
        for (int x = 0; x < w; x += step) {
            const uint8_t* p = row + (size_t)x * bpp;
            hash ^= p[0];
            hash *= 1099511628211ull;
            hash ^= p[1];
            hash *= 1099511628211ull;
            hash ^= p[2];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

} // namespace

struct Capturer::Impl {
    Display* display = nullptr;
    Window root = 0;
    bool available = false;
    bool started = false;
    std::string error;
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
    bool extend = false;

    // MIT-SHM fast capture
    bool shm = false;
    XShmSegmentInfo shmInfo {};
    XImage* image = nullptr;

    // coarse-hash change detection
    bool hasFrame = false;
    uint64_t lastHash = 0;
    std::chrono::steady_clock::time_point lastCaptureAt;
};

Capturer::Capturer() : m_impl(new Impl()) {}

Capturer::~Capturer()
{
    Stop();
}

bool Capturer::PlatformSupported()
{
    Display* d = XOpenDisplay(nullptr);
    if (d == nullptr) {
        return false;
    }
    XCloseDisplay(d);
    return true;
}

bool Capturer::Start(bool extend)
{
    m_impl->error.clear();
    m_impl->display = XOpenDisplay(nullptr);
    if (m_impl->display == nullptr) {
        m_impl->error = "cannot open X display (Wayland sessions are not supported)";
        return false;
    }
    m_impl->extend = extend;
    const int screen = DefaultScreen(m_impl->display);
    m_impl->root = RootWindow(m_impl->display, screen);
    // extend: root spans all monitors (Xinerama); mirror: primary screen size
    m_impl->width = DisplayWidth(m_impl->display, screen);
    m_impl->height = DisplayHeight(m_impl->display, screen);

    // ---- MIT-SHM ----
    if (XShmQueryExtension(m_impl->display)) {
        XImage* img = XShmCreateImage(m_impl->display,
                                      DefaultVisual(m_impl->display, screen),
                                      DefaultDepth(m_impl->display, screen),
                                      ZPixmap, nullptr, &m_impl->shmInfo,
                                      m_impl->width, m_impl->height);
        if (img != nullptr) {
            m_impl->shmInfo.shmid = shmget(IPC_PRIVATE,
                                           (size_t)img->bytes_per_line * img->height,
                                           IPC_CREAT | 0600);
            if (m_impl->shmInfo.shmid >= 0) {
                m_impl->shmInfo.shmaddr = (char*)shmat(m_impl->shmInfo.shmid, nullptr, 0);
                m_impl->shmInfo.readOnly = False;
                if (m_impl->shmInfo.shmaddr != (char*)-1 &&
                    XShmAttach(m_impl->display, &m_impl->shmInfo)) {
                    // XShmCreateImage set obdata = &shmInfo, so XDestroyImage
                    // will NOT XFree the shared memory pointer
                    img->data = m_impl->shmInfo.shmaddr;
                    m_impl->image = img;
                    m_impl->shm = true;
                    XSync(m_impl->display, False);
                }
                else {
                    if (m_impl->shmInfo.shmaddr != (char*)-1) {
                        shmdt(m_impl->shmInfo.shmaddr);
                    }
                    shmctl(m_impl->shmInfo.shmid, IPC_RMID, nullptr);
                    XDestroyImage(img);
                }
            }
            else {
                XDestroyImage(img);
            }
        }
    }

    m_impl->available = true;
    m_impl->started = true;
    return true;
}

void Capturer::Stop()
{
    m_impl->started = false;
    if (m_impl->display != nullptr) {
        if (m_impl->shm) {
            XShmDetach(m_impl->display, &m_impl->shmInfo);
            XDestroyImage(m_impl->image); // obdata set: does not free shmaddr
            shmdt(m_impl->shmInfo.shmaddr);
            shmctl(m_impl->shmInfo.shmid, IPC_RMID, nullptr);
        }
        XCloseDisplay(m_impl->display);
        m_impl->display = nullptr;
    }
    m_impl->shm = false;
    m_impl->image = nullptr;
    m_impl->hasFrame = false;
}

bool Capturer::Capture(CaptureFrame& out)
{
    if (!m_impl->available || !m_impl->started || m_impl->display == nullptr) {
        return false;
    }

    // ---- grab the raw pixels (fast path: shared memory) ----
    XImage* img = nullptr;
    bool ownImage = false; // true when the XGetImage fallback must be freed
    if (m_impl->shm) {
        if (XShmGetImage(m_impl->display, m_impl->root, m_impl->image, 0, 0,
                         AllPlanes)) {
            img = m_impl->image;
        }
    }
    else {
        img = XGetImage(m_impl->display, m_impl->root, 0, 0, m_impl->width,
                        m_impl->height, AllPlanes, ZPixmap);
        ownImage = img != nullptr;
    }
    if (img == nullptr) {
        return false;
    }

    // ---- coarse change detection: skip encode/send when unchanged ----
    const uint64_t hash = CoarseHashImage(img);
    const auto now = std::chrono::steady_clock::now();
    bool skip = false;
    if (m_impl->hasFrame && hash == m_impl->lastHash &&
        now - m_impl->lastCaptureAt < std::chrono::seconds(1)) {
        skip = true;
    }
    if (skip) {
        if (ownImage) {
            XDestroyImage(img);
        }
        return false;
    }

    // ---- changed: convert and publish ----
    out.width = m_impl->width;
    out.height = m_impl->height;
    out.rgba.resize((size_t)m_impl->width * m_impl->height * 4);
    ConvertPixels(img, out.rgba.data());
    if (ownImage) {
        XDestroyImage(img);
    }

    m_impl->hasFrame = true;
    m_impl->lastHash = hash;
    m_impl->lastCaptureAt = now;
    return true;
}

// The SCStream-based resolution switching is macOS-only; other platforms
// capture at the native resolution and the host downscales when needed.
void Capturer::SetTargetResolution(int /*width*/, int /*height*/)
{
}

int Capturer::GetWidth() const { return m_impl->width; }
int Capturer::GetHeight() const { return m_impl->height; }
int Capturer::GetOriginX() const { return m_impl->originX; }
int Capturer::GetOriginY() const { return m_impl->originY; }
bool Capturer::CursorEmbedded() const { return false; } // XGetImage: no pointer in the frame
void Capturer::SetHideCursor(bool) {} // XGetImage: cursor not in the frame either way
bool Capturer::GetCursor(int&, int&, bool&) const { return false; } // InputInjector covers it
bool Capturer::IsAvailable() const { return m_impl->available; }
std::string Capturer::GetError() const { return m_impl->error; }

} // namespace sdk
