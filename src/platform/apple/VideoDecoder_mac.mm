// macOS VideoToolbox decoder (hardware accelerated) for H.264 / VP9 / AV1,
// with a software fallback (libvpx / libaom) for VP9/AV1 on machines whose
// VideoToolbox lacks hardware support (VTIsHardwareDecodeSupported == 0).
// See VideoDecoder.h.
#if defined(__APPLE__)

#include "../../codec/VideoDecoder.h"
#include "../../codec/CodecUtils.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#if defined(STARDESK_HAS_VPX)
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#endif
#if defined(STARDESK_HAS_AOM)
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>
#endif

#include <cstdio>
#include <cstring>
#include <mutex>

namespace sdk {

namespace {

struct DecodeContext {
    VideoDecoder::Impl* impl = nullptr;
};

} // namespace

struct VideoDecoder::Impl {
    // ---- VideoToolbox path ----
    VTDecompressionSessionRef session = nullptr;
    CMVideoFormatDescriptionRef fmtDesc = nullptr;
    dispatch_semaphore_t sem = nullptr;
    std::vector<uint8_t> decoded; // from the decoder (RGBA after conversion)
    int outW = 0;
    int outH = 0;
    bool gotFrame = false;
    std::mutex mtx;
    void* decodedCtx = nullptr; // DecodeContext* kept for the session lifetime

    // ---- software path (libvpx / libaom) ----
    int swCodec = 0;   // 2 = VP9, 3 = AV1
    void* swCtx = nullptr; // vpx_codec_ctx_t* / aom_codec_ctx_t*
    std::vector<uint8_t> swRgba; // converted RGBA staging
};

namespace {

// VT output callback (runs on VideoToolbox's thread): copy the pixels, signal.
// VideoToolbox can deliver 32-bit output in several byte orders (BGRA is
// common, but some macOS versions deliver ARGB despite a BGRA request), which
// showed up as swapped red/blue channels. Convert by the ACTUAL reported
// format instead of assuming one order.
void DecodeOutputCallback(void* refCon, void* sourceFrameRefCon, OSStatus status,
                          VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer,
                          CMTime presentationTimeStamp, CMTime presentationDuration)
{
    (void)sourceFrameRefCon;
    (void)infoFlags;
    (void)presentationTimeStamp;
    (void)presentationDuration;
    DecodeContext* ctx = (DecodeContext*)refCon;
    if (ctx == nullptr || ctx->impl == nullptr) {
        return;
    }
    VideoDecoder::Impl* impl = ctx->impl;
    if (status != noErr || imageBuffer == nullptr) {
        static int sLogged = 0;
        if (sLogged < 5) {
            ++sLogged;
            fprintf(stderr, "[VideoDecoder] outputCallback status=%d frame=%p flags=%u\n",
                    (int)status, imageBuffer, (unsigned)infoFlags);
        }
        dispatch_semaphore_signal(impl->sem);
        return;
    }
    const OSType fmt = CVPixelBufferGetPixelFormatType(imageBuffer);
    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(imageBuffer);
    const size_t w = CVPixelBufferGetWidth(imageBuffer);
    const size_t h = CVPixelBufferGetHeight(imageBuffer);
    const size_t stride = CVPixelBufferGetBytesPerRow(imageBuffer);
    // NV12 (bi-planar Y + interleaved CbCr): the hardware decoder's native
    // output - converted here so the pixel-buffer pool stays small
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        const uint8_t* yP = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0);
        const uint8_t* uvP = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1);
        const size_t yS = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);
        const size_t uvS = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);
        if (yP != nullptr && uvP != nullptr) {
            auto clamp255 = [](int v) {
                return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            std::lock_guard<std::mutex> lock(impl->mtx);
            impl->decoded.resize(w * h * 4);
            impl->outW = (int)w;
            impl->outH = (int)h;
            for (size_t yy = 0; yy < h / 2; ++yy) {
                const uint8_t* y0 = yP + (yy * 2) * yS;
                const uint8_t* y1 = y0 + yS;
                const uint8_t* uv = uvP + yy * uvS;
                uint8_t* d0 = impl->decoded.data() + (yy * 2) * w * 4;
                uint8_t* d1 = d0 + w * 4;
                for (size_t x = 0; x < w / 2; ++x) {
                    const int cb = uv[x * 2] - 128;
                    const int cr = uv[x * 2 + 1] - 128;
                    const int c0 = y0[x * 2] - 16;
                    const int c1 = y0[x * 2 + 1] - 16;
                    const int c2 = y1[x * 2] - 16;
                    const int c3 = y1[x * 2 + 1] - 16;
                    d0[x * 8 + 0] = clamp255((298 * c0 + 409 * cr + 128) >> 8);
                    d0[x * 8 + 1] = clamp255((298 * c0 - 100 * cb - 208 * cr + 128) >> 8);
                    d0[x * 8 + 2] = clamp255((298 * c0 + 516 * cb + 128) >> 8);
                    d0[x * 8 + 3] = 255;
                    d0[x * 8 + 4] = clamp255((298 * c1 + 409 * cr + 128) >> 8);
                    d0[x * 8 + 5] = clamp255((298 * c1 - 100 * cb - 208 * cr + 128) >> 8);
                    d0[x * 8 + 6] = clamp255((298 * c1 + 516 * cb + 128) >> 8);
                    d0[x * 8 + 7] = 255;
                    d1[x * 8 + 0] = clamp255((298 * c2 + 409 * cr + 128) >> 8);
                    d1[x * 8 + 1] = clamp255((298 * c2 - 100 * cb - 208 * cr + 128) >> 8);
                    d1[x * 8 + 2] = clamp255((298 * c2 + 516 * cb + 128) >> 8);
                    d1[x * 8 + 3] = 255;
                    d1[x * 8 + 4] = clamp255((298 * c3 + 409 * cr + 128) >> 8);
                    d1[x * 8 + 5] = clamp255((298 * c3 - 100 * cb - 208 * cr + 128) >> 8);
                    d1[x * 8 + 6] = clamp255((298 * c3 + 516 * cb + 128) >> 8);
                    d1[x * 8 + 7] = 255;
                }
            }
            impl->gotFrame = true;
        }
    }
    else if (base != nullptr && (fmt == kCVPixelFormatType_32BGRA ||
                            fmt == kCVPixelFormatType_32ARGB ||
                            fmt == kCVPixelFormatType_32RGBA ||
                            fmt == kCVPixelFormatType_32ABGR)) {
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->decoded.resize(w * h * 4);
        impl->outW = (int)w;
        impl->outH = (int)h;
        for (size_t y = 0; y < h; ++y) {
            const uint8_t* src = base + y * stride;
            uint8_t* dst = impl->decoded.data() + y * w * 4;
            switch (fmt) {
            case kCVPixelFormatType_32BGRA: // B,G,R,A
                for (size_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
                break;
            case kCVPixelFormatType_32ARGB: // A,R,G,B
                for (size_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 1];
                    dst[x * 4 + 1] = src[x * 4 + 2];
                    dst[x * 4 + 2] = src[x * 4 + 3];
                    dst[x * 4 + 3] = 255;
                }
                break;
            case kCVPixelFormatType_32RGBA: // R,G,B,A
                for (size_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 0];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 2];
                    dst[x * 4 + 3] = 255;
                }
                break;
            case kCVPixelFormatType_32ABGR: // A,B,G,R
                for (size_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 3];
                    dst[x * 4 + 1] = src[x * 4 + 2];
                    dst[x * 4 + 2] = src[x * 4 + 1];
                    dst[x * 4 + 3] = 255;
                }
                break;
            }
        }
        impl->gotFrame = true;
    }
    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    dispatch_semaphore_signal(impl->sem);
}

} // namespace

VideoDecoder::VideoDecoder() : m_impl(new Impl())
{
    m_impl->sem = dispatch_semaphore_create(0);
}

VideoDecoder::~VideoDecoder()
{
    if (m_impl->session != nullptr) {
        VTDecompressionSessionWaitForAsynchronousFrames(m_impl->session);
        VTDecompressionSessionInvalidate(m_impl->session);
        CFRelease(m_impl->session);
    }
    if (m_impl->fmtDesc != nullptr) {
        CFRelease(m_impl->fmtDesc);
    }
    if (m_impl->sem != nullptr) {
        dispatch_release(m_impl->sem);
    }
    delete (DecodeContext*)m_impl->decodedCtx;
#if defined(STARDESK_HAS_VPX)
    if (m_impl->swCodec == 2 && m_impl->swCtx != nullptr) {
        vpx_codec_destroy((vpx_codec_ctx_t*)m_impl->swCtx);
        delete (vpx_codec_ctx_t*)m_impl->swCtx;
    }
#endif
#if defined(STARDESK_HAS_AOM)
    if (m_impl->swCodec == 3 && m_impl->swCtx != nullptr) {
        aom_codec_destroy((aom_codec_ctx_t*)m_impl->swCtx);
        delete (aom_codec_ctx_t*)m_impl->swCtx;
    }
#endif
    delete m_impl;
}

bool VideoDecoder::Init(int codec, const uint8_t* sps, size_t spsLen,
                        const uint8_t* pps, size_t ppsLen, int width, int height)
{
    // software fallback for VP9/AV1 when VideoToolbox cannot decode them
    // (VTIsHardwareDecodeSupported is unreliable - it reports VP9 unsupported
    // on Apple Silicon where the hardware path actually works - so we attempt
    // the VT session first and fall back only when it fails)
    auto initSoftware = [&]() -> bool {
#if defined(STARDESK_HAS_VPX) || defined(STARDESK_HAS_AOM)
        fprintf(stderr, "[VideoDecoder] VideoToolbox %s unavailable - using software\n",
                codec == 2 ? "VP9" : "AV1");
        m_impl->swCodec = codec;
#if defined(STARDESK_HAS_VPX)
        if (codec == 2) {
            vpx_codec_ctx_t* ctx = new vpx_codec_ctx_t();
            if (vpx_codec_dec_init(ctx, vpx_codec_vp9_dx(), nullptr, 0) !=
                VPX_CODEC_OK) {
                delete ctx;
                return false;
            }
            m_impl->swCtx = ctx;
            return true;
        }
#endif
#if defined(STARDESK_HAS_AOM)
        if (codec == 3) {
            aom_codec_ctx_t* ctx = new aom_codec_ctx_t();
            if (aom_codec_dec_init(ctx, aom_codec_av1_dx(), nullptr, 0) !=
                AOM_CODEC_OK) {
                delete ctx;
                return false;
            }
            m_impl->swCtx = ctx;
            return true;
        }
#endif
        return false;
#else
        fprintf(stderr, "[VideoDecoder] %s decode unsupported (no libvpx/libaom)\n",
                codec == 2 ? "VP9" : "AV1");
        return false;
#endif
    };

    // ---- VideoToolbox path ----
    OSStatus st = noErr;
    if (codec == 1 || codec == 4) { // H.264 (x264 or OpenH264): SPS/PPS sets
        if (sps == nullptr || pps == nullptr || spsLen == 0 || ppsLen == 0) {
            return false;
        }
        const uint8_t* params[2] = {sps, pps};
        const size_t sizes[2] = {spsLen, ppsLen};
        st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, params, sizes, 4, &m_impl->fmtDesc);
    }
    else if (codec == 2 || codec == 3) { // VP9 / AV1: no parameter sets
        CMVideoCodecType vtCodec =
            codec == 2 ? kCMVideoCodecType_VP9 : kCMVideoCodecType_AV1;
        st = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, vtCodec,
                                            (int32_t)width, (int32_t)height,
                                            nullptr, &m_impl->fmtDesc);
    }
    else {
        return false;
    }
    if (st != noErr || m_impl->fmtDesc == nullptr) {
        fprintf(stderr, "[VideoDecoder] format description failed: codec=%d st=%d\n",
                codec, (int)st);
        if (codec == 2 || codec == 3) {
            return initSoftware();
        }
        return false;
    }

    VTDecompressionOutputCallbackRecord cb;
    DecodeContext* ctx = new DecodeContext();
    ctx->impl = m_impl;
    cb.decompressionOutputCallback = DecodeOutputCallback;
    cb.decompressionOutputRefCon = ctx;
    m_impl->decodedCtx = ctx;

    CFMutableDictionaryRef destAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    // NV12 (the hardware decoder's native format): the BGRA output made the
    // decoder reserve a huge pixel-buffer pool (~14 x 1080p BGRA buffers,
    // measured +120MB RSS one-time). NV12 buffers are ~1/4 the size.
    int pxFmtVal = (int)kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pxFmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                       &pxFmtVal);
    CFDictionarySetValue(destAttrs, kCVPixelBufferPixelFormatTypeKey, pxFmt);
    CFRelease(pxFmt);
    // Cap the decoder's pixel-buffer pool: without this VideoToolbox lazily
    // reserves a huge pool (~40x1080p buffers, measured +120MB RSS) which
    // looks like a memory leak. 4 buffers is plenty for this single-frame
    // pipeline (latest-wins, one decode in flight).
    int poolMin = 4;
    CFNumberRef poolNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                         &poolMin);
    CFDictionarySetValue(destAttrs, kCVPixelBufferPoolMinimumBufferCountKey,
                         poolNum);
    CFRelease(poolNum);

    st = VTDecompressionSessionCreate(kCFAllocatorDefault, m_impl->fmtDesc,
                                      nullptr, destAttrs, &cb, &m_impl->session);
    CFRelease(destAttrs);
    if (st != noErr) {
        fprintf(stderr, "[VideoDecoder] VTDecompressionSessionCreate failed: codec=%d st=%d\n",
                codec, (int)st);
        if (m_impl->fmtDesc != nullptr) {
            CFRelease(m_impl->fmtDesc);
            m_impl->fmtDesc = nullptr;
        }
        delete ctx;
        m_impl->decodedCtx = nullptr;
        if (codec == 2 || codec == 3) {
            return initSoftware();
        }
        return false;
    }
    return true;
}

bool VideoDecoder::Decode(const uint8_t* data, size_t len, std::vector<uint8_t>& out,
                          int& w, int& h)
{
    // ---- software path ----
    if (m_impl->swCtx != nullptr && (m_impl->swCodec == 2 || m_impl->swCodec == 3)) {
        bool ok = false;
        int fw = 0, fh = 0;
#if defined(STARDESK_HAS_VPX)
        if (m_impl->swCodec == 2) {
            if (vpx_codec_decode((vpx_codec_ctx_t*)m_impl->swCtx, data, len,
                                 nullptr, 0) == VPX_CODEC_OK) {
                vpx_codec_iter_t iter = nullptr;
                const vpx_image_t* img = vpx_codec_get_frame(
                    (vpx_codec_ctx_t*)m_impl->swCtx, &iter);
                if (img != nullptr && img->fmt == VPX_IMG_FMT_I420) {
                    fw = (int)img->d_w;
                    fh = (int)img->d_h;
                    static bool sLogged = false;
                    if (!sLogged && img->planes[0] != nullptr) {
                        sLogged = true;
                        fprintf(stderr, "[VideoDecoder] vpx: range=%d cs=%d bd=%d firstYUV=(%d,%d,%d)\n",
                                (int)img->range, (int)img->cs, (int)img->bit_depth,
                                img->planes[0][0], img->planes[1][0], img->planes[2][0]);
                    }
                    std::lock_guard<std::mutex> lock(m_impl->mtx);
                    m_impl->swRgba.resize((size_t)fw * fh * 4);
                    // libvpx YV12: planes[1] is V(Cr), planes[2] is U(Cb)
                    I420ToRgba(img->planes[0], img->stride[0],
                               img->planes[2], img->stride[2],
                               img->planes[1], img->stride[1],
                               fw, fh, m_impl->swRgba.data());
                    ok = true;
                }
            }
        }
#endif
#if defined(STARDESK_HAS_AOM)
        if (m_impl->swCodec == 3) {
            if (aom_codec_decode((aom_codec_ctx_t*)m_impl->swCtx, data, len,
                                 nullptr) == AOM_CODEC_OK) {
                aom_codec_iter_t iter = nullptr;
                const aom_image_t* img = aom_codec_get_frame(
                    (aom_codec_ctx_t*)m_impl->swCtx, &iter);
                if (img != nullptr && img->fmt == AOM_IMG_FMT_I420) {
                    fw = (int)img->d_w;
                    fh = (int)img->d_h;
                    std::lock_guard<std::mutex> lock(m_impl->mtx);
                    m_impl->swRgba.resize((size_t)fw * fh * 4);
                    // libaom YV12: planes[1] is V(Cr), planes[2] is U(Cb)
                    I420ToRgba(img->planes[0], img->stride[0],
                               img->planes[2], img->stride[2],
                               img->planes[1], img->stride[1],
                               fw, fh, m_impl->swRgba.data());
                    ok = true;
                }
            }
        }
#endif
        if (ok) {
            std::lock_guard<std::mutex> lock(m_impl->mtx);
            out = m_impl->swRgba;
            w = fw;
            h = fh;
        }
        return ok;
    }

    // ---- VideoToolbox path ----
    if (m_impl->session == nullptr || data == nullptr || len == 0) {
        fprintf(stderr, "[VideoDecoder] decode: no session or empty data\n");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);
        m_impl->gotFrame = false;
    }
    // Robust sample-buffer creation: COPY the NAL into an owned, malloc'd
    // block (kCFAllocatorMalloc lets CoreMedia free it). Borrowing the
    // caller's payload vector made CoreMedia memmove over a pointer that was
    // no longer valid once the frame buffer moved (vector realloc), which
    // crashed in CMSampleBufferCreateReady (SIGSEGV, KERN_INVALID_ADDRESS at
    // 0x0) after a stream hiccup.
    if (len == 0 || len > (64u << 20)) {
        return false;
    }
    uint8_t* copy = (uint8_t*)malloc(len);
    if (copy == nullptr) {
        return false;
    }
    std::memcpy(copy, data, len);
    CMBlockBufferRef blockBuf = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, copy, len, kCFAllocatorMalloc, nullptr, 0, len, 0,
        &blockBuf);
    if (st != noErr) {
        fprintf(stderr, "[VideoDecoder] CMBlockBufferCreate st=%d\n", (int)st);
        free(copy);
        return false;
    }
    CMSampleBufferRef sampleBuf = nullptr;
    const size_t sampleSizes[1] = {len};
    // numSampleTimingEntries must be 0 when sampleTimingArray is NULL (a
    // non-zero count with a NULL array makes CoreMedia memmove from address 0)
    st = CMSampleBufferCreateReady(kCFAllocatorDefault, blockBuf, m_impl->fmtDesc,
                                   1, 0, nullptr, 1, sampleSizes, &sampleBuf);
    if (st != noErr) {
        fprintf(stderr, "[VideoDecoder] CMSampleBufferCreate st=%d\n", (int)st);
        CFRelease(blockBuf);
        return false;
    }
    st = VTDecompressionSessionDecodeFrame(m_impl->session, sampleBuf, 0, nullptr,
                                           nullptr);
    CFRelease(sampleBuf);
    CFRelease(blockBuf);
    if (st != noErr) {
        fprintf(stderr, "[VideoDecoder] VTDecodeFrame st=%d\n", (int)st);
        return false;
    }
    // wait for the output callback (hardware decode ~1-2ms; cap at 250ms)
    const long r = dispatch_semaphore_wait(
        m_impl->sem, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC));
    if (r != 0) {
        fprintf(stderr, "[VideoDecoder] decode timeout\n");
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (!m_impl->gotFrame) {
        fprintf(stderr, "[VideoDecoder] callback produced no frame\n");
        return false;
    }
    out = m_impl->decoded;
    w = m_impl->outW;
    h = m_impl->outH;
    return true;
}

} // namespace sdk

#endif // __APPLE__
