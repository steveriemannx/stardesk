// CoreAudio (AudioQueue) playback for the StarDesk client.
//
// The session thread feeds decoded PCM via SubmitPcm(); an internal deque
// (guarded by a mutex) buffers a few frames to absorb network jitter, and
// the AudioQueue callback pulls frames from it, filling with silence when
// empty (a dropped packet just causes a short silence gap).
#import "../AudioPlayback.h"

#import <AudioToolbox/AudioToolbox.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace sdk {

namespace {

const size_t kMaxQueuedFrames = 32; // ~640ms headroom, bounds latency

class AudioQueuePlayback : public AudioPlayback {
public:
    ~AudioQueuePlayback() override { Stop(); }

    bool Start(int sampleRate, int channels, int frameSamples) override
    {
        if (m_queue != nullptr) {
            return true;
        }
        m_sampleRate = sampleRate;
        m_channels = channels;
        m_frameSamples = frameSamples;
        m_frameBytes = (UInt32)(frameSamples * channels * sizeof(int16_t));

        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate = sampleRate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags =
            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        fmt.mFramesPerPacket = 1;
        fmt.mChannelsPerFrame = channels;
        fmt.mBitsPerChannel = 16;
        fmt.mBytesPerFrame = (UInt32)(channels * sizeof(int16_t));
        fmt.mBytesPerPacket = fmt.mBytesPerFrame;

        OSStatus st = AudioQueueNewOutput(&fmt, &AudioQueuePlayback::OnAudioQueue,
                                          this, nullptr, nullptr, 0, &m_queue);
        if (st != noErr || m_queue == nullptr) {
            fprintf(stderr, "[audio] AudioQueueNewOutput failed: %d\n", (int)st);
            m_queue = nullptr;
            return false;
        }

        // pre-allocate 3 buffers (the classic AudioQueue pattern)
        for (int i = 0; i < 3; ++i) {
            AudioQueueBufferRef buf = nullptr;
            if (AudioQueueAllocateBuffer(m_queue, m_frameBytes, &buf) == noErr &&
                buf != nullptr) {
                m_buffers.push_back(buf);
            }
        }
        if (m_buffers.empty()) {
            fprintf(stderr, "[audio] AudioQueueAllocateBuffer failed\n");
            AudioQueueDispose(m_queue, true);
            m_queue = nullptr;
            return false;
        }
        // prime the queue: enqueue all buffers with silence BEFORE Start.
        // Without an initial enqueue the queue has no data to play and never
        // calls the callback, so the output stays silent forever.
        for (AudioQueueBufferRef buf : m_buffers) {
            memset(buf->mAudioData, 0, m_frameBytes);
            buf->mAudioDataByteSize = m_frameBytes;
            AudioQueueEnqueueBuffer(m_queue, buf, 0, nullptr);
        }

        st = AudioQueueStart(m_queue, nullptr);
        if (st != noErr) {
            fprintf(stderr, "[audio] AudioQueueStart failed: %d\n", (int)st);
            AudioQueueDispose(m_queue, true);
            m_queue = nullptr;
            return false;
        }
        // explicit full volume (the default is 1.0, but be safe)
        AudioQueueSetParameter(m_queue, kAudioQueueParam_Volume, 1.0);
        fprintf(stderr, "[audio] playback started (%dHz %dch %d-samples)\n",
                sampleRate, channels, frameSamples);
        return true;
    }

    void SubmitPcm(const int16_t* data, size_t sampleCount) override
    {
        if (m_queue == nullptr) {
            return;
        }
        const size_t frameVals = (size_t)m_frameSamples * m_channels;
        if (sampleCount < frameVals) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pcm.emplace_back(data, data + frameVals);
        // bound the queue: drop the oldest frames when the consumer stalls
        while (m_pcm.size() > kMaxQueuedFrames) {
            m_pcm.pop_front();
        }
    }

    void Stop() override
    {
        if (m_queue != nullptr) {
            AudioQueueStop(m_queue, true);
            for (AudioQueueBufferRef b : m_buffers) {
                AudioQueueFreeBuffer(m_queue, b);
            }
            m_buffers.clear();
            AudioQueueDispose(m_queue, true);
            m_queue = nullptr;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pcm.clear();
    }

private:
    static void OnAudioQueue(void* userData, AudioQueueRef queue,
                             AudioQueueBufferRef buffer)
    {
        AudioQueuePlayback* self = static_cast<AudioQueuePlayback*>(userData);
        self->FillBuffer(buffer);
    }

    void FillBuffer(AudioQueueBufferRef buffer)
    {
        // pull one frame; silence when the jitter buffer is empty
        std::vector<int16_t> frame;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pcm.empty()) {
                frame = std::move(m_pcm.front());
                m_pcm.pop_front();
            }
        }
        if (frame.empty()) {
            memset(buffer->mAudioData, 0, m_frameBytes);
        } else {
            memcpy(buffer->mAudioData, frame.data(),
                   m_frameBytes < frame.size() * sizeof(int16_t)
                       ? m_frameBytes
                       : frame.size() * sizeof(int16_t));
        }
        buffer->mAudioDataByteSize = m_frameBytes;
        AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr);
    }

    AudioQueueRef m_queue = nullptr;
    std::vector<AudioQueueBufferRef> m_buffers;
    int m_sampleRate = 0;
    int m_channels = 0;
    int m_frameSamples = 0;
    UInt32 m_frameBytes = 0;
    std::mutex m_mutex;
    std::deque<std::vector<int16_t>> m_pcm;
};

} // namespace

std::unique_ptr<AudioPlayback> AudioPlayback::Create()
{
    return std::make_unique<AudioQueuePlayback>();
}

} // namespace sdk
