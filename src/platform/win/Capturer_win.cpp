// Windows screen capture via GDI BitBlt from the desktop DC.
// Mirror mode: primary display; extend mode: the virtual screen (all displays).

#include <windows.h>
#include <wingdi.h>

#include "../Capturer.h"

namespace sdk {

struct Capturer::Impl {
    bool available = false;
    bool started = false;
    std::string error;
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
};

Capturer::Capturer() : m_impl(new Impl()) {}

Capturer::~Capturer()
{
    Stop();
}

bool Capturer::PlatformSupported()
{
    return true;
}

bool Capturer::Start(bool extend)
{
    m_impl->error.clear();
    if (extend) {
        m_impl->originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_impl->originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_impl->width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_impl->height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    else {
        m_impl->originX = 0;
        m_impl->originY = 0;
        m_impl->width = GetSystemMetrics(SM_CXSCREEN);
        m_impl->height = GetSystemMetrics(SM_CYSCREEN);
    }
    if (m_impl->width <= 0 || m_impl->height <= 0) {
        m_impl->error = "no screen";
        return false;
    }
    m_impl->available = true;
    m_impl->started = true;
    return true;
}

void Capturer::Stop()
{
    m_impl->started = false;
}

bool Capturer::Capture(CaptureFrame& out)
{
    if (!m_impl->available || !m_impl->started) {
        return false;
    }
    const int w = m_impl->width;
    const int h = m_impl->height;

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr) {
        return false;
    }
    HDC memDC = CreateCompatibleDC(screenDC);
    if (memDC == nullptr) {
        ReleaseDC(nullptr, screenDC);
        return false;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return false;
    }
    HGDIOBJ old = SelectObject(memDC, dib);
    const BOOL ok = BitBlt(memDC, 0, 0, w, h, screenDC,
                           m_impl->originX, m_impl->originY, SRCCOPY | CAPTUREBLT);
    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    if (!ok) {
        return false;
    }

    // DIB is BGRA; convert to RGBA
    out.width = w;
    out.height = h;
    out.rgba.resize((size_t)w * h * 4);
    const uint8_t* src = (const uint8_t*)bits;
    uint8_t* dst = out.rgba.data();
    for (int i = 0; i < w * h; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2]; // R
        dst[i * 4 + 1] = src[i * 4 + 1]; // G
        dst[i * 4 + 2] = src[i * 4 + 0]; // B
        dst[i * 4 + 3] = 255;
    }
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
bool Capturer::CursorEmbedded() const { return false; } // GDI blit: no pointer in the frame
void Capturer::SetHideCursor(bool) {} // GDI capture: cursor not in the frame either way
bool Capturer::GetCursor(int&, int&, bool&) const { return false; } // InputInjector covers it
bool Capturer::IsAvailable() const { return m_impl->available; }
std::string Capturer::GetError() const { return m_impl->error; }

} // namespace sdk
