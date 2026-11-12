#include "ClientSession.h"

#include "../app/AppConfig.h"
#include "../app/Language.h"
#include "../codec/FrameCodec.h"
#include "../net/Sha256.h"
#include "../net/TcpClient.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace sdk {

namespace {

// Fast fixed-point bilinear RGBA downscale for the receive-thread frame
// pre-scale. stbir's general linear path measured 8-16ms per frame here
// (premultiply/sRGB handling we do not need), which blocked the receive
// thread and capped the client at ~37fps. This integer-math version does
// plain bilinear with no alpha premultiply (~1-3ms for 1440x900 -> window).
void FastBilinearRGBA(const uint8_t* src, int sw, int sh, int ss,
                      uint8_t* dst, int dw, int dh)
{
    uint8_t* d = dst;
    for (int y = 0; y < dh; ++y) {
        const int sy = y * sh / dh;
        const int fy = (y * sh * 256 / dh) - sy * 256; // 0..255 fraction
        const int sy1 = (sy + 1 < sh) ? sy + 1 : sy;
        const uint8_t* r0 = src + (size_t)sy * ss;
        const uint8_t* r1 = src + (size_t)sy1 * ss;
        for (int x = 0; x < dw; ++x) {
            const int sx = x * sw / dw;
            const int fx = (x * sw * 256 / dw) - sx * 256;
            const int sx1 = (sx + 1 < sw) ? sx + 1 : sx;
            const uint8_t* p00 = r0 + (size_t)sx * 4;
            const uint8_t* p01 = r0 + (size_t)sx1 * 4;
            const uint8_t* p10 = r1 + (size_t)sx * 4;
            const uint8_t* p11 = r1 + (size_t)sx1 * 4;
            uint8_t* o = d + (size_t)x * 4;
            for (int c = 0; c < 4; ++c) {
                const int top = p00[c] * (256 - fx) + p01[c] * fx;
                const int bot = p10[c] * (256 - fx) + p11[c] * fx;
                o[c] = (uint8_t)(((top * (256 - fy) + bot * fy) + 32768) >> 16);
            }
        }
        d += (size_t)dw * 4;
    }
}

} // namespace

// ---------------------------------------------------------------- ClientSession

ClientSession::ClientSession(const Options& opt, Callbacks cb)
    : m_opt(opt), m_cb(std::move(cb)), m_mode(opt.mode)
{
    StartDecodeWorkers();
}

ClientSession::~ClientSession()
{
    Stop();
    StopDecodeWorkers();
}

void ClientSession::StartDecodeWorkers()
{
    int n = (int)std::thread::hardware_concurrency();
    if (n < 1) {
        n = 1;
    }
    if (n > 4) {
        n = 4;
    }
    for (int i = 0; i < n; ++i) {
        m_decodeWorkers.emplace_back(&ClientSession::DecodeWorkerMain, this);
    }
}

void ClientSession::StopDecodeWorkers()
{
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        m_decodeShutdown = true;
    }
    m_decodeCv.notify_all();
    for (std::thread& t : m_decodeWorkers) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_decodeWorkers.clear();
}

void ClientSession::DecodeWorkerMain()
{
    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lock(m_decodeMutex);
            m_decodeCv.wait(lock, [this]() {
                return m_decodeShutdown || m_decodePos < m_decodeJobs.size();
            });
            if (m_decodeShutdown) {
                return;
            }
            job = std::move(m_decodeJobs[m_decodePos++]);
        }
        // disjoint tile regions: blit without the frame mutex
        FrameDecoder::DecodeTileInto(job.png.data(), job.png.size(),
                                     m_decodeFrame, m_decodeW, m_decodeH,
                                     job.x, job.y);
        {
            std::lock_guard<std::mutex> lock(m_decodeMutex);
            if (--m_decodeLeft == 0) {
                m_decodeDoneCv.notify_one();
            }
        }
    }
}

void ClientSession::Start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_thread = std::thread(&ClientSession::Run, this);
    // Parsec-style render loop: repaint the latest frame at the negotiated
    // rate (e.g. 60Hz) even when the host is idle (it then only sends ~1fps
    // keep-alives). The UI keeps refreshing smoothly and the fps display
    // shows the render rate instead of 0/1.
    const int renderMs = std::max(10, 1000 / std::max(1, m_opt.fps));
    m_renderThread = std::thread([this, renderMs]() {
        while (m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(renderMs));
            if (!m_running.load()) {
                break;
            }
            Callbacks cb;
            {
                std::lock_guard<std::mutex> lock(m_cbMutex);
                cb = m_cb;
            }
            if (cb.onFrame) {
                cb.onFrame(); // UI takes the latest frame (same one if idle)
            }
        }
    });
}

void ClientSession::Stop()
{
    // Always join the session thread (it may still be finishing even when
    // m_running is already false); std::thread destructor aborts otherwise.
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning) {
        // wake the receive loop without releasing the fd number; the session
        // thread owns the fd and closes it itself
        Socket::Shutdown(m_fd);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
    if (m_state != State::Failed && m_state != State::Closed) {
        SetState(State::Closed, "");
    }
}

void ClientSession::SetState(State st, const std::string& text)
{
    m_state = st;
    if (m_cb.onState) {
        m_cb.onState(st, text);
    }
}

void ClientSession::SendInput(const InputEventMsg& ev)
{
    if (!IsConnected() || IsViewMode()) {
        static uint32_t sBlocked = 0;
        if ((++sBlocked & 0x3F) == 1) {
            fprintf(stderr, "[client] SendInput blocked: connected=%d view=%d\n",
                    IsConnected() ? 1 : 0, IsViewMode() ? 1 : 0);
        }
        return; // server-side gate too
    }
    PayloadWriter w;
    w.PutU8(ev.kind);
    w.PutU16(ev.xNorm);
    w.PutU16(ev.yNorm);
    w.PutU8(ev.buttons);
    w.PutU16((uint16_t)(int16_t)ev.wheelDelta);
    w.PutU16(ev.vkCode);
    w.PutU8(ev.modifiers);
    w.PutU8(ev.flags);
    std::lock_guard<std::mutex> lock(m_sendMutex);
    SendFrame(m_fd, MsgType::InputEvent, w.Result());
    static uint32_t sSent = 0;
    if ((++sSent & 0x3F) == 1) {
        fprintf(stderr, "[client] SendInput #%u kind=%u\n", sSent, ev.kind);
    }
}

void ClientSession::SendCursorMode(bool hide)
{
    if (m_fd == Socket::kInvalid || !m_running) {
        return;
    }
    std::vector<uint8_t> p(1);
    p[0] = hide ? 1 : 0;
    std::lock_guard<std::mutex> lock(m_sendMutex);
    SendFrame(m_fd, MsgType::CursorModeReq, p);
    fprintf(stderr, "[client] cursor mode req: hide=%d\n", hide ? 1 : 0);
}

void ClientSession::RequestCodec(uint8_t codec)
{
    if (m_fd == Socket::kInvalid) {
        return;
    }
    std::vector<uint8_t> p(1);
    p[0] = codec;
    std::lock_guard<std::mutex> lock(m_sendMutex);
    SendFrame(m_fd, MsgType::CodecSwitch, p);
}

void ClientSession::SetDirListCallback(
    std::function<void(bool, const std::string&, const std::vector<FileEntry>&)> cb)
{
    std::lock_guard<std::mutex> lock(m_cbMutex);
    m_cb.onDirList = std::move(cb);
}

void ClientSession::RequestDirList(const std::string& path)
{
    if (m_fd == Socket::kInvalid) {
        return;
    }
    PayloadWriter w;
    w.PutString(path);
    std::lock_guard<std::mutex> lock(m_sendMutex);
    SendFrame(m_fd, MsgType::DirListReq, w.Result());
}

void ClientSession::RequestDownload(const std::string& remotePath,
                                    const std::string& localDir)
{
    if (m_fd == Socket::kInvalid) {
        return;
    }
    PayloadWriter w;
    w.PutString(remotePath);
    w.PutString(localDir);
    std::lock_guard<std::mutex> lock(m_sendMutex);
    SendFrame(m_fd, MsgType::FileDownloadReq, w.Result());
}

void ClientSession::HandleDirListResp(const Frame& frame)
{
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const bool ok = r.GetU8() != 0;
    std::string path;
    r.GetString(path);
    std::vector<FileEntry> entries;
    if (ok && r.Ok()) {
        const uint16_t count = r.GetU16();
        for (uint16_t i = 0; i < count && r.Ok(); ++i) {
            FileEntry e;
            e.isDir = r.GetU8() != 0;
            e.size = r.GetU64();
            r.GetString(e.name);
            if (r.Ok()) {
                entries.push_back(std::move(e));
            }
        }
    }
    std::function<void(bool, const std::string&, const std::vector<FileEntry>&)> cb;
    {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        cb = m_cb.onDirList;
    }
    if (cb) {
        cb(ok && r.Ok(), path, entries);
    }
}

void ClientSession::SetDisplaySize(int w, int h)
{
    m_displayW.store(w > 0 ? w : 0);
    m_displayH.store(h > 0 ? h : 0);
}

void ClientSession::SetFrameCallbacks(Callbacks cb)
{
    std::lock_guard<std::mutex> lock(m_cbMutex);
    m_cb.onFrame = std::move(cb.onFrame);
    m_cb.onCursor = std::move(cb.onCursor);
    m_cb.onStats = std::move(cb.onStats);
    m_cb.onDirList = std::move(cb.onDirList);
}

void ClientSession::Run()
{
    RunImpl();
    Socket::Close(m_fd);
    m_fd = Socket::kInvalid;
    m_running = false;
}

void ClientSession::RunImpl()
{
    SetState(State::Connecting, SDK_TR("client.connecting"));

    // ---- connect ----
    m_fd = TcpClient::ConnectTo(m_opt.host, m_opt.port, 5000);
    if (m_fd == Socket::kInvalid) {
        m_lastError = "connect failed";
        if (m_running) SetState(State::Failed, SDK_TR("client.timeout"));
        m_running = false;
        return;
    }

    // ---- Hello ----
    {
        PayloadWriter w;
        w.PutString(ui::StringConvert::TToUTF8(App::Instance().Config().deviceName));
        w.PutU8((uint8_t)m_opt.mode);
        w.PutU8((uint8_t)m_opt.resCode);
        w.PutU8((uint8_t)m_opt.fps);
        // codec preference: video codec on macOS (VideoToolbox), PNG tiles
        // elsewhere. STARDESK_CODEC=1|2|3 selects H.264|VP9|AV1, 0 forces PNG.
        uint8_t codec = 0;
#if defined(__APPLE__)
        codec = (uint8_t)VideoCodec::H264;
#endif
        if (const char* c = std::getenv("STARDESK_CODEC")) {
            codec = (uint8_t)std::atoi(c);
        }
        w.PutU8(codec);
        // advertise our file-channel port so the host can push downloads
        w.PutU16(App::Instance().FileTx().BoundPort());
        // remote-cursor preference (trailer; absent = 0 for older hosts):
        // 1 = hide the remote cursor in the stream (local-cursor mode) -
        // Wayland embedded hosts re-capture with cursor_mode=1 (hidden)
        w.PutU8(App::Instance().Config().cursorMode == 0 ? 1 : 0);
        if (!SendFrame(m_fd, MsgType::Hello, w.Result())) {
            FailNet(SDK_TR("client.networkError"));
            return;
        }
    }

    // ---- wait for challenge (host may show a manual-accept dialog first) ----
    // The connect/handshake phase has a hard deadline so a host that accepts
    // TCP but never answers cannot leave the UI stuck on "connecting" forever.
    auto phaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    Frame frame;
    while (m_running) {
        if (!RecvFrame(m_fd, frame, 500)) {
            if (std::chrono::steady_clock::now() > phaseDeadline) {
                SetState(State::Failed, SDK_TR("client.timeout"));
                m_running = false;
                Socket::Close(m_fd);
                m_fd = Socket::kInvalid;
                return;
            }
            continue;
        }
        if (frame.type == MsgType::AcceptRequest) {
            // a manual-accept dialog is waiting for the remote user: allow longer
            phaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            SetState(State::WaitingAccept, SDK_TR("client.waitingAccept"));
            continue;
        }
        if (frame.type == MsgType::Challenge && frame.payload.size() >= 16) {
            break; // challenge received
        }
        if (frame.type == MsgType::AuthResult) {
            PayloadReader r(frame.payload.data(), frame.payload.size());
            const AuthResultCode code = (AuthResultCode)r.GetU8();
            m_lastError = "auth rejected";
            if (m_running) SetState(State::Failed, SDK_TR("client.rejected"));
            m_running = false;
            Socket::Close(m_fd);
            m_fd = Socket::kInvalid;
            return;
        }
    }
    if (!m_running) {
        return;
    }

    // ---- Auth (the challenge frame from the loop above is still in `frame`) ----
    {
        PayloadReader r(frame.payload.data(), frame.payload.size());
        uint8_t nonce[16] = {0};
        r.GetBytes(nonce, 16);
        if (!r.Ok()) {
            FailNet(SDK_TR("client.networkError"));
            return;
        }
        std::string challenge;
        challenge.assign((const char*)nonce, 16);
        challenge += ui::StringConvert::TToUTF8(m_opt.password);
        uint8_t digest[Sha256::kDigestSize];
        Sha256::Hash(challenge, digest);
        if (!SendFrame(m_fd, MsgType::Auth, digest, Sha256::kDigestSize)) {
            FailNet(SDK_TR("client.networkError"));
            return;
        }
    }

    // ---- AuthResult ----
    {
        Frame result;
        while (m_running) {
            if (RecvFrame(m_fd, result, 500)) {
                break;
            }
            if (std::chrono::steady_clock::now() > phaseDeadline) {
                FailNet(SDK_TR("client.timeout"));
                return;
            }
        }
        if (!m_running || result.type != MsgType::AuthResult) {
            FailNet(SDK_TR("client.networkError"));
            return;
        }
        PayloadReader r(result.payload.data(), result.payload.size());
        const AuthResultCode code = (AuthResultCode)r.GetU8();
        m_filePort = r.GetU16();
        r.GetBytes(m_token, 32);
        if (code == AuthResultCode::Ok) {
            App::Instance().FileTx().SetSessionToken(m_token);
        }
        if (code != AuthResultCode::Ok) {
            m_lastError = "auth rejected";
            if (m_running) SetState(State::Failed, SDK_TR("client.authFailed"));
            m_running = false;
            Socket::Close(m_fd);
            m_fd = Socket::kInvalid;
            return;
        }
    }

    SetState(State::Connected, SDK_TR("client.connected_"));

    // ---- receive loop ----
    m_statsAt = std::chrono::steady_clock::now();
    m_lastPingAt = m_statsAt;
    m_lastNotifyAt = m_statsAt;
    while (m_running) {
        // periodic latency ping - time-based so it fires even while the
        // stream is busy (the old idle-gated ping never fired during active
        // use, so the UI latency readout stayed 0)
        const auto pingNow = std::chrono::steady_clock::now();
        if (pingNow - m_lastPingAt > std::chrono::seconds(2)) {
            m_lastPingAt = pingNow;
            PayloadWriter w;
            w.PutU64((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         pingNow.time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(m_sendMutex);
            SendFrame(m_fd, MsgType::Ping, w.Result());
        }
        if (!Socket::WaitReadable(m_fd, 200)) {
            continue; // idle: loop back
        }
        Frame frame;
        if (!RecvFrame(m_fd, frame, 5000)) {
            break;
        }
        switch (frame.type) {
        case MsgType::Ping: {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            SendFrame(m_fd, MsgType::Pong, frame.payload);
            break;
        }
        case MsgType::Pong: {
            PayloadReader r(frame.payload.data(), frame.payload.size());
            const uint64_t t0 = r.GetU64();
            const auto now = std::chrono::steady_clock::now();
            if (t0 > 0) {
                m_latencyMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - std::chrono::steady_clock::time_point(
                                            std::chrono::milliseconds(t0)))
                                  .count();
            }
            break;
        }
        case MsgType::Disconnect: {
            m_running = false;
            break;
        }
        case MsgType::DirListResp: {
            HandleDirListResp(frame);
            break;
        }
        case MsgType::ScreenInit: {
            HandleScreenInit(frame);
            break;
        }
        case MsgType::ScreenTile: {
            HandleScreenTile(frame);
            break;
        }
        case MsgType::ScreenTiles: {
            HandleScreenTiles(frame);
            break;
        }
        case MsgType::VideoInit: {
            HandleVideoInit(frame);
            break;
        }
        case MsgType::VideoFrame: {
            HandleVideoFrame(frame);
            break;
        }
        case MsgType::AudioInit: {
            HandleAudioInit(frame);
            break;
        }
        case MsgType::AudioFrame: {
            HandleAudioFrame(frame);
            break;
        }
        case MsgType::ScreenEnd: {
            HandleScreenEnd(frame);
            break;
        }
        case MsgType::CursorPos: {
            HandleCursorPos(frame);
            break;
        }
        default:
            break;
        }
    }

    Socket::Close(m_fd);
    m_fd = Socket::kInvalid;
    m_running = false;
    SetState(State::Closed, SDK_TR("client.remoteDisconnected"));
    return;

}

void ClientSession::FailNet(const std::string& text)
{
    m_lastError = Socket::LastError();
    Socket::Close(m_fd);
    m_fd = Socket::kInvalid;
    m_running = false;
    if (m_state != State::Connected) {
        SetState(State::Failed, text);
    }
    else {
        SetState(State::Closed, SDK_TR("client.remoteDisconnected"));
    }
}

void ClientSession::Cleanup()
{
    Socket::Close(m_fd);
    m_fd = Socket::kInvalid;
    m_running = false;
#ifdef STARDESK_HAS_OPUS
    if (m_audioPlayback) {
        m_audioPlayback->Stop();
        m_audioPlayback.reset();
    }
    m_audioDecoder.reset();
    m_audioInit = false;
#endif
}

// ---------------------------------------------------------------- audio

#ifdef STARDESK_HAS_OPUS

void ClientSession::HandleAudioInit(const Frame& frame)
{
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const uint8_t codec = r.GetU8();
    const uint32_t rate = r.GetU32();
    const uint8_t channels = r.GetU8();
    const uint32_t samples = r.GetU32();
    if (!r.Ok() || codec != (uint8_t)AudioCodec::Opus || rate == 0 ||
        channels == 0 || samples == 0) {
        return;
    }
    // (re)create the decoder and the output device
    m_audioDecoder = std::make_unique<OpusAudioDecoder>();
    if (!m_audioDecoder->Init((int)rate, (int)channels, (int)samples)) {
        m_audioDecoder.reset();
        return;
    }
    m_audioPlayback = AudioPlayback::Create();
    if (m_audioPlayback &&
        !m_audioPlayback->Start((int)rate, (int)channels, (int)samples)) {
        m_audioPlayback.reset();
    }
    m_audioInit = true;
    fprintf(stderr, "[client] audio stream: Opus %uHz %uch %u-samples%s\n",
            rate, channels, samples,
            m_audioPlayback ? " (playing)" : " (no output device)");
    fflush(stderr);
}

void ClientSession::HandleAudioFrame(const Frame& frame)
{
    if (!m_audioInit || !m_audioDecoder || frame.payload.size() < 3) {
        return; // payload: u16 seq + opus packet
    }
    const uint8_t* pkt = frame.payload.data() + 2;
    const size_t pktLen = frame.payload.size() - 2;
    std::vector<int16_t> pcm;
    if (!m_audioDecoder->Decode(pkt, pktLen, pcm) || pcm.empty()) {
        return; // dropped/partial packet handled inside the decoder
    }
    if (m_audioPlayback) {
        m_audioPlayback->SubmitPcm(pcm.data(), pcm.size());
    }
}

#else // !STARDESK_HAS_OPUS

void ClientSession::HandleAudioInit(const Frame&) {}
void ClientSession::HandleAudioFrame(const Frame&) {}

#endif // STARDESK_HAS_OPUS

// ---------------------------------------------------------------- frame stream

void ClientSession::HandleScreenInit(const Frame& frame)
{
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const uint32_t w = r.GetU32();
    const uint32_t h = r.GetU32();
    if (!r.Ok() || w == 0 || h == 0 || w > 16384 || h > 16384) {
        return;
    }
    if (r.Remaining() >= 1) {
        m_cursorInStream.store(r.GetU8() != 0);
    }
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameW = (int)w;
        m_frameH = (int)h;
        m_frame.assign((size_t)w * h * 4, 0xFF);
        m_frameDirty = true;
    }
    {
        // arm the decode batch (tiles blit into m_frame; realloc only here,
        // before any tile of the batch arrives)
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        m_decodeFrame = m_frame.data();
        m_decodeW = (int)w;
        m_decodeH = (int)h;
        m_decodeJobs.clear();
        m_decodePos = 0;
        m_decodeLeft = 0;
    }
    NotifyFrame();
}

void ClientSession::HandleScreenTile(const Frame& frame)
{
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const int x = r.GetU16();
    const int y = r.GetU16();
    const int tw = r.GetU16();
    const int th = r.GetU16();
    std::vector<uint8_t> webp;
    webp.assign(frame.payload.begin() + 8, frame.payload.end());
    if (!r.Ok() || webp.empty()) {
        return;
    }
    QueueTile(x, y, std::move(webp));
    // no render here: the frame is only complete once the host sends
    // ScreenEnd, so a frame never mixes tiles from two batches (seams)
}

void ClientSession::HandleScreenTiles(const Frame& frame)
{
    // batched tile message: u32 count, then per tile
    // {u16 x, u16 y, u16 w, u16 h, u32 pngLen, png bytes}
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const uint32_t count = r.GetU32();
    for (uint32_t i = 0; i < count && r.Ok(); ++i) {
        const int x = r.GetU16();
        const int y = r.GetU16();
        const int tw = r.GetU16();
        const int th = r.GetU16();
        const uint32_t len = r.GetU32();
        if (len == 0 || len > 4 * 1024 * 1024) {
            break;
        }
        std::vector<uint8_t> png(len);
        if (!r.GetBytes(png.data(), len)) {
            break;
        }
        (void)tw;
        (void)th;
        QueueTile(x, y, std::move(png));
    }
    // no render here: the frame is only complete once the host sends
    // ScreenEnd, so a frame never mixes tiles from two batches (seams)
}

void ClientSession::QueueTile(int x, int y, std::vector<uint8_t> png)
{
    // push the tile to the decode pool; the blit happens on a worker
    DecodeJob job;
    job.x = x;
    job.y = y;
    job.png = std::move(png);
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        if (m_decodeFrame == nullptr) {
            return; // no frame yet (ScreenInit not seen)
        }
        m_decodeJobs.push_back(std::move(job));
        ++m_decodeLeft;
    }
    m_decodeCv.notify_one();
}

void ClientSession::HandleScreenEnd(const Frame& frame)
{
    (void)frame;
    const auto tEndStart = std::chrono::steady_clock::now();
    // wait for all tiles of this batch to be decoded+blitted, then render
    // the complete frame
    {
        std::unique_lock<std::mutex> lock(m_decodeMutex);
        m_decodeDoneCv.wait(lock, [this]() { return m_decodeLeft == 0; });
    }
    const auto tDecoded = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameDirty = true; // batch complete - NotifyFrame may render it
    }
    NotifyFrame();
    // test hook: STARDESK_PROFILE_LOG=<file> logs per-ScreenEnd timing
    if (const char* prof = std::getenv("STARDESK_PROFILE_LOG")) {
        const auto tNow = std::chrono::steady_clock::now();
        const long long waitUs = std::chrono::duration_cast<std::chrono::microseconds>(tDecoded - tEndStart).count();
        const long long notifyUs = std::chrono::duration_cast<std::chrono::microseconds>(tNow - tDecoded).count();
        const long long nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            tNow.time_since_epoch()).count();
        FILE* f = std::fopen(prof, "a");
        if (f != nullptr) {
            std::fprintf(f, "%lld %lld %lld\n", nowUs, waitUs, notifyUs);
            std::fclose(f);
        }
    }
}

void ClientSession::HandleCursorPos(const Frame& frame)
{
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const int xn = r.GetU16();
    const int yn = r.GetU16();
    const int visible = r.GetU8(); // 0 = host pointer hidden (Wayland metadata)
    if (!r.Ok()) {
        return;
    }
    m_cursorVisible.store(visible != 0);
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_cursorXN = xn;
        m_cursorYN = yn;
    }
    Callbacks cb;
    {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        cb = m_cb;
    }
    if (cb.onCursor) {
        static int sCursorDiag = 0;
        if ((++sCursorDiag & 0x1F) == 1) {
            fprintf(stderr, "[client] cursor received=(%d,%d)\n", xn, yn);
        }
        cb.onCursor(xn, yn);
    }
}

void ClientSession::NotifyFrame()
{
    // ScreenEnd framing means this is called exactly once per host frame,
    // and the host already paces to the negotiated fps - so render every
    // frame. A gap-based rate cap is harmful here: the host delivers frames
    // in clusters (2-3 frames then a pause), and any fixed gap rejects the
    // clustered frames, measuring ~49fps effective instead of the host's
    // ~60fps. The UI consumes via latest-wins (at most one pending frame),
    // so a slow renderer drops intermediate frames instead of stalling this
    // (receive) thread.
    std::vector<uint8_t> snapshot;
    int w = 0, h = 0, cxn = -1, cyn = -1;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_frameDirty) {
            return;
        }
        m_frameDirty = false;
        w = m_frameW;
        h = m_frameH;
        snapshot = m_frame;
        cxn = m_cursorXN;
        cyn = m_cursorYN;
    }
    PublishFrame(w, h, std::move(snapshot), cxn, cyn);
}

void ClientSession::HandleVideoInit(const Frame& frame)
{
    // {u8 codec, u32 w, u32 h, u32 spsLen, u32 ppsLen, sps, pps}
    PayloadReader r(frame.payload.data(), frame.payload.size());
    const int codec = (int)r.GetU8();
    const int w = (int)r.GetU32();
    const int h = (int)r.GetU32();
    const uint32_t spsLen = r.GetU32();
    const uint32_t ppsLen = r.GetU32();
    std::vector<uint8_t> sps(spsLen);
    std::vector<uint8_t> pps(ppsLen);
    if (!r.Ok() || w <= 0 || h <= 0 || spsLen > 1024 * 1024 || ppsLen > 1024 * 1024 ||
        !r.GetBytes(sps.data(), spsLen) || !r.GetBytes(pps.data(), ppsLen)) {
        return;
    }
    // trailer: cursorInStream = the capture already embeds the real pointer
    // (Wayland ScreenCast); absent (older host) = false
    if (r.Remaining() >= 1) {
        m_cursorInStream.store(r.GetU8() != 0);
    }
    m_decoder = std::make_unique<VideoDecoder>();
    if (!m_decoder->Init(codec, sps.data(), spsLen, pps.data(), ppsLen, w, h)) {
        fprintf(stderr, "[client] VideoDecoder::Init failed codec=%d\n", codec);
        m_decoder.reset(); // codec unavailable (non-macOS): keep PNG path
    }
    else {
        fprintf(stderr, "[client] video init ok codec=%d %dx%d\n", codec, w, h);
    }
}

void ClientSession::HandleVideoFrame(const Frame& frame)
{
    // H.264 stream: each VideoFrame is one complete frame; decode it and
    // publish through the same pipeline (resize + latest-wins + stats)
    if (m_decoder == nullptr || frame.payload.empty()) {
        return;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!m_decoder->Decode(frame.payload.data(), frame.payload.size(), rgba, w, h)) {
        return;
    }
    int cursorXN = -1;
    int cursorYN = -1;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        cursorXN = m_cursorXN;
        cursorYN = m_cursorYN;
    }
    PublishFrame(w, h, std::move(rgba), cursorXN, cursorYN);
}

void ClientSession::PublishFrame(int w, int h, std::vector<uint8_t>&& snapshot,
                                 int cxn, int cyn)
{
    static uint32_t sPub = 0;
    if (++sPub <= 10) {
        fprintf(stderr, "[client] publish frame %u %dx%d\n", sPub, w, h);
        fflush(stderr);
    }
    // test hook: dump a decoded frame as raw RGBA for pixel-level
    // color verification (STARDESK_DUMP_FRAME=<path>). Skips the first ~2s
    // so a codec-switch / missing-IDR white frame is not captured.
    if (const char* dumpPath = std::getenv("STARDESK_DUMP_FRAME")) {
        static int sFramesSeen = 0;
        if (!m_dumpedFrame && !snapshot.empty() && ++sFramesSeen >= 5) {
            m_dumpedFrame = true;
            FILE* f = std::fopen(dumpPath, "wb");
            if (f != nullptr) {
                const uint32_t hdr[2] = {(uint32_t)w, (uint32_t)h};
                std::fwrite(hdr, sizeof(hdr), 1, f);
                std::fwrite(snapshot.data(), snapshot.size(), 1, f);
                std::fclose(f);
                std::fprintf(stderr, "[dump] frame %dx%d saved\n", w, h);
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    // scale to the UI display size on the RECEIVE thread: the UI then blits
    // the frame 1:1 in paint, so a frame never triggers an expensive
    // full-window CPU resize that would starve input events on the UI thread
    const int dw = m_displayW.load();
    const int dh = m_displayH.load();
    if (dw > 0 && dh > 0 && !snapshot.empty()) {
        // pre-scale only when the display target is SMALLER than the source
        // (downscale). Upscaling here is pure waste: a 1440x900 source
        // stretched to a ~2880x1800 fullscreen dest burned a 4x bilinear
        // upscale + ~20MB/frame of copies on the receive thread every frame
        // (measured ~30fps in fullscreen). The paint upscales the smaller
        // frame to the window instead.
        if (dw < w || dh < h) {
            std::vector<uint8_t> scaled((size_t)dw * (size_t)dh * 4);
            FastBilinearRGBA(snapshot.data(), w, h, w * 4,
                             scaled.data(), dw, dh);
            snapshot.swap(scaled);
            w = dw;
            h = dh;
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    // latest-wins: store the newest frame; post the UI task only when none
    // is pending, so a slow renderer drops intermediate frames instead of
    // blocking this (receive) thread
    bool needPost = false;
    {
        std::lock_guard<std::mutex> lock(m_latestMutex);
        if (w > 0 && h > 0) {
            m_latestW = w;
            m_latestH = h;
            m_latestFrame = std::move(snapshot);
            m_latestCxn = cxn;
            m_latestCyn = cyn;
            if (!m_latestPending) {
                m_latestPending = true;
                needPost = true;
            }
        }
    }
    if (needPost) {
        Callbacks cb;
        {
            std::lock_guard<std::mutex> lock(m_cbMutex);
            cb = m_cb;
        }
        if (cb.onFrame) {
            cb.onFrame();
        }
    }
    UpdateStats();
    // fine-grained profile (env-gated): resizeUs postUs
    if (const char* prof2 = std::getenv("STARDESK_PROFILE2_LOG")) {
        const auto t3 = std::chrono::steady_clock::now();
        const long long rUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count();
        const long long pUs = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
        FILE* f = std::fopen(prof2, "a");
        if (f != nullptr) {
            std::fprintf(f, "%lld %lld\n", rUs, pUs);
            std::fclose(f);
        }
    }
}

bool ClientSession::TakeLatestFrame(int& w, int& h, std::vector<uint8_t>& rgba,
                                    int& cursorXN, int& cursorYN)
{
    std::lock_guard<std::mutex> lock(m_latestMutex);
    if (m_latestFrame.empty()) {
        return false;
    }
    w = m_latestW;
    h = m_latestH;
    rgba.swap(m_latestFrame); // move the pixels out (UI owns them now)
    cursorXN = m_latestCxn;
    cursorYN = m_latestCyn;
    m_latestPending = false; // session may post again for the next frame
    return true;
}

void ClientSession::UpdateStats()
{
    const auto now = std::chrono::steady_clock::now();
    ++m_framesThisSecond;
    if (now - m_statsAt >= std::chrono::seconds(1)) {
        const double dt = std::chrono::duration<double>(now - m_statsAt).count();
        const int rxFps = (int)(m_framesThisSecond / dt);
        m_framesThisSecond = 0;
        m_statsAt = now;
        // test hook: STARDESK_STATS_LOG=<file> appends "unix_ms fps latency_ms"
        // once per second (rxFps = the REAL received rate; the UI shows the
        // render rate below)
        if (const char* logPath = std::getenv("STARDESK_STATS_LOG")) {
            FILE* f = std::fopen(logPath, "a");
            if (f != nullptr) {
                const long long unixMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                std::fprintf(f, "%lld %d %d\n", unixMs, rxFps, m_latencyMs);
                std::fclose(f);
            }
        }
        Callbacks cb;
        {
            std::lock_guard<std::mutex> lock(m_cbMutex);
            cb = m_cb;
        }
        if (cb.onStats) {
            // Parsec-style split display: report the RENDER rate (the UI
            // repaints at this rate even when idle) AND the real received
            // rate, so the status bar can show both (渲染 60 | 远端 1)
            cb.onStats(m_opt.fps, rxFps, m_latencyMs);
        }
    }
}

} // namespace sdk
