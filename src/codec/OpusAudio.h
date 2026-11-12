// Opus audio codec wrappers (libopus).
//
// Encoder: host side - turns 48kHz stereo float PCM into 20ms Opus packets.
// Decoder: client side - turns Opus packets back into 48kHz stereo S16 PCM
// for the audio-output device. Both are compiled only when the build finds
// libopus (STARDESK_HAS_OPUS); the rest of the app degrades gracefully.
#pragma once

#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace sdk {

class OpusAudioEncoder {
public:
    OpusAudioEncoder() = default;
    ~OpusAudioEncoder();

    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    /** 48kHz stereo. frameSamples = samples per packet (960 = 20ms).
     *  Returns false if libopus init failed. */
    bool Init(int sampleRate, int channels, int frameSamples);

    /** Encode one complete frame of interleaved F32 PCM (frameSamples *
     *  channels values). Appends the Opus packet to out. Returns false if
     *  the input was not a full frame. */
    bool EncodeFloat(const float* pcm, int sampleCount, std::vector<uint8_t>& out);

    int SampleRate() const { return m_rate; }
    int Channels() const { return m_channels; }
    int FrameSamples() const { return m_frameSamples; }

private:
    ::OpusEncoder* m_enc = nullptr;
    int m_rate = 0;
    int m_channels = 0;
    int m_frameSamples = 0;
};

class OpusAudioDecoder {
public:
    OpusAudioDecoder() = default;
    ~OpusAudioDecoder();

    OpusAudioDecoder(const OpusAudioDecoder&) = delete;
    OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

    /** 48kHz stereo (must match the encoder). frameSamples = 960 (20ms). */
    bool Init(int sampleRate, int channels, int frameSamples);

    /** Decode one Opus packet into interleaved S16 PCM (frameSamples *
     *  channels int16 values appended to out). Returns false on decode
     *  error (the packet is dropped). */
    bool Decode(const uint8_t* data, size_t len, std::vector<int16_t>& out);

    int SampleRate() const { return m_rate; }
    int Channels() const { return m_channels; }
    int FrameSamples() const { return m_frameSamples; }

private:
    ::OpusDecoder* m_dec = nullptr;
    int m_rate = 0;
    int m_channels = 0;
    int m_frameSamples = 0;
};

} // namespace sdk
