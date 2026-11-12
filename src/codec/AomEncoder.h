// AV1 encoder wrapper around AOM's libaom (BSD-2-Clause). Same interface as
// H264Encoder; AV1 sequence headers ride in the keyframe, so VideoInit
// carries no parameter sets for this codec.
#pragma once

#include <cstdint>
#include <vector>

namespace sdk {

class AomEncoder {
public:
    AomEncoder() = default;
    ~AomEncoder();

    AomEncoder(const AomEncoder&) = delete;
    AomEncoder& operator=(const AomEncoder&) = delete;

    bool Init(int width, int height, int fps);
    bool Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out);

    int Width() const { return m_w; }
    int Height() const { return m_h; }

private:
    void* m_ctx = nullptr; // aom_codec_ctx_t
    int m_w = 0;
    int m_h = 0;
    unsigned int m_frameIndex = 0;
    std::vector<uint8_t> m_i420;
};

} // namespace sdk
