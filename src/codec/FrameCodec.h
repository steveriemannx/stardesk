#ifndef STARDESK_CODEC_FRAME_CODEC_H_
#define STARDESK_CODEC_FRAME_CODEC_H_

#include "dui/dui.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sdk {

/** One encoded screen tile (PNG bytes) to send to the client. */
struct EncodedTile {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> webp;
};

/** Host-side encoder: compares the incoming frame with the previous one
 *  (64x64 tiles, FNV-1a hash) and PNG-encodes only the changed tiles.
 *  libpng comes from the dui build (libdui-png, linked automatically).
 *  (The dui-bundled libwebp has no encoder, so PNG is used in phase 1.)
 */
class FrameEncoder {
public:
    FrameEncoder(int quality = 75);
    ~FrameEncoder();

    /** Feed a new full frame (RGBA). Changed tiles are appended to `out`.
     *  Returns true when the frame size changed (caller must send ScreenInit
     *  before the tiles).
     */
    bool EncodeFrame(const uint8_t* rgba, int width, int height,
                     std::vector<EncodedTile>& out);

    /** Forget the previous frame (forces a full frame next time). */
    void Reset();

    /** Bilinear downscale RGBA to fit inside targetW x targetH (aspect kept);
     *  returns false when the source already fits (caller keeps the source). */
    static bool Downscale(const uint8_t* src, int srcW, int srcH,
                          int targetW, int targetH,
                          std::vector<uint8_t>& dst, int& dstW, int& dstH);

private:
    // one tile hashed + compared + (if changed) PNG-encoded by a worker
    struct TileJob {
        int x0 = 0;
        int y0 = 0;
        int tw = 0;
        int th = 0;
        size_t hashIdx = 0;
    };

    void WorkerMain(size_t workerId);
    void StartWorkers();
    void StopWorkers();

    static const int kTile = 64;
    int m_quality = 75;
    int m_width = 0;
    int m_height = 0;
    std::vector<uint8_t> m_prev;      // previous frame RGBA
    std::vector<uint64_t> m_hashes;   // per-tile hash of the previous frame
    int m_tilesX = 0;
    int m_tilesY = 0;

    // parallel tile encoding state (see EncodeFrame)
    std::vector<std::thread> m_workers;
    std::vector<std::vector<uint8_t>> m_workerScratch; // per-worker 64x64 buffer
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;   // new jobs available
    std::condition_variable m_doneCv;  // all jobs finished
    std::vector<TileJob> m_jobs;
    size_t m_jobPos = 0;
    int m_jobsLeft = 0;
    bool m_shutdown = false;
    const uint8_t* m_frame = nullptr;  // frame being encoded (read-only)
    int m_frameW = 0;
    int m_frameH = 0;
    std::vector<EncodedTile> m_results; // tiles produced by the workers
};

/** Client-side decoder helpers: PNG -> RGBA blit into a frame buffer. */
class FrameDecoder {
public:
    /** Decode a PNG buffer into `dst` at (x, y). dst is RGBA of dstW x dstH.
     *  Returns false on decode error. */
    static bool DecodeTileInto(const uint8_t* png, size_t len,
                               uint8_t* dst, int dstW, int dstH,
                               int x, int y);
};

} // namespace sdk

#endif // STARDESK_CODEC_FRAME_CODEC_H_
