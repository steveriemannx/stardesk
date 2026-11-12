// Non-Apple stub for AudioPlayback::Create(): the client is macOS, so on
// every other platform we simply return nullptr and audio frames are
// dropped by the session. Kept in src/platform/ so it is compiled on all
// non-Apple targets (the Apple implementation lives in platform/apple).
#if !defined(__APPLE__)

#include "AudioPlayback.h"

namespace sdk {

std::unique_ptr<AudioPlayback> AudioPlayback::Create()
{
    return nullptr;
}

} // namespace sdk

#endif // !defined(__APPLE__)
