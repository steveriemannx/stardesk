// VP9 encoder wrapper around Google's libvpx (BSD-3-Clause). Same interface
// as H264Encoder; no parameter sets (VP9 keyframes are self-contained), so
// GetSpsPps is not needed and VideoInit carries no sps/pps for this codec.
#pragma once

#include <cstdint>
#include <vector>

namespace sdk {

class VpxEncoder {
public:
    VpxEncoder() = default;
    ~VpxEncoder();

    VpxEncoder(const VpxEncoder&) = delete;
    VpxEncoder& operator=(const VpxEncoder&) = delete;

    bool Init(int width, int height, int fps);
    bool Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out);

    int Width() const { return m_w; }
    int Height() const { return m_h; }

private:
    void* m_ctx = nullptr; // vpx_codec_ctx_t
    int m_w = 0;
    int m_h = 0;
    unsigned int m_frameIndex = 0;
    std::vector<uint8_t> m_i420;
};

} // namespace sdk
