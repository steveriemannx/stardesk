// Video decoder for the client (macOS VideoToolbox hardware decode of
// H.264 / VP9 / AV1). Input is codec-specific frame data plus (H.264 only)
// the SPS/PPS from VideoInit. Output is RGBA (swizzled from the decoder's
// BGRA) so the rest of the pipeline is unchanged. Non-Apple builds link a
// stub (Init fails -> the session stays on the tile/PNG path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdk {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    struct Impl; // pimpl (defined in the platform .mm)

    /**
     * codec: 1 = H.264 (sps/pps required), 2 = VP9, 3 = AV1.
     * Returns false on error (e.g. non-macOS, unsupported codec).
     */
    bool Init(int codec, const uint8_t* sps, size_t spsLen,
              const uint8_t* pps, size_t ppsLen, int width, int height);

    /** Decode one frame synchronously; fills out (RGBA, w*h*4). */
    bool Decode(const uint8_t* data, size_t len, std::vector<uint8_t>& out,
                int& w, int& h);

private:
    Impl* m_impl;
};

} // namespace sdk
