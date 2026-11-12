#include "H264Encoder.h"
#include "CodecUtils.h"

#if defined(STARDESK_HAS_X264)

#include <x264.h>

#include <cstring>

namespace sdk {

H264Encoder::~H264Encoder()
{
    if (m_enc != nullptr) {
        x264_encoder_close(m_enc);
        m_enc = nullptr;
    }
}

bool H264Encoder::Init(int width, int height, int fps)
{
    if (m_enc != nullptr) {
        return true;
    }
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
        return false;
    }
    x264_param_t param;
    x264_param_default_preset(&param, "veryfast", "zerolatency");
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = fps > 0 ? fps : 60;
    param.i_fps_den = 1;
    param.i_keyint_max = fps > 0 ? fps * 5 : 300; // IDR every ~5s for recovery
    param.b_repeat_headers = 0; // SPS/PPS go out once via VideoInit
    param.b_annexb = 0;         // length-prefixed (AVCC) NAL output
    param.i_threads = 0;        // x264 picks (12 cores on the host)
    param.i_log_level = X264_LOG_NONE;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 22;
    if (x264_param_apply_profile(&param, "baseline") < 0) {
        return false;
    }
    m_enc = x264_encoder_open(&param);
    if (m_enc == nullptr) {
        return false;
    }
    m_w = width;
    m_h = height;

    // SPS/PPS for VideoInit (raw bytes, no length prefix)
    x264_nal_t* nals = nullptr;
    int i_nals = 0;
    if (x264_encoder_headers(m_enc, &nals, &i_nals) >= 0) {
        for (int i = 0; i < i_nals; ++i) {
            const uint8_t* p = nals[i].p_payload;
            const int payloadLen = nals[i].i_payload;
            if (payloadLen < 5) {
                continue;
            }
            // length prefix is 4 bytes BE; the NAL header is the 5th byte
            const int nalLen = ((int)p[0] << 24) | ((int)p[1] << 16) |
                               ((int)p[2] << 8) | (int)p[3];
            const uint8_t type = p[4] & 0x1F;
            const uint8_t* data = p + 4;
            const int len = nalLen <= payloadLen - 4 ? nalLen : payloadLen - 4;
            if (type == 7) { // SPS
                m_sps.assign(data, data + len);
            }
            else if (type == 8) { // PPS
                m_pps.assign(data, data + len);
            }
        }
    }
    m_i420.resize((size_t)width * height * 3 / 2);
    return !m_sps.empty() && !m_pps.empty();
}

void H264Encoder::GetSpsPps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const
{
    sps = m_sps;
    pps = m_pps;
}

bool H264Encoder::Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
{
    if (m_enc == nullptr || rgba == nullptr || w != m_w || h != m_h) {
        return false;
    }
    // RGBA -> I420
    uint8_t* Y = m_i420.data();
    uint8_t* U = Y + (size_t)m_w * m_h;
    uint8_t* V = U + (size_t)(m_w / 2) * (m_h / 2);
    RgbaToI420(rgba, m_w, m_h, Y, m_w, U, m_w / 2, V, m_w / 2); // standard Cb/Cr

    x264_picture_t picIn;
    x264_picture_init(&picIn);
    picIn.img.i_csp = X264_CSP_I420;
    picIn.img.i_plane = 3;
    picIn.img.plane[0] = Y;
    picIn.img.plane[1] = U;
    picIn.img.plane[2] = V;
    picIn.img.i_stride[0] = m_w;
    picIn.img.i_stride[1] = m_w / 2;
    picIn.img.i_stride[2] = m_w / 2;

    x264_nal_t* nals = nullptr;
    int i_nals = 0;
    x264_picture_t picOut;
    x264_picture_init(&picOut);
    const int frameSize = x264_encoder_encode(m_enc, &nals, &i_nals, &picIn, &picOut);
    if (frameSize < 0) {
        return false;
    }
    for (int i = 0; i < i_nals; ++i) {
        out.insert(out.end(), nals[i].p_payload,
                   nals[i].p_payload + nals[i].i_payload);
    }
    return true;
}

} // namespace sdk

#else // !STARDESK_HAS_X264

// stub: the encoder is only available where libx264 is linked
namespace sdk {
H264Encoder::~H264Encoder() {}
bool H264Encoder::Init(int, int, int) { return false; }
bool H264Encoder::Encode(const uint8_t*, int, int, std::vector<uint8_t>&) { return false; }
void H264Encoder::GetSpsPps(std::vector<uint8_t>&, std::vector<uint8_t>&) const {}
} // namespace sdk

#endif // STARDESK_HAS_X264
