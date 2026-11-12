#include "FrameCodec.h"

#include <png.h>

// Declarations only: the stbir implementation is compiled into the dui
// library (ImageDecoderUtil.cpp defines STB_IMAGE_RESIZE_IMPLEMENTATION).
#include "stb_image_resize2.h"

#include <algorithm>
#include <cstring>

namespace sdk {

namespace {

// ---- PNG memory-stream callbacks ----

void PngWriteFn(png_structp png, png_bytep data, png_size_t len)
{
    std::vector<uint8_t>* buf = (std::vector<uint8_t>*)png_get_io_ptr(png);
    buf->insert(buf->end(), data, data + len);
}

void PngFlushFn(png_structp /*png*/)
{
}

struct PngReadCtx {
    const uint8_t* data;
    size_t len;
    size_t pos;
};

void PngReadFn(png_structp png, png_bytep out, png_size_t n)
{
    PngReadCtx* ctx = (PngReadCtx*)png_get_io_ptr(png);
    const size_t avail = ctx->len - ctx->pos;
    const size_t take = n < avail ? n : avail;
    std::memcpy(out, ctx->data + ctx->pos, take);
    ctx->pos += take;
}

// ---- encode/decode ----

bool PngEncodeRGBA(const uint8_t* rgba, int w, int h, int stride,
                   std::vector<uint8_t>& out)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                              nullptr, nullptr);
    if (png == nullptr) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_set_write_fn(png, &out, PngWriteFn, PngFlushFn);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    // level 3: ~2x faster than the default 6 with a modest size increase -
    // tiles are tiny and this is a live stream, speed wins over ratio
    png_set_compression_level(png, 3);
    png_write_info(png, info);

    std::vector<png_bytep> rows((size_t)h);
    for (int y = 0; y < h; ++y) {
        rows[(size_t)y] = (png_bytep)(rgba + (size_t)y * stride);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return true;
}

bool PngDecodeRGBA(const uint8_t* data, size_t len,
                   std::vector<uint8_t>& rgba, int& w, int& h)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                             nullptr, nullptr);
    if (png == nullptr) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }
    PngReadCtx ctx = { data, len, 0 };
    png_set_read_fn(png, &ctx, PngReadFn);

    png_read_info(png, info);
    png_uint_32 pw = 0, ph = 0;
    int bitDepth = 0, colorType = 0;
    png_get_IHDR(png, info, &pw, &ph, &bitDepth, &colorType, nullptr, nullptr, nullptr);
    if (pw == 0 || ph == 0 || pw > 16384 || ph > 16384) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    // normalize to 8-bit RGBA
    if (bitDepth == 16) {
        png_set_strip_16(png);
    }
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);

    w = (int)pw;
    h = (int)ph;
    rgba.resize((size_t)w * h * 4);
    std::vector<png_bytep> rows((size_t)h);
    for (int y = 0; y < h; ++y) {
        rows[(size_t)y] = rgba.data() + (size_t)y * w * 4;
    }
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

} // namespace

// ---------------------------------------------------------------- FrameEncoder

FrameEncoder::FrameEncoder(int quality) : m_quality(quality)
{
    StartWorkers();
}

FrameEncoder::~FrameEncoder()
{
    StopWorkers();
}

void FrameEncoder::Reset()
{
    m_prev.clear();
    m_hashes.clear();
    m_width = 0;
    m_height = 0;
}

void FrameEncoder::StartWorkers()
{
    int n = (int)std::thread::hardware_concurrency();
    if (n < 1) {
        n = 1;
    }
    if (n > 8) {
        n = 8;
    }
    m_workerScratch.resize((size_t)n);
    for (size_t i = 0; i < m_workerScratch.size(); ++i) {
        m_workerScratch[i].resize((size_t)kTile * kTile * 4);
        m_workers.emplace_back(&FrameEncoder::WorkerMain, this, i);
    }
}

void FrameEncoder::StopWorkers()
{
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_shutdown = true;
    }
    m_jobCv.notify_all();
    for (std::thread& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workers.clear();
}

void FrameEncoder::WorkerMain(size_t workerId)
{
    std::vector<uint8_t>& scratch = m_workerScratch[workerId];
    for (;;) {
        TileJob job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [this]() { return m_shutdown || m_jobPos < m_jobs.size(); });
            if (m_shutdown) {
                return;
            }
            job = m_jobs[m_jobPos++];
        }

        const uint8_t* rgba = m_frame;
        const int width = m_frameW;
        // FNV-1a hash over 32-bit words (4x faster than byte-wise; a changed
        // byte always changes its word, so change detection stays reliable)
        uint64_t hash = 1469598103934665603ull;
        for (int yy = 0; yy < job.th; ++yy) {
            const uint32_t* row =
                (const uint32_t*)(rgba + (size_t)((job.y0 + yy) * width + job.x0) * 4);
            for (int xx = 0; xx < job.tw; ++xx) {
                hash ^= row[xx];
                hash *= 1099511628211ull;
            }
        }
        if (hash == m_hashes[job.hashIdx]) {
            // unchanged tile: nothing to send
        }
        else {
            m_hashes[job.hashIdx] = hash;
            // copy the tile into the worker's full 64x64 scratch (PNG stride)
            for (int yy = 0; yy < job.th; ++yy) {
                std::memcpy(scratch.data() + (size_t)yy * kTile * 4,
                            rgba + (size_t)((job.y0 + yy) * width + job.x0) * 4,
                            (size_t)job.tw * 4);
            }
            for (int yy = 0; yy < kTile; ++yy) {
                std::memset(scratch.data() + (size_t)(yy * kTile + job.tw) * 4, 0,
                            (size_t)(kTile - job.tw) * 4);
            }
            for (int yy = job.th; yy < kTile; ++yy) {
                std::memset(scratch.data() + (size_t)yy * kTile * 4, 0,
                            (size_t)kTile * 4);
            }
            EncodedTile tile;
            tile.x = job.x0;
            tile.y = job.y0;
            tile.w = job.tw;
            tile.h = job.th;
            if (PngEncodeRGBA(scratch.data(), kTile, kTile, kTile * 4, tile.webp)) {
                std::lock_guard<std::mutex> lock(m_jobMutex);
                m_results.push_back(std::move(tile));
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            if (--m_jobsLeft == 0) {
                m_doneCv.notify_one();
            }
        }
    }
}

bool FrameEncoder::EncodeFrame(const uint8_t* rgba, int width, int height,
                               std::vector<EncodedTile>& out)
{
    const bool sizeChanged = (width != m_width || height != m_height);
    if (sizeChanged) {
        m_width = width;
        m_height = height;
        m_prev.assign(rgba, rgba + (size_t)width * height * 4);
        m_tilesX = (width + kTile - 1) / kTile;
        m_tilesY = (height + kTile - 1) / kTile;
        m_hashes.assign((size_t)m_tilesX * m_tilesY, 0);

        // first frame after a size change: full frame
        EncodedTile full;
        full.x = 0;
        full.y = 0;
        full.w = width;
        full.h = height;
        if (!PngEncodeRGBA(rgba, width, height, width * 4, full.webp)) {
            return sizeChanged;
        }
        // hash the tiles of the stored frame
        for (int ty = 0; ty < m_tilesY; ++ty) {
            for (int tx = 0; tx < m_tilesX; ++tx) {
                const int x0 = tx * kTile;
                const int y0 = ty * kTile;
                const int tw = std::min(kTile, width - x0);
                const int th = std::min(kTile, height - y0);
                uint64_t hash = 1469598103934665603ull; // FNV-1a offset basis
                for (int yy = 0; yy < th; ++yy) {
                    const uint32_t* row = (const uint32_t*)(m_prev.data() +
                        (size_t)((y0 + yy) * width + x0) * 4);
                    for (int xx = 0; xx < tw; ++xx) {
                        hash ^= row[xx];
                        hash *= 1099511628211ull;
                    }
                }
                m_hashes[(size_t)ty * m_tilesX + tx] = hash;
            }
        }
        out.push_back(std::move(full));
        return sizeChanged;
    }

    // diff against the stored frame - the per-tile hash + PNG encode is
    // dispatched to the worker pool (tiles are independent)
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_frame = rgba;
        m_frameW = width;
        m_frameH = height;
        m_jobs.clear();
        m_jobs.reserve((size_t)m_tilesX * m_tilesY);
        for (int ty = 0; ty < m_tilesY; ++ty) {
            for (int tx = 0; tx < m_tilesX; ++tx) {
                TileJob job;
                job.x0 = tx * kTile;
                job.y0 = ty * kTile;
                job.tw = std::min(kTile, width - job.x0);
                job.th = std::min(kTile, height - job.y0);
                job.hashIdx = (size_t)ty * m_tilesX + tx;
                m_jobs.push_back(job);
            }
        }
        m_jobPos = 0;
        m_jobsLeft = (int)m_jobs.size();
        m_results.clear();
    }
    m_jobCv.notify_all();
    {
        std::unique_lock<std::mutex> lock(m_jobMutex);
        m_doneCv.wait(lock, [this]() { return m_jobsLeft == 0; });
        out.swap(m_results);
    }
    // (the previous frame's pixels are not kept: the delta path compares
    // tile hashes only, so copying m_prev would be pure overhead)
    return sizeChanged;
}

bool FrameEncoder::Downscale(const uint8_t* src, int srcW, int srcH,
                             int targetW, int targetH,
                             std::vector<uint8_t>& dst, int& dstW, int& dstH)
{
    if (targetW <= 0 || targetH <= 0) {
        return false;
    }
    const double scale = std::min((double)targetW / srcW, (double)targetH / srcH);
    if (scale >= 1.0) {
        return false; // fits already
    }
    dstW = std::max(1, (int)(srcW * scale));
    dstH = std::max(1, (int)(srcH * scale));
    dst.resize((size_t)dstW * dstH * 4);

    // stbir (SIMD-accelerated) instead of the previous per-pixel double
    // bilinear loop - ~5-10x faster for the 4K -> 1080p downscale case
    stbir_resize_uint8_linear(src, srcW, srcH, srcW * 4,
                              dst.data(), dstW, dstH, dstW * 4, STBIR_RGBA);
    return true;
}

// ---------------------------------------------------------------- FrameDecoder

bool FrameDecoder::DecodeTileInto(const uint8_t* png, size_t len,
                                  uint8_t* dst, int dstW, int dstH,
                                  int x, int y)
{
    std::vector<uint8_t> decoded;
    int w = 0, h = 0;
    if (!PngDecodeRGBA(png, len, decoded, w, h)) {
        return false;
    }
    // blit into the frame (may be partially off-screen for edge tiles)
    for (int yy = 0; yy < h; ++yy) {
        const int dstY = y + yy;
        if (dstY < 0 || dstY >= dstH) {
            continue;
        }
        for (int xx = 0; xx < w; ++xx) {
            const int dstX = x + xx;
            if (dstX < 0 || dstX >= dstW) {
                continue;
            }
            std::memcpy(dst + (size_t)(dstY * dstW + dstX) * 4,
                        decoded.data() + (size_t)(yy * w + xx) * 4, 4);
        }
    }
    return true;
}

} // namespace sdk
