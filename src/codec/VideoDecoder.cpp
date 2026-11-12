// Non-Apple stub for VideoDecoder: VideoToolbox only exists on macOS. The
// client requests video codecs only on macOS, so on other platforms Init()
// fails and the session stays on the tile/PNG path.
#if !defined(__APPLE__)

#include "VideoDecoder.h"

namespace sdk {

VideoDecoder::VideoDecoder() : m_impl(nullptr) {}
VideoDecoder::~VideoDecoder() {}
bool VideoDecoder::Init(int, const uint8_t*, size_t, const uint8_t*, size_t, int, int)
{
    return false;
}
bool VideoDecoder::Decode(const uint8_t*, size_t, std::vector<uint8_t>&, int&, int&)
{
    return false;
}

} // namespace sdk

#endif // !__APPLE__
