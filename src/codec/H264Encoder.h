// H.264 encoder wrapper around libx264 for the host stream path.
// Compiled on Linux (links libx264); on other platforms the host falls back
// to the tile/PNG path. Output is AVCC-style (each NAL starts with its
// 4-byte big-endian length), which the macOS VideoToolbox decoder consumes
// directly.
#pragma once

#include <cstdint>
#include <vector>

struct x264_t;
struct x264_nal_t;

namespace sdk {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    /** width/height must be even (I420). Returns false if x264 init failed. */
    bool Init(int width, int height, int fps);

    /** Encode one RGBA frame; appends the AVCC NAL data to out. */
    bool Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out);

    /** Raw SPS/PPS bytes (no length prefix / start code) for VideoInit. */
    void GetSpsPps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const;

    int Width() const { return m_w; }
    int Height() const { return m_h; }

private:
    x264_t* m_enc = nullptr;
    int m_w = 0;
    int m_h = 0;
    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
    std::vector<uint8_t> m_i420; // RGBA -> I420 scratch
    void* m_picIn = nullptr;     // x264_picture_t
    void* m_picOut = nullptr;    // x264_picture_t
};

} // namespace sdk
