#include "Capturer.h"

namespace sdk {

// The platform implementation (Capturer_mac.mm / Capturer_win.cpp /
// Capturer_x11.cpp) defines the Capturer::Impl struct and these methods.
// This translation unit only implements the shared TestPatternSource.

TestPatternSource::TestPatternSource() = default;

bool TestPatternSource::Capture(CaptureFrame& out, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign((size_t)width * height * 4, 0);

    const uint64_t t = m_tick++;

    // moving gradient background
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &out.rgba[(size_t)(y * width + x) * 4];
            p[0] = (uint8_t)((x + t / 2) & 0xFF);
            p[1] = (uint8_t)((y + t) & 0xFF);
            p[2] = (uint8_t)((x + y + t / 3) & 0xFF);
            p[3] = 255;
        }
    }

    // moving block
    const int block = width / 5;
    const int bx = (int)((t * (width / 64)) % (width - block));
    const int by = (int)((t * (height / 48)) % (height - block));
    for (int y = by; y < by + block && y < height; ++y) {
        for (int x = bx; x < bx + block && x < width; ++x) {
            uint8_t* p = &out.rgba[(size_t)(y * width + x) * 4];
            p[0] = 0xFF;
            p[1] = 0xC0;
            p[2] = 0x40;
            p[3] = 255;
        }
    }

    // frame border
    for (int x = 0; x < width; ++x) {
        out.rgba[(size_t)x * 4] = 255;
        out.rgba[(size_t)((height - 1) * width + x) * 4] = 255;
    }
    for (int y = 0; y < height; ++y) {
        out.rgba[(size_t)(y * width) * 4 + 2] = 255;
        out.rgba[(size_t)(y * width + width - 1) * 4 + 2] = 255;
    }

    // blinking center dot
    if ((t / 8) % 2 == 0) {
        const int cx = width / 2, cy = height / 2;
        for (int y = cy - 6; y <= cy + 6; ++y) {
            for (int x = cx - 6; x <= cx + 6; ++x) {
                if (y >= 0 && y < height && x >= 0 && x < width) {
                    uint8_t* p = &out.rgba[(size_t)(y * width + x) * 4];
                    p[0] = p[1] = p[2] = 0;
                    p[3] = 255;
                }
            }
        }
    }
    return true;
}

} // namespace sdk
