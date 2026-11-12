// Audio playback abstraction (client side): receives 48kHz stereo S16 PCM
// and plays it. The Apple implementation uses CoreAudio's AudioQueue;
// on other platforms Create() returns nullptr and audio is silently
// dropped (the client is macOS, but the code compiles everywhere).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sdk {

class AudioPlayback {
public:
    virtual ~AudioPlayback() = default;

    /** Start the output device. frameSamples = PCM samples per audio frame
     *  (960 = 20ms at 48kHz). Returns false when audio output is
     *  unavailable. */
    virtual bool Start(int sampleRate, int channels, int frameSamples) = 0;

    /** Feed one decoded PCM frame (interleaved S16, frameSamples * channels
     *  values). Called from the session thread. */
    virtual void SubmitPcm(const int16_t* data, size_t sampleCount) = 0;

    virtual void Stop() = 0;

    /** Platform implementation; nullptr when unsupported. */
    static std::unique_ptr<AudioPlayback> Create();
};

} // namespace sdk
