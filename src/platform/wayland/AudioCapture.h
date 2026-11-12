// Desktop audio capture for the Wayland host: records the default audio
// sink's monitor port through PipeWire (so whatever the remote desktop
// plays is captured), encodes it to Opus (20ms packets), and delivers each
// packet via a callback. Compiled on Wayland builds with libopus.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sdk {

class AudioCapture {
public:
    /** One encoded Opus packet (20ms at 48kHz). Called from the capture
     *  thread; keep it fast (the packet is tiny). */
    using PacketCallback = std::function<void(const uint8_t* data, size_t len)>;

    struct Impl; // pimpl, defined in AudioCapture_wayland.cpp

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /** Start capturing the default sink monitor. sampleRate/channels/
     *  frameSamples describe the Opus stream (48kHz stereo 960 = 20ms).
     *  Returns false when PipeWire or the audio sink is unavailable (the
     *  PipeWire setup runs on the capture thread; this blocks up to ~8s). */
    bool Start(int sampleRate, int channels, int frameSamples,
               PacketCallback cb);

    /** Stop and join the capture thread. */
    void Stop();

    bool Active() const { return m_running.load(); }

private:
    void LoopMain();

    std::unique_ptr<Impl> m_impl;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    // setup completion (Start() waits for the capture thread to report)
    std::mutex m_mutex;
    std::condition_variable m_readyCv;
    bool m_ready = false;
};

} // namespace sdk
