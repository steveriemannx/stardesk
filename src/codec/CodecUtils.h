// Shared helpers for the video encoders (x264 / libvpx / libaom).
#pragma once

#include <cstdint>

namespace sdk {

// ITU-R BT.601 limited-range RGBA -> I420.
// vtSwap: VideoToolbox's H.264 decode-to-BGRA has a documented U/V (Cb/Cr)
// swap on some macOS versions, which shows up as swapped red/blue. The H.264
// encoder compensates by setting vtSwap=true. VP9/AV1 are decoded with
// standard software decoders (libvpx/libaom) on the client, so those
// encoders use vtSwap=false.
inline void RgbaToI420(const uint8_t* rgba, int w, int h, uint8_t* yPlane, int yStride,
                       uint8_t* uPlane, int uStride, uint8_t* vPlane, int vStride,
                       bool vtSwap = false)
{
    const int h2 = h / 2;
    const int w2 = w / 2;
    for (int yy = 0; yy < h2; ++yy) {
        const int y0 = yy * 2;
        const int y1 = y0 + 1;
        const uint8_t* r0 = rgba + (size_t)y0 * w * 4;
        const uint8_t* r1 = rgba + (size_t)y1 * w * 4;
        uint8_t* yp0 = yPlane + (size_t)y0 * yStride;
        uint8_t* yp1 = yPlane + (size_t)y1 * yStride;
        uint8_t* up = uPlane + (size_t)yy * uStride;
        uint8_t* vp = vPlane + (size_t)yy * vStride;
        for (int x = 0; x < w2; ++x) {
            const int x0 = x * 2;
            const int x1 = x0 + 1;
            const uint8_t* p00 = r0 + (size_t)x0 * 4;
            const uint8_t* p01 = r0 + (size_t)x1 * 4;
            const uint8_t* p10 = r1 + (size_t)x0 * 4;
            const uint8_t* p11 = r1 + (size_t)x1 * 4;
            const int rSum = p00[0] + p01[0] + p10[0] + p11[0];
            const int gSum = p00[1] + p01[1] + p10[1] + p11[1];
            const int bSum = p00[2] + p01[2] + p10[2] + p11[2];
            const int y00 = ((66 * p00[0] + 129 * p00[1] + 25 * p00[2] + 128) >> 8) + 16;
            const int y01 = ((66 * p01[0] + 129 * p01[1] + 25 * p01[2] + 128) >> 8) + 16;
            const int y10 = ((66 * p10[0] + 129 * p10[1] + 25 * p10[2] + 128) >> 8) + 16;
            const int y11 = ((66 * p11[0] + 129 * p11[1] + 25 * p11[2] + 128) >> 8) + 16;
            yp0[x0] = (uint8_t)y00;
            yp0[x1] = (uint8_t)y01;
            yp1[x0] = (uint8_t)y10;
            yp1[x1] = (uint8_t)y11;
            const int r = (rSum + 2) >> 2;
            const int g = (gSum + 2) >> 2;
            const int b = (bSum + 2) >> 2;
            if (vtSwap) { // VT chroma-swap compensation (see header)
                up[x] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                vp[x] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            }
            else { // standard Cb/Cr
                up[x] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                vp[x] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
        if ((w & 1) != 0) {
            const int x = w - 1;
            const uint8_t* p0 = r0 + (size_t)x * 4;
            const uint8_t* p1 = r1 + (size_t)x * 4;
            yp0[x] = (uint8_t)(((66 * p0[0] + 129 * p0[1] + 25 * p0[2] + 128) >> 8) + 16);
            yp1[x] = (uint8_t)(((66 * p1[0] + 129 * p1[1] + 25 * p1[2] + 128) >> 8) + 16);
            const int r = (p0[0] + p1[0] + 1) >> 1;
            const int g = (p0[1] + p1[1] + 1) >> 1;
            const int b = (p0[2] + p1[2] + 1) >> 1;
            if (vtSwap) {
                up[x / 2] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                vp[x / 2] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            }
            else {
                up[x / 2] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                vp[x / 2] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    if ((h & 1) != 0) {
        const int y = h - 1;
        const uint8_t* r0 = rgba + (size_t)y * w * 4;
        uint8_t* yp = yPlane + (size_t)y * yStride;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = r0 + (size_t)x * 4;
            yp[x] = (uint8_t)(((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16);
        }
    }
}

// end RgbaToI420

// ITU-R BT.601 limited-range I420 -> RGBA (client software decode).
// The int math can go slightly out of [0,255] (esp. at chroma range edges);
// clamp instead of letting the uint8 cast wrap (wrapping turned pure red
// into magenta/blue).
inline uint8_t Clamp255(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline void I420ToRgba(const uint8_t* yPlane, int yStride,
                       const uint8_t* uPlane, int uStride,
                       const uint8_t* vPlane, int vStride,
                       int w, int h, uint8_t* rgba)
{
    const int h2 = h / 2;
    const int w2 = w / 2;
    for (int yy = 0; yy < h2; ++yy) {
        const int y0 = yy * 2;
        const int y1 = y0 + 1;
        const uint8_t* yp0 = yPlane + (size_t)y0 * yStride;
        const uint8_t* yp1 = yPlane + (size_t)y1 * yStride;
        const uint8_t* up = uPlane + (size_t)yy * uStride;
        const uint8_t* vp = vPlane + (size_t)yy * vStride;
        uint8_t* d0 = rgba + (size_t)y0 * w * 4;
        uint8_t* d1 = rgba + (size_t)y1 * w * 4;
        for (int x = 0; x < w2; ++x) {
            const int d = up[x] - 128;
            const int e = vp[x] - 128;
            const int c0 = yp0[x * 2] - 16;
            const int c1 = yp0[x * 2 + 1] - 16;
            const int c2 = yp1[x * 2] - 16;
            const int c3 = yp1[x * 2 + 1] - 16;
            // each iteration writes TWO pixels -> advance 8 bytes, not 4
            // (using x*4 overlapped each pair and never wrote the right
            // half: the output looked horizontally compressed with a black
            // right half)
            d0[x * 8 + 0] = Clamp255((298 * c0 + 409 * e + 128) >> 8);
            d0[x * 8 + 1] = Clamp255((298 * c0 - 100 * d - 208 * e + 128) >> 8);
            d0[x * 8 + 2] = Clamp255((298 * c0 + 516 * d + 128) >> 8);
            d0[x * 8 + 3] = 255;
            d0[x * 8 + 4] = Clamp255((298 * c1 + 409 * e + 128) >> 8);
            d0[x * 8 + 5] = Clamp255((298 * c1 - 100 * d - 208 * e + 128) >> 8);
            d0[x * 8 + 6] = Clamp255((298 * c1 + 516 * d + 128) >> 8);
            d0[x * 8 + 7] = 255;
            d1[x * 8 + 0] = Clamp255((298 * c2 + 409 * e + 128) >> 8);
            d1[x * 8 + 1] = Clamp255((298 * c2 - 100 * d - 208 * e + 128) >> 8);
            d1[x * 8 + 2] = Clamp255((298 * c2 + 516 * d + 128) >> 8);
            d1[x * 8 + 3] = 255;
            d1[x * 8 + 4] = Clamp255((298 * c3 + 409 * e + 128) >> 8);
            d1[x * 8 + 5] = Clamp255((298 * c3 - 100 * d - 208 * e + 128) >> 8);
            d1[x * 8 + 6] = Clamp255((298 * c3 + 516 * d + 128) >> 8);
            d1[x * 8 + 7] = 255;
        }
        if ((w & 1) != 0) {
            const int x = w - 1;
            const int c = yp0[x] - 16;
            const int d = up[x / 2] - 128;
            const int e = vp[x / 2] - 128;
            d0[x * 4 + 0] = Clamp255((298 * c + 409 * e + 128) >> 8);
            d0[x * 4 + 1] = Clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
            d0[x * 4 + 2] = Clamp255((298 * c + 516 * d + 128) >> 8);
            d0[x * 4 + 3] = 255;
        }
    }
    if ((h & 1) != 0) {
        const int y = h - 1;
        const uint8_t* yp = yPlane + (size_t)y * yStride;
        uint8_t* d = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const int c = yp[x] - 16;
            const int uu = uPlane[(size_t)(y / 2) * uStride + x / 2] - 128;
            const int vv = vPlane[(size_t)(y / 2) * vStride + x / 2] - 128;
            d[x * 4 + 0] = Clamp255((298 * c + 409 * vv + 128) >> 8);
            d[x * 4 + 1] = Clamp255((298 * c - 100 * uu - 208 * vv + 128) >> 8);
            d[x * 4 + 2] = Clamp255((298 * c + 516 * uu + 128) >> 8);
            d[x * 4 + 3] = 255;
        }
    }
}

} // namespace sdk
