#include "VpxEncoder.h"
#include "CodecUtils.h"

#if defined(STARDESK_HAS_VPX)

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#include <cstdio>
#include <cstring>

namespace sdk {

VpxEncoder::~VpxEncoder()
{
    if (m_ctx != nullptr) {
        vpx_codec_destroy((vpx_codec_ctx_t*)m_ctx);
        delete (vpx_codec_ctx_t*)m_ctx;
        m_ctx = nullptr;
    }
}

bool VpxEncoder::Init(int width, int height, int fps)
{
    if (m_ctx != nullptr) {
        return true;
    }
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
        return false;
    }
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) {
        return false;
    }
    cfg.g_w = width;
    cfg.g_h = height;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = fps > 0 ? fps : 60;
    cfg.g_pass = VPX_RC_ONE_PASS;
    cfg.g_lag_in_frames = 0;       // realtime: no lookahead latency
    cfg.rc_end_usage = VPX_VBR;    // quality-based; bitrate is a ceiling
    cfg.rc_target_bitrate = 6000;  // 6 Mbps ceiling at 1440x900
    cfg.rc_min_quantizer = 18;
    cfg.rc_max_quantizer = 48;
    cfg.g_threads = 0;             // auto (12 cores on the host)
    cfg.kf_max_dist = fps > 0 ? fps * 5 : 300; // keyframe every ~5s

    vpx_codec_ctx_t* ctx = new vpx_codec_ctx_t();
    if (vpx_codec_enc_init(ctx, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) {
        delete ctx;
        return false;
    }
    // realtime speed setting (0-9; higher = faster). 9 = the fastest realtime
    // preset: slightly lower quality, measurably higher encode throughput
    // (the host was the bottleneck at 8 - VP9 measured ~10fps behind H.264).
    vpx_codec_control(ctx, VP8E_SET_CPUUSED, 9);
    m_ctx = ctx;
    m_w = width;
    m_h = height;
    m_i420.resize((size_t)width * height * 3 / 2);
    return true;
}

bool VpxEncoder::Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
{
    vpx_codec_ctx_t* ctx = (vpx_codec_ctx_t*)m_ctx;
    if (ctx == nullptr || rgba == nullptr || w != m_w || h != m_h) {
        return false;
    }
    uint8_t* Y = m_i420.data();
    uint8_t* U = Y + (size_t)m_w * m_h;
    uint8_t* V = U + (size_t)(m_w / 2) * (m_h / 2);
    RgbaToI420(rgba, m_w, m_h, Y, m_w, U, m_w / 2, V, m_w / 2);
    static bool sDbg = false;
    if (!sDbg) {
        sDbg = true;
        fprintf(stderr, "[VpxEncoder] input RGBA=(%d,%d,%d) I420 YUV=(%d,%d,%d)\n",
                rgba[0], rgba[1], rgba[2], Y[0], U[0], V[0]);
    }

    vpx_image_t img;
    std::memset(&img, 0, sizeof(img));
    // border=0: we own the buffer and set strides manually; a non-zero border
    // makes libvpx treat the frame as (w+2*border) wide, which misreads the
    // tightly packed buffer and produced a black right half in the decode.
    vpx_img_wrap(&img, VPX_IMG_FMT_I420, (unsigned int)m_w, (unsigned int)m_h, 0,
                 Y);
    // NOTE: libvpx's "I420" is actually YV12 plane order (Y, V, U):
    // planes[1] is V (Cr), planes[2] is U (Cb) - values are swapped here so
    // the decoded stream carries standard Cb/Cr.
    img.planes[0] = Y;
    img.planes[1] = V; // Cr
    img.planes[2] = U; // Cb
    img.stride[0] = m_w;
    img.stride[1] = m_w / 2;
    img.stride[2] = m_w / 2;

    if (vpx_codec_encode(ctx, &img, m_frameIndex++, 1, 0, VPX_DL_REALTIME) !=
        VPX_CODEC_OK) {
        return false;
    }
    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t* pkt = nullptr;
    while ((pkt = vpx_codec_get_cx_data(ctx, &iter)) != nullptr) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT && pkt->data.frame.sz > 0) {
            out.insert(out.end(), (const uint8_t*)pkt->data.frame.buf,
                       (const uint8_t*)pkt->data.frame.buf + pkt->data.frame.sz);
        }
    }
    return true;
}

} // namespace sdk

#else // !STARDESK_HAS_VPX

namespace sdk {
VpxEncoder::~VpxEncoder() {}
bool VpxEncoder::Init(int, int, int) { return false; }
bool VpxEncoder::Encode(const uint8_t*, int, int, std::vector<uint8_t>&) { return false; }
} // namespace sdk

#endif // STARDESK_HAS_VPX
