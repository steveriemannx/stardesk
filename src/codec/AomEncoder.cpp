#include "AomEncoder.h"
#include "CodecUtils.h"

#if defined(STARDESK_HAS_AOM)

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

#include <cstring>

namespace sdk {

AomEncoder::~AomEncoder()
{
    if (m_ctx != nullptr) {
        aom_codec_destroy((aom_codec_ctx_t*)m_ctx);
        delete (aom_codec_ctx_t*)m_ctx;
        m_ctx = nullptr;
    }
}

bool AomEncoder::Init(int width, int height, int fps)
{
    if (m_ctx != nullptr) {
        return true;
    }
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
        return false;
    }
    aom_codec_enc_cfg_t cfg;
    if (aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg,
                                     AOM_USAGE_REALTIME) != AOM_CODEC_OK) {
        return false;
    }
    cfg.g_w = width;
    cfg.g_h = height;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = fps > 0 ? fps : 60;
    cfg.g_pass = AOM_RC_ONE_PASS;
    cfg.g_lag_in_frames = 0;       // realtime: no lookahead latency
    cfg.rc_end_usage = AOM_VBR;    // quality-based; bitrate is a ceiling
    cfg.rc_target_bitrate = 6000;  // 6 Mbps ceiling at 1440x900
    cfg.rc_min_quantizer = 18;
    cfg.rc_max_quantizer = 48;
    cfg.g_threads = 0;             // auto
    cfg.kf_max_dist = fps > 0 ? fps * 5 : 300; // keyframe every ~5s

    aom_codec_ctx_t* ctx = new aom_codec_ctx_t();
    if (aom_codec_enc_init(ctx, aom_codec_av1_cx(), &cfg, 0) != AOM_CODEC_OK) {
        delete ctx;
        return false;
    }
    // AV1 software encoding is the slowest of the three; cpu-used 8-9 keeps
    // it near-realtime on the 12-core host. 0-9 (higher = faster/lower quality)
    aom_codec_control(ctx, AOME_SET_CPUUSED, 8);
    aom_codec_control(ctx, AOME_SET_CQ_LEVEL, 28);
    m_ctx = ctx;
    m_w = width;
    m_h = height;
    m_i420.resize((size_t)width * height * 3 / 2);
    return true;
}

bool AomEncoder::Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
{
    aom_codec_ctx_t* ctx = (aom_codec_ctx_t*)m_ctx;
    if (ctx == nullptr || rgba == nullptr || w != m_w || h != m_h) {
        return false;
    }
    uint8_t* Y = m_i420.data();
    uint8_t* U = Y + (size_t)m_w * m_h;
    uint8_t* V = U + (size_t)(m_w / 2) * (m_h / 2);
    RgbaToI420(rgba, m_w, m_h, Y, m_w, U, m_w / 2, V, m_w / 2);

    aom_image_t img;
    std::memset(&img, 0, sizeof(img));
    // border=0: same rationale as VpxEncoder (a non-zero border widens the
    // frame to w+2*border, misreading the tightly packed buffer -> black
    // right half in the decode)
    aom_img_wrap(&img, AOM_IMG_FMT_I420, (unsigned int)m_w, (unsigned int)m_h, 0,
                 Y);
    // NOTE: libaom's "I420" is also YV12 plane order (Y, V, U): planes[1] is
    // V (Cr), planes[2] is U (Cb).
    img.planes[0] = Y;
    img.planes[1] = V; // Cr
    img.planes[2] = U; // Cb
    img.stride[0] = m_w;
    img.stride[1] = m_w / 2;
    img.stride[2] = m_w / 2;

    if (aom_codec_encode(ctx, &img, (aom_codec_pts_t)m_frameIndex++, 1, 0) !=
        AOM_CODEC_OK) {
        return false;
    }
    aom_codec_iter_t iter = nullptr;
    const aom_codec_cx_pkt_t* pkt = nullptr;
    while ((pkt = aom_codec_get_cx_data(ctx, &iter)) != nullptr) {
        if (pkt->kind == AOM_CODEC_CX_FRAME_PKT && pkt->data.frame.sz > 0) {
            out.insert(out.end(), (const uint8_t*)pkt->data.frame.buf,
                       (const uint8_t*)pkt->data.frame.buf + pkt->data.frame.sz);
        }
    }
    return true;
}

} // namespace sdk

#else // !STARDESK_HAS_AOM

namespace sdk {
AomEncoder::~AomEncoder() {}
bool AomEncoder::Init(int, int, int) { return false; }
bool AomEncoder::Encode(const uint8_t*, int, int, std::vector<uint8_t>&) { return false; }
} // namespace sdk

#endif // STARDESK_HAS_AOM
