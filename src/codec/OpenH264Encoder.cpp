#include "OpenH264Encoder.h"
#include "CodecUtils.h"

#if defined(STARDESK_HAS_OPENH264)

#include <wels/codec_api.h>

#include <cstdio>
#include <cstring>

namespace sdk {

OpenH264Encoder::~OpenH264Encoder()
{
    if (m_enc != nullptr) {
        WelsDestroySVCEncoder(m_enc);
        m_enc = nullptr;
    }
}

namespace {

// Parse one Annex-B NAL: returns the NAL payload (after the start code) and
// its length. pNalLengthInByte includes the start code; the start code is
// 00 00 01 (3) or 00 00 00 01 (4).
const uint8_t* NalPayload(const uint8_t* start, int nalLength, int& payloadLen)
{
    const uint8_t* p = start;
    int sc = 0;
    if (nalLength >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        sc = 4;
    }
    else if (nalLength >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        sc = 3;
    }
    else {
        return nullptr;
    }
    payloadLen = nalLength - sc;
    return p + sc;
}

} // namespace

bool OpenH264Encoder::Init(int width, int height, int fps)
{
    if (m_enc != nullptr) {
        return true;
    }
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (WelsCreateSVCEncoder(&m_enc) != 0 || m_enc == nullptr) {
        m_enc = nullptr;
        return false;
    }
    SEncParamExt param;
    m_enc->GetDefaultParams(&param);
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.fMaxFrameRate = (float)(fps > 0 ? fps : 60);
    param.iPicWidth = width;
    param.iPicHeight = height;
    param.iTargetBitrate = 6000;
    param.iMaxBitrate = 6000;
    param.iRCMode = RC_BITRATE_MODE;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableFrameSkip = false; // encode every frame - the encoder is fast
                                    // (measured 336fps @1440x900); frame-skip
                                    // makes the rate control drop nearly every
                                    // frame -> PNG fallback (user saw 4-5fps)
    param.uiIntraPeriod = (fps > 0 ? fps : 60) * 5; // IDR every ~5s
    param.eSpsPpsIdStrategy = CONSTANT_ID; // same SPS/PPS across IDRs
    param.bPrefixNalAddingCtrl = false;
    param.iMultipleThreadIdc = 4;
    param.sSpatialLayers[0].iVideoWidth = width;
    param.sSpatialLayers[0].iVideoHeight = height;
    param.sSpatialLayers[0].iSpatialBitrate = 6000;
    param.sSpatialLayers[0].iMaxSpatialBitrate = 6000;
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    if (m_enc->InitializeExt(&param) != 0) {
        WelsDestroySVCEncoder(m_enc);
        m_enc = nullptr;
        return false;
    }
    m_w = width;
    m_h = height;
    m_i420.resize((size_t)width * height * 3 / 2);

    // OpenH264 has no "get headers" API - encode dummy frames until the first
    // IDR carries SPS/PPS (CONSTANT_ID keeps them stable for VideoInit).
    std::vector<uint8_t> dummy;
    std::vector<uint8_t> black((size_t)width * height * 4, 0);
    for (int i = 0; i < 4 && (m_sps.empty() || m_pps.empty()); ++i) {
        Encode(black.data(), width, height, dummy);
    }
    // the dummy frames consumed the initial IDR - force the first REAL frame
    // to be an IDR so the client's decoder can start
    m_needIdr = true;
    return !m_sps.empty() && !m_pps.empty();
}

void OpenH264Encoder::GetSpsPps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const
{
    sps = m_sps;
    pps = m_pps;
}

void OpenH264Encoder::RequestKeyFrame()
{
    m_needIdr = true; // applied in Encode() right before EncodeFrame
}

bool OpenH264Encoder::Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
{
    if (m_enc == nullptr || rgba == nullptr || w != m_w || h != m_h) {
        return false;
    }
    uint8_t* Y = m_i420.data();
    uint8_t* U = Y + (size_t)m_w * m_h;
    uint8_t* V = U + (size_t)(m_w / 2) * (m_h / 2);
    RgbaToI420(rgba, m_w, m_h, Y, m_w, U, m_w / 2, V, m_w / 2); // standard Cb/Cr

    SSourcePicture pic;
    std::memset(&pic, 0, sizeof(pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = m_w;
    pic.iPicHeight = m_h;
    pic.iStride[0] = m_w;
    pic.iStride[1] = m_w / 2;
    pic.iStride[2] = m_w / 2;
    pic.pData[0] = Y;
    pic.pData[1] = U;
    pic.pData[2] = V;

    SFrameBSInfo info;
    std::memset(&info, 0, sizeof(info));
    if (m_needIdr) {
        m_enc->ForceIntraFrame(true);
    }
    if (m_enc->EncodeFrame(&pic, &info) != 0) {
        return false;
    }
    // Confirm an actual IDR slice came out before clearing the force flag.
    // Scan ALL layers (OpenH264 can put the slice in a different layer than
    // SPS/PPS). Keep forcing until one appears - giving up early produced an
    // IDR-less stream (IDR=0 over 40 frames) that VideoToolbox rejects with
    // kVTInvalidSessionErr -12909 (no keyframe to start from).
    if (m_needIdr) {
        bool foundIdr = false;
        for (int l = 0; l < info.iLayerNum && !foundIdr; ++l) {
            const SLayerBSInfo& li = info.sLayerInfo[l];
            const uint8_t* b = li.pBsBuf;
            for (int i = 0; i < li.iNalCount; ++i) {
                int pl = 0;
                const uint8_t* n = NalPayload(b, li.pNalLengthInByte[i], pl);
                b += li.pNalLengthInByte[i];
                if (n != nullptr && pl >= 1 && (n[0] & 0x1F) == 5) {
                    foundIdr = true;
                    break;
                }
            }
        }
        if (foundIdr) {
            m_needIdr = false;
        }
    }
    if (info.iLayerNum <= 0 || info.sLayerInfo[0].iNalCount <= 0 ||
        info.sLayerInfo[0].pBsBuf == nullptr) {
        return false;
    }
    // OpenH264 splits output across layers: SPS/PPS in layer 0, the actual
    // slice in layer 1 (or later) - so ALL layers' NALs must be emitted,
    // not just layer[0] (sending only layer[0] dropped every IDR slice).
    for (int l = 0; l < info.iLayerNum; ++l) {
        const SLayerBSInfo& layer = info.sLayerInfo[l];
        const uint8_t* buf = layer.pBsBuf;
        for (int i = 0; i < layer.iNalCount; ++i) {
            const int nalLen = layer.pNalLengthInByte[i];
            int payloadLen = 0;
            const uint8_t* nal = NalPayload(buf, nalLen, payloadLen);
            buf += nalLen;
            if (nal == nullptr || payloadLen < 1) {
                continue;
            }
            const uint8_t type = nal[0] & 0x1F;
            if (type == 7 || type == 8) {
                // SPS/PPS: cached for VideoInit, not sent in-band
                if (m_sps.empty() && type == 7) {
                    m_sps.assign(nal, nal + payloadLen);
                }
                if (m_pps.empty() && type == 8) {
                    m_pps.assign(nal, nal + payloadLen);
                }
                continue;
            }
            // AVCC: 4-byte big-endian length + NAL payload
            const uint8_t len4[4] = {(uint8_t)((payloadLen >> 24) & 0xFF),
                                     (uint8_t)((payloadLen >> 16) & 0xFF),
                                     (uint8_t)((payloadLen >> 8) & 0xFF),
                                     (uint8_t)(payloadLen & 0xFF)};
            out.insert(out.end(), len4, len4 + 4);
            out.insert(out.end(), nal, nal + payloadLen);
        }
    }
    return true;
}

} // namespace sdk

#else // !STARDESK_HAS_OPENH264

// stub: the encoder is only available where libopenh264 is linked
namespace sdk {
OpenH264Encoder::~OpenH264Encoder() {}
bool OpenH264Encoder::Init(int, int, int) { return false; }
bool OpenH264Encoder::Encode(const uint8_t*, int, int, std::vector<uint8_t>&) { return false; }
void OpenH264Encoder::GetSpsPps(std::vector<uint8_t>&, std::vector<uint8_t>&) const {}
void OpenH264Encoder::RequestKeyFrame() {}
} // namespace sdk

#endif // STARDESK_HAS_OPENH264
