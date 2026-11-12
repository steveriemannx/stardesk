// Opus audio codec wrappers (libopus). See OpusAudio.h.
#include "OpusAudio.h"

#ifdef STARDESK_HAS_OPUS

#include <opus/opus.h>

#include <cstdio>

namespace sdk {

// ------------------------------------------------------------ encoder

OpusAudioEncoder::~OpusAudioEncoder()
{
    if (m_enc != nullptr) {
        ::opus_encoder_destroy(m_enc);
        m_enc = nullptr;
    }
}

bool OpusAudioEncoder::Init(int sampleRate, int channels, int frameSamples)
{
    int err = 0;
    m_enc = ::opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO,
                                  &err);
    if (err != OPUS_OK || m_enc == nullptr) {
        fprintf(stderr, "[audio] opus encoder init failed: %s\n",
                ::opus_strerror(err));
        m_enc = nullptr;
        return false;
    }
    m_rate = sampleRate;
    m_channels = channels;
    m_frameSamples = frameSamples;
    // 96 kbps stereo: transparent for speech, good for music at low cost
    ::opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(96000));
    ::opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(5));
    ::opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(OPUS_AUTO));
    return true;
}

bool OpusAudioEncoder::EncodeFloat(const float* pcm, int sampleCount,
                                   std::vector<uint8_t>& out)
{
    if (m_enc == nullptr || sampleCount != m_frameSamples) {
        return false;
    }
    // worst-case Opus packet is 1275 bytes per 20ms frame
    unsigned char packet[1500];
    const opus_int32 n =
        ::opus_encode_float(m_enc, pcm, m_frameSamples, packet, sizeof(packet));
    if (n < 0) {
        fprintf(stderr, "[audio] opus encode failed: %s\n",
                ::opus_strerror((int)n));
        return false;
    }
    out.insert(out.end(), packet, packet + n);
    return true;
}

// ------------------------------------------------------------ decoder

OpusAudioDecoder::~OpusAudioDecoder()
{
    if (m_dec != nullptr) {
        ::opus_decoder_destroy(m_dec);
        m_dec = nullptr;
    }
}

bool OpusAudioDecoder::Init(int sampleRate, int channels, int frameSamples)
{
    int err = 0;
    m_dec = ::opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || m_dec == nullptr) {
        fprintf(stderr, "[audio] opus decoder init failed: %s\n",
                ::opus_strerror(err));
        m_dec = nullptr;
        return false;
    }
    m_rate = sampleRate;
    m_channels = channels;
    m_frameSamples = frameSamples;
    return true;
}

bool OpusAudioDecoder::Decode(const uint8_t* data, size_t len,
                              std::vector<int16_t>& out)
{
    if (m_dec == nullptr) {
        return false;
    }
    // decode into a fixed scratch buffer, then append (S16 interleaved)
    std::vector<int16_t> pcm((size_t)m_frameSamples * m_channels);
    const int n = ::opus_decode(m_dec, data, (opus_int32)len, pcm.data(),
                                m_frameSamples, 0);
    if (n < 0) {
        // a lost/partial packet: decode silence instead of dropping the
        // stream position (opus_decode with PLC)
        ::opus_decode(m_dec, nullptr, 0, pcm.data(), m_frameSamples, 0);
        out.insert(out.end(), pcm.begin(), pcm.end());
        return false;
    }
    if (n < m_frameSamples) {
        // decoder returned a partial frame (should not happen at 20ms
        // packets); pad the remainder with silence
        pcm.resize((size_t)m_frameSamples * m_channels, 0);
    }
    out.insert(out.end(), pcm.begin(), pcm.end());
    return true;
}

} // namespace sdk

#endif // STARDESK_HAS_OPUS
