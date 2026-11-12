// H.264 encoder wrapper around Cisco OpenH264 (BSD-2-Clause) for the host
// stream path. Built from the Debian/Ubuntu libopenh264-dev package (the
// "dfsg" repackage - clean BSD source, no Cisco binary patent-license
// wrapper). Output is AVCC-style (each NAL starts with its 4-byte big-endian
// length), which the macOS VideoToolbox decoder consumes directly - the same
// as the x264-based codec 1, but license-clean.
#pragma once

#include <cstdint>
#include <vector>

class ISVCEncoder; // Cisco OpenH264 (global namespace, from wels/codec_api.h)

namespace sdk {

class OpenH264Encoder {
public:
    OpenH264Encoder() = default;
    ~OpenH264Encoder();

    OpenH264Encoder(const OpenH264Encoder&) = delete;
    OpenH264Encoder& operator=(const OpenH264Encoder&) = delete;

    /** width/height must be even (I420). Encodes a dummy frame internally to
     *  obtain SPS/PPS (OpenH264 has no "get headers" API). */
    bool Init(int width, int height, int fps);

    /** Encode one RGBA frame; appends the AVCC NAL data to out (SPS/PPS are
     *  stripped - they go out via VideoInit). */
    bool Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out);

    /** Raw SPS/PPS bytes (no length prefix / start code) for VideoInit. */
    void GetSpsPps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const;

    /** Force an IDR on the next Encode (after a live codec switch the client
     *  re-creates its decoder and needs a keyframe to start). */
    void RequestKeyFrame();

    int Width() const { return m_w; }
    int Height() const { return m_h; }

private:
    ISVCEncoder* m_enc = nullptr;
    int m_w = 0;
    int m_h = 0;
    bool m_needIdr = true; // force the first real frame (and re-negotiations) to be an IDR
    int m_idrForceCount = 0;
    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
    std::vector<uint8_t> m_i420; // RGBA -> I420 scratch
};

} // namespace sdk
