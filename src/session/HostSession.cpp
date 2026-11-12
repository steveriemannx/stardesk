#include "HostSession.h"

#include "../app/AppConfig.h"
#include "../app/Language.h"
#include "../codec/FrameCodec.h"
#include "../codec/H264Encoder.h"
#include "../codec/OpenH264Encoder.h"
#include "../codec/VpxEncoder.h"
#include "../codec/AomEncoder.h"
#include "../net/Sha256.h"
#include "../net/TcpServer.h"
#include "../platform/InputInject.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace sdk {

namespace {

uint64_t g_seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();

int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Re-anchor the relative-motion tracker to the compositor's real pointer only
// after the client has been quiet this long. DURING an input burst the tracker
// is exact (every delta is computed from it) while the metadata lags the
// pointer by a frame or two - merging the stale meta into the tracker on every
// event/tick makes each motion overshoot by that lag and the remote cursor
// visibly runs ahead and snaps back. Idle windows are safe: the pointer is
// stopped, the meta has settled, and any physical-mouse movement on the host
// since the last event gets absorbed here.
static constexpr int64_t kReanchorIdleUs = 300000;

uint64_t NextRandom()
{
    // xorshift64 (thread-safe enough for nonces/tokens)
    uint64_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_seed = x;
    return x;
}

void FillRandom(uint8_t* out, size_t len)
{
    for (size_t i = 0; i < len; i += 8) {
        const uint64_t v = NextRandom();
        for (size_t j = 0; j < 8 && i + j < len; ++j) {
            out[i + j] = (uint8_t)(v >> (j * 8));
        }
    }
}

} // namespace

// ---------------------------------------------------------------- HostSession

HostSession::HostSession(Socket::sock_t fd, std::string peerIp, uint16_t filePort,
                         const Callbacks& cb)
    : m_fd(fd), m_peerIp(std::move(peerIp)), m_filePort(filePort), m_cb(cb)
{
}

HostSession::~HostSession()
{
    Stop();
}

void HostSession::Start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_thread = std::thread(&HostSession::Run, this);
}

void HostSession::TakeFd()
{
    // transfer the socket to thread-local ownership: from now on only this
    // thread may Close() it; Stop() only Shutdown()s to wake blocked recv
    m_threadFd = m_fd;
    m_fd = Socket::kInvalid;
}

void HostSession::Stop()
{
    // Always join the session thread (it may still be finishing even when
    // m_running is already false); std::thread destructor aborts otherwise.
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning) {
        // wake the blocked recv WITHOUT releasing the fd number (Close() from
        // another thread can race with fd reuse and kill a newer connection);
        // the session thread owns the fd and closes it itself
        Socket::Shutdown(m_fd);
        Socket::Shutdown(m_threadFd);
        m_askCv.notify_all();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // stop the capture thread too (it may be blocked in Capture(); the
    // SCScreenshotManager call has its own 3s timeout)
    m_captureRunning = false;
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
}

void HostSession::RespondAsk(bool accept)
{
    {
        std::lock_guard<std::mutex> lock(m_askMutex);
        m_askDecided = true;
        m_askAccept = accept;
    }
    m_askCv.notify_all();
}

void HostSession::Run()
{
    TakeFd();
    RunImpl();
    CloseThreadFd(); // thread owns the fd; no-op if RunImpl already closed it
    m_finished = true;
}

void HostSession::CloseThreadFd()
{
    Socket::Close(m_threadFd);
    m_threadFd = Socket::kInvalid;
}

void HostSession::RunImpl()
{
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;

    // ---- 1. Hello ----
    Frame frame;
    if (!RecvFrame(m_threadFd, frame, 10000) || frame.type != MsgType::Hello) {
        CloseThreadFd();
        m_running = false;
        if (m_cb.onClosed) m_cb.onClosed();
        return;
    }
    PayloadReader r(frame.payload.data(), frame.payload.size());
    std::string deviceName;
    if (!r.GetString(deviceName) || !r.Ok()) {
        CloseThreadFd();
        m_running = false;
        if (m_cb.onClosed) m_cb.onClosed();
        return;
    }
    m_view = (ConnMode)r.GetU8() == ConnMode::View;
    m_peerRes = r.GetU8();
    m_peerFps = r.GetU8();
    m_peerCodec = r.GetU8(); // 0=PNG tiles 1=H.264 2=VP9 3=AV1 4=OpenH.264
    if (r.Ok() && r.Remaining() >= 2) {
        m_clientFilePort = r.GetU16(); // client's file-channel port (downloads)
    }
    // trailer (absent = 0 for older clients): 1 = hide the remote cursor in
    // the stream - wayland embedded hosts re-capture with cursor_mode=1
    if (r.Ok() && r.Remaining() >= 1) {
        m_peerHideCursor = r.GetU8() != 0;
    }

    const AppConfig& cfg = App::Instance().Config();
    const std::string password = ui::StringConvert::TToUTF8(cfg.password);

    // tell the client a manual-accept dialog is showing
    bool manualGranted = false;
    if (cfg.manualAccept) {
        PayloadWriter w;
        w.PutString(ui::StringConvert::TToUTF8(App::GetHostName()));
        SendFrame(m_threadFd, MsgType::AcceptRequest, w.Result());

        AskInfo info;
        info.deviceName = ui::StringConvert::UTF8ToT(deviceName);
        info.ip = ui::StringConvert::UTF8ToT(m_peerIp);
        info.view = m_view;
        if (m_cb.onAsk) {
            // posted by the caller side (MainWindow wraps with PostTask)
            m_cb.onAsk(this, info);
        }
        {
            std::unique_lock<std::mutex> lock(m_askMutex);
            if (!m_askDecided) {
                m_askCv.wait_for(lock, std::chrono::seconds(120));
            }
        }
        fprintf(stderr, "[host] ask decision decided=%d accept=%d\n",
                (int)m_askDecided, (int)m_askAccept);
        fflush(stderr);
        manualGranted = m_askDecided && m_askAccept;
        if (!m_askDecided || !m_askAccept) {
            // rejected or timed out
            PayloadWriter w;
            w.PutU8((uint8_t)AuthResultCode::Rejected);
            w.PutU16(0);
            uint8_t zero[32] = {0};
            w.PutBytes(zero, 32);
            SendFrame(m_threadFd, MsgType::AuthResult, w.Result());
            CloseThreadFd();
            m_running = false;
            if (m_cb.onClosed) m_cb.onClosed();
            return;
        }
    }

    if (!manualGranted) {
        // ---- 2. challenge ----
        uint8_t nonce[16];
        FillRandom(nonce, sizeof(nonce));
        PayloadWriter w1;
        w1.PutBytes(nonce, sizeof(nonce));
        if (!SendFrame(m_threadFd, MsgType::Challenge, w1.Result())) {
            CloseThreadFd();
            m_running = false;
            if (m_cb.onClosed) m_cb.onClosed();
            return;
        }

        // ---- 3. auth ----
        if (!RecvFrame(m_threadFd, frame, 10000) || frame.type != MsgType::Auth ||
            frame.payload.size() != Sha256::kDigestSize) {
            CloseThreadFd();
            m_running = false;
            if (m_cb.onClosed) m_cb.onClosed();
            return;
        }

        uint8_t expected[Sha256::kDigestSize];
        {
            std::string challenge;
            challenge.assign((const char*)nonce, sizeof(nonce));
            challenge += password;
            Sha256::Hash(challenge, expected);
        }
        ok = std::memcmp(expected, frame.payload.data(), Sha256::kDigestSize) == 0;
        fprintf(stderr, "[host] auth: password_len=%zu match=%d\n", password.size(), (int)ok);
        if (!ok) {
            // diagnostic: identify which password the client actually used
            char nonceHex[33] = {0};
            char digestHex[65] = {0};
            for (int i = 0; i < 16; ++i) {
                snprintf(nonceHex + i * 2, 3, "%02x", nonce[i]);
            }
            for (int i = 0; i < Sha256::kDigestSize; ++i) {
                snprintf(digestHex + i * 2, 3, "%02x", frame.payload.data()[i]);
            }
            fprintf(stderr, "[host] auth-mismatch nonce=%s digest=%s\n", nonceHex, digestHex);
        }
        fflush(stderr);

    }
    else {
        ok = true;
        // the user explicitly approved this connection: no password needed
        fprintf(stderr, "[host] manual accept granted, password check skipped\n");
        fflush(stderr);
    }

    FillRandom(m_token, sizeof(m_token));
    if (ok) {
        // register the session token so the host can send files back
        App::Instance().FileTx().SetSessionToken(m_token);
    }

    PayloadWriter w2;
    w2.PutU8((uint8_t)(ok ? AuthResultCode::Ok : AuthResultCode::WrongPassword));
    w2.PutU16(m_filePort);
    w2.PutBytes(m_token, sizeof(m_token));
    SendFrame(m_threadFd, MsgType::AuthResult, w2.Result());

    if (!ok) {
        CloseThreadFd();
        m_running = false;
        if (m_cb.onClosed) m_cb.onClosed();
        return;
    }

    m_connected = true;
    // re-anchor the injected cursor so a stale tracked position from a
    // previous session cannot shift the pointer (intermittent offset fix)
    InputInjector::ResetCursor();
    if (m_cb.onStatus) {
        // "xxx 正在连接本机" (format via UTF-8 to work on narrow + wide builds)
        const std::string fmt = ui::StringConvert::TToUTF8(SDK_TR("host.sessionOf"));
        const std::string text = ui::StringUtil::Printf(fmt.c_str(), deviceName.c_str());
        m_cb.onStatus(ui::StringConvert::UTF8ToT(text), true);
    }

    // ---- screen capture (may fall back to the test pattern) ----
    m_capturer = std::make_unique<Capturer>();
    m_captureRunning = true;
    if (m_capturer->Start(cfg.extendScreen)) {
        m_usingTestPattern = false;
        m_logicalW = m_capturer->GetWidth();
        m_logicalH = m_capturer->GetHeight();
    }
    else {
        m_usingTestPattern = true;
        m_testPattern = std::make_unique<TestPatternSource>();
        // logical size will be set by the first encoded frame
        // surface the failure on the host status bar (permission hint when
        // the cause is TCC, otherwise the capturer error verbatim)
        if (m_cb.onStatus) {
            const std::string why = m_capturer->GetError();
            if (why.find("permission") != std::string::npos) {
                m_cb.onStatus(SDK_TR("remote.permissionHint"), false);
            }
            else {
                const std::string fmt = ui::StringConvert::TToUTF8(SDK_TR("host.captureError"));
                const std::string text = ui::StringUtil::Printf(fmt.c_str(), why.c_str());
                m_cb.onStatus(ui::StringConvert::UTF8ToT(text), false);
            }
        }
    }
    m_captureThread = std::thread(&HostSession::CaptureLoop, this);

#ifdef STARDESK_HAS_AUDIO
    // ---- desktop audio: PipeWire sink monitor -> Opus -> AudioFrame ----
    // (started after the screen capture; the client plays it alongside the
    // video. Runs regardless of control/view mode - both watch and listen.)
    m_audio = std::make_unique<AudioCapture>();
    if (m_audio->Start(48000, 2, 960, [this](const uint8_t* data, size_t len) {
            // the callback runs on the audio capture thread
            if (!m_audioInitSent) {
                PayloadWriter w;
                w.PutU8((uint8_t)AudioCodec::Opus);
                w.PutU32(48000);
                w.PutU8(2);
                w.PutU32(960); // samples per packet (20ms)
                SendFrameLocked(MsgType::AudioInit, w.Result());
                m_audioInitSent = true;
            }
            PayloadWriter w;
            w.PutU16(m_audioSeq++);
            w.PutBytes(data, len);
            SendFrameLocked(MsgType::AudioFrame, w.Result());
        })) {
        fprintf(stderr, "[host] desktop audio streaming enabled\n");
        fflush(stderr);
    } else {
        m_audio.reset(); // no sink/PipeWire - run video-only
        fprintf(stderr, "[host] desktop audio unavailable - video only\n");
        fflush(stderr);
    }
#endif

    // ---- 4. streaming loop (heartbeat for now; screen/input in later steps) ----
    const auto lastRecv = std::make_shared<std::chrono::steady_clock::time_point>(t0);
    const auto lastPing = std::make_shared<std::chrono::steady_clock::time_point>(t0);
    while (m_running) {
        // heartbeat: ping every 3s, drop after 15s of silence
        const auto now = std::chrono::steady_clock::now();
        if (now - *lastPing > std::chrono::seconds(3)) {
            PayloadWriter w;
            w.PutU64((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
            SendFrameLocked(MsgType::Ping, w.Result());
            *lastPing = now;
        }
        if (now - *lastRecv > std::chrono::seconds(15)) {
            break; // dead peer
        }

        if (!Socket::WaitReadable(m_threadFd, 100)) {
            continue;
        }
        if (!RecvFrame(m_threadFd, frame, 5000)) {
            break;
        }
        switch (frame.type) {
        case MsgType::Ping: {
            SendFrameLocked(MsgType::Pong, frame.payload);
            break;
        }
        case MsgType::Pong: {
            *lastRecv = std::chrono::steady_clock::now();
            break;
        }
        case MsgType::InputEvent: {
            static uint32_t sInputCount = 0;
            if ((++sInputCount & 0x3F) == 1) {
                fprintf(stderr, "[host] input event #%u (lw=%d lh=%d)\n",
                        sInputCount, m_logicalW.load(), m_logicalH.load());
            }
            HandleInputEvent(frame.payload); // view-only sessions gate inside
            break;
        }
        case MsgType::CodecSwitch: {
            // {u8 codec} - switch the stream codec live; EncodeAndSendVideo
            // re-sends VideoInit for the new codec on the next frame
            if (!frame.payload.empty() && frame.payload[0] <= 4) {
                m_peerCodec = frame.payload[0];
            }
            break;
        }
        case MsgType::CursorModeReq: {
            // {u8 hide} - the client toggled local/remote-cursor mode. With
            // cursor_mode=4 (metadata) the visibility is applied client-side
            // (host stream never contains the pointer), so this is a no-op
            // for the capture - kept for older embedded hosts.
            fprintf(stderr, "[host] cursor-mode req: hide=%d (no re-capture)\n",
                    !frame.payload.empty() && frame.payload[0] != 0 ? 1 : 0);
            break;
        }
        case MsgType::DirListReq: {
            HandleDirList(frame.payload);
            break;
        }
        case MsgType::FileDownloadReq: {
            HandleFileDownload(frame.payload);
            break;
        }
        case MsgType::Disconnect: {
            m_running = false;
            break;
        }
        default:
            break;
        }
        *lastRecv = std::chrono::steady_clock::now();
    }

    // stop the capture thread (it may be blocked in Capture(); the
    // SCScreenshotManager call has its own 3s timeout)
    m_captureRunning = false;
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    m_capturer.reset();
    m_testPattern.reset();

#ifdef STARDESK_HAS_AUDIO
    // stop the desktop-audio capture (joins the audio thread)
    if (m_audio) {
        m_audio->Stop();
        m_audio.reset();
    }
#endif

    Cleanup();
    if (m_cb.onStatus) {
        m_cb.onStatus(SDK_TR("host.ready"), true);
    }
    if (m_cb.onClosed) {
        m_cb.onClosed();
    }
}

void HostSession::Cleanup()
{
    m_connected = false;
    m_running = false;
}

void HostSession::HandleInputEvent(const std::vector<uint8_t>& payload)
{
    // view-only sessions never inject (server-side enforcement)
    if (m_view) {
        return;
    }
    PayloadReader r(payload.data(), payload.size());
    if (!r.Ok()) {
        return;
    }
    const uint8_t kind = r.GetU8();
    const uint16_t xNorm = r.GetU16();
    const uint16_t yNorm = r.GetU16();
    const uint8_t buttons = r.GetU8();
    const int16_t wheelDelta = (int16_t)r.GetU16();
    const uint16_t vkCode = r.GetU16();
    const uint8_t modifiers = r.GetU8();
    (void)buttons;

    const int lw = m_logicalW.load();
    const int lh = m_logicalH.load();
    if (lw <= 0 || lh <= 0) {
        // screen size not known yet (capture still starting): a normalized
        // position cannot be mapped - dropping avoids moving the cursor to
        // the screen origin
        static uint32_t sDropZero = 0;
        if ((++sDropZero & 0x3F) == 1) {
            fprintf(stderr, "[host] input dropped: logical size %dx%d\n", lw, lh);
        }
        return;
    }
    static const std::vector<std::string> sLocalIps = Socket::GetLocalIPv4s();
    if (!m_peerLocalChecked) {
        m_peerLocalChecked = true;
        m_peerIsLocal = m_peerIp == "127.0.0.1" || m_peerIp == "::1" ||
                        std::find(sLocalIps.begin(), sLocalIps.end(), m_peerIp) != sLocalIps.end();
    }

    const double nx = (double)xNorm / 65535.0;
    const double ny = (double)yNorm / 65535.0;

    // input-time realignment (quiet-window): merge the injector's tracker
    // with the compositor's real pointer before the FIRST event after a
    // pause - absorbs any physical-mouse movement on the host since the last
    // event (the following relative motion/click starts at the real pointer).
    // During an ongoing input burst the metadata lags the pointer by a frame
    // or two, so realigning per event would overshoot every delta (flicker);
    // mid-burst the tracker (lock-step with the injected motion) is exact.
    const int64_t nowUs = NowUs();
    const bool firstAfterPause =
        nowUs - (int64_t)m_lastInputUs.load() > kReanchorIdleUs;
    if (firstAfterPause && kind <= 2 && m_capturer != nullptr) {
        int rcx = 0, rcy = 0;
        bool rcv = true;
        if (m_capturer->GetCursor(rcx, rcy, rcv) && rcx >= 0 && rcy >= 0) {
            InputInjector::ResetCursorTo(rcx, rcy);
            m_cursorCalibrated.store(true);
        }
    }
    if (kind <= 2) {
        m_lastInputUs.store(nowUs);
    }

    // same-machine feedback guard: never inject into a StarDesk window
    // (any instance). A click/key aimed into the remote-view window would
    // land on it and re-trigger an input event (infinite loop); the windows
    // are excluded from the capture anyway, so they are invisible in the
    // stream and there is nothing meaningful to operate there.
    // Only relevant when the peer is on THIS machine: a remote peer must be
    // able to operate whatever is on the host screen, including areas that
    // our (visible) window happens to cover.
    if (m_capturer && m_peerIsLocal) {
        const int pw = m_capturer->GetWidth();   // physical points
        const int ph = m_capturer->GetHeight();
        const int ox = m_capturer->GetOriginX();
        const int oy = m_capturer->GetOriginY();
        if (kind <= 2) { // mouse move / button / wheel carry a position
            if (InputInjector::IsOwnWindowAt(ox + nx * pw, oy + ny * ph)) {
                return;
            }
        }
        else if (InputInjector::IsOwnAppFocused()) {
            return;
        }
    }

    switch (kind) {
    case 0: { // mouse move
        static uint32_t sInMv = 0;
        if ((++sInMv & 0x7F) == 1) {
            fprintf(stderr, "[host] move n=(%.3f,%.3f) target=(%d,%d) lw=%d lh=%d\n",
                    nx, ny, (int)(nx * lw), (int)(ny * lh), lw, lh);
        }
        InputInjector::MoveTo(nx, ny, lw, lh);
        break;
    }
    case 1: { // mouse button: bit0..2 in `buttons`
        // Absolute-click semantics (press only): the compositor metadata
        // reports the DISPLAYED cursor, which on a host with a physical
        // mouse is that physical pointer - never ours. So the click starts
        // from the injector's own absolute target: a normalized MoveTo before
        // the button. Relative motions/buttons go over the same ordered
        // stream, so the button fires after its preceding motion and always
        // lands exactly at the client's coordinates. The RELEASE stays bare:
        // mid-drag the pointer may be behind too, but a release move would
        // teleport the drag (the window follows the pointer leap, landing the
        // window at the screen edge).
        if (buttons & 0x10) {
            InputInjector::MoveTo(nx, ny, lw, lh);
            // click diagnosis: target vs injector tracker vs real cursor
            static uint32_t sClickDiag = 0;
            if ((++sClickDiag & 0x1F) == 1) {
                int tx = (int)(nx * lw), ty = (int)(ny * lh);
                int ix = -1, iy = -1, rx = -1, ry = -1;
                bool rv = true;
                InputInjector::GetTrackedPos(ix, iy);
                if (m_capturer != nullptr) {
                    m_capturer->GetCursor(rx, ry, rv);
                }
                fprintf(stderr,
                        "[host] click diag target=(%d,%d) tracked=(%d,%d) real=(%d,%d)\n",
                        tx, ty, ix, iy, rx, ry);
            }
        }
        if ((buttons & 0x01) && (buttons & 0x10)) InputInjector::Button(true, 0);
        if ((buttons & 0x02) && (buttons & 0x10)) InputInjector::Button(true, 1);
        if ((buttons & 0x04) && (buttons & 0x10)) InputInjector::Button(true, 2);
        if ((buttons & 0x01) && !(buttons & 0x10)) InputInjector::Button(false, 0);
        if ((buttons & 0x02) && !(buttons & 0x10)) InputInjector::Button(false, 1);
        if ((buttons & 0x04) && !(buttons & 0x10)) InputInjector::Button(false, 2);
        break;
    }
    case 2: // wheel
        InputInjector::Wheel(wheelDelta);
        break;
    case 3: // key down
        SyncModifiers(modifiers);
        InputInjector::Key(vkCode, true);
        break;
    case 4: // key up
        SyncModifiers(modifiers);
        InputInjector::Key(vkCode, false);
        break;
    default:
        break;
    }
}

// Keep the remote modifier keys (Shift/Ctrl/Alt/Win) in sync with the
// client's modifier bitmask. The client only sends the MAIN key plus a
// modifiers field; without this the injected key would be a bare keycode
// and every Shift/Ctrl/Alt combo would come out wrong (Shift+2 -> '2',
// Shift+a -> 'a', Ctrl+C -> 'c'). The modifier keys are injected as real
// key events (down/up) into the same keyboard device, before the main key.
void HostSession::SyncModifiers(uint8_t modifiers)
{
    const uint8_t delta = modifiers ^ m_lastModifiers;
    if (delta == 0) {
        return;
    }
    static const struct {
        uint8_t bit;
        uint16_t vk;
    } kMods[] = {
        { 0x01, ui::kVK_SHIFT },
        { 0x02, ui::kVK_CONTROL },
        { 0x04, ui::kVK_MENU },   // Alt
        { 0x08, ui::kVK_LWIN },   // Win/Super
    };
    // release the modifiers that went up first, then press the new ones
    for (const auto& m : kMods) {
        if ((delta & m.bit) && !(modifiers & m.bit)) {
            InputInjector::Key(m.vk, false);
        }
    }
    for (const auto& m : kMods) {
        if ((delta & m.bit) && (modifiers & m.bit)) {
            InputInjector::Key(m.vk, true);
        }
    }
    m_lastModifiers = modifiers;
}

// file browser: the remote side is restricted to the host user's home
// directory (a LAN peer must not browse the whole machine)
namespace {

std::string HomeBaseDir()
{
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) {
        return p;
    }
#else
    if (const char* p = std::getenv("HOME")) {
        return p;
    }
#endif
    return std::string("/");
}

bool UnderBase(const std::filesystem::path& p, const std::filesystem::path& base)
{
    const auto rp = p.lexically_normal();
    const auto rb = base.lexically_normal();
    return rp == rb || rp.string().rfind(rb.string() + std::filesystem::path::preferred_separator, 0) == 0;
}

} // namespace

void HostSession::HandleDirList(const std::vector<uint8_t>& payload)
{
    PayloadReader r(payload.data(), payload.size());
    std::string path;
    if (!r.GetString(path) || !r.Ok()) {
        return;
    }
    const std::string home = HomeBaseDir();
    std::filesystem::path base(home);
    std::filesystem::path dir = path.empty() ? base : std::filesystem::path(path);
    if (!dir.is_absolute()) {
        dir = base / dir; // browser paths are relative to home
    }
    // normalize + restrict to home
    if (!UnderBase(dir, base)) {
        dir = base;
    }
    // canonicalize when possible (symlinks, "..")
    std::error_code ec;
    const auto canon = std::filesystem::canonical(dir, ec);
    if (!ec) {
        dir = canon;
    }
    if (!UnderBase(dir, base)) {
        dir = base;
    }
    const std::string dirStr = dir.string();

    struct Entry {
        std::string name;
        bool isDir = false;
        uint64_t size = 0;
    };
    std::vector<Entry> entries;
    ec.clear();
    std::filesystem::directory_iterator it(dir, ec);
    if (!ec) {
        for (const auto& de : it) {
            std::error_code e2;
            const std::string name = de.path().filename().string();
            if (name.empty() || name[0] == '.') {
                continue; // skip dotfiles for a cleaner browser
            }
            Entry e;
            e.name = name;
            e.isDir = de.is_directory(e2);
            if (!e.isDir) {
                e.size = (uint64_t)de.file_size(e2);
            }
            entries.push_back(std::move(e));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) {
            return a.isDir; // dirs first
        }
        return a.name < b.name;
    });

    PayloadWriter w;
    w.PutU8(1); // ok
    w.PutString(dirStr);
    w.PutU16((uint16_t)entries.size());
    for (const Entry& e : entries) {
        w.PutU8(e.isDir ? 1 : 0);
        w.PutU64(e.size);
        w.PutString(e.name);
    }
    SendFrameLocked(MsgType::DirListResp, w.Result());
}

void HostSession::HandleFileDownload(const std::vector<uint8_t>& payload)
{
    PayloadReader r(payload.data(), payload.size());
    std::string path;
    std::string localDir;
    if (!r.GetString(path) || !r.GetString(localDir) || !r.Ok()) {
        return;
    }
    if (m_clientFilePort == 0) {
        return; // peer does not listen for file transfers
    }
    const std::string home = HomeBaseDir();
    std::filesystem::path base(home);
    std::filesystem::path file(path);
    if (!file.is_absolute()) {
        file = base / file; // browser paths are relative to home
    }
    if (!UnderBase(file, base)) {
        return;
    }
    std::error_code ec;
    const auto canon = std::filesystem::canonical(file, ec);
    // files AND folders (the send side walks folders recursively)
    if (ec || !std::filesystem::exists(canon, ec)) {
        return;
    }
    std::vector<ui::FilePath> files;
    files.emplace_back(ui::FilePath(ui::StringConvert::UTF8ToT(canon.string())));
    std::string err;
    // push the file over the file channel to the peer's listener
    const bool ok = App::Instance().FileTx().SendFiles(
        m_peerIp, m_clientFilePort, m_token, files, localDir,
        [](const FileTransfer::Item&) {}, err);
    fprintf(stderr, "[HostSession] download %s -> %s:%u ok=%d err=%s\n",
            canon.c_str(), m_peerIp.c_str(), (unsigned)m_clientFilePort,
            ok ? 1 : 0, err.c_str());
    fflush(stderr);
}

bool HostSession::SendFrameLocked(MsgType type, const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (!m_connected) {        return false;
    }
    return SendFrame(m_threadFd, type, payload);
}

// stream variant: short (250ms) send timeout so a congested link drops the
// current frame instead of freezing the whole pipeline for seconds
bool HostSession::SendFrameLockedStream(MsgType type, const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (!m_connected) {
        return false;
    }
    return SendFrameStream(m_threadFd, type, payload);
}

// Video mode: (re)create the codec-specific encoder on size change (sending
// VideoInit; H.264 carries SPS/PPS, VP9/AV1 keyframes are self-contained),
// then encode the frame and send it as one VideoFrame. Returns false when
// the requested codec is unavailable so the caller falls back to PNG.
bool HostSession::EncodeAndSendVideo(uint8_t codec, const uint8_t* rgba, int w,
                                     int h, int fps)
{
    std::vector<uint8_t> sps, pps;
    bool ready = false;

    auto sendVideoInit = [&]() {
        PayloadWriter wInit;
        wInit.PutU8(codec);
        wInit.PutU32((uint32_t)w);
        wInit.PutU32((uint32_t)h);
        wInit.PutU32((uint32_t)sps.size());
        wInit.PutU32((uint32_t)pps.size());
        wInit.PutBytes(sps.data(), sps.size());
        wInit.PutBytes(pps.data(), pps.size());
        // cursorInStream: 1 when the capture already contains the real
        // pointer (Wayland ScreenCast embedded); the client must not draw an
        // additional overlay in that case.
        wInit.PutU8(m_capturer && m_capturer->CursorEmbedded() ? 1 : 0);
        SendFrameLockedStream(MsgType::VideoInit, wInit.Result());
    };

    std::vector<uint8_t> encoded;
    if (codec == (uint8_t)VideoCodec::H264) {
        if (m_h264 == nullptr || m_h264->Width() != w || m_h264->Height() != h) {
            m_h264 = std::make_unique<H264Encoder>();
            if (!m_h264->Init(w, h, fps)) {
                m_h264.reset(); // x264 unavailable: caller falls back to PNG
                return false;
            }
            m_h264->GetSpsPps(sps, pps);
        }
        if (m_videoInitCodec != codec) { // live codec switch: re-negotiate
            m_h264->GetSpsPps(sps, pps);
            sendVideoInit();
            m_videoInitCodec = codec;
        }
        ready = m_h264->Encode(rgba, w, h, encoded);
    }
    else if (codec == (uint8_t)VideoCodec::Vp9) {
        if (m_vpx == nullptr || m_vpx->Width() != w || m_vpx->Height() != h) {
            m_vpx = std::make_unique<VpxEncoder>();
            if (!m_vpx->Init(w, h, fps)) {
                m_vpx.reset();
                return false;
            }
        }
        if (m_videoInitCodec != codec) {
            sendVideoInit(); // no sps/pps for VP9
            m_videoInitCodec = codec;
        }
        ready = m_vpx->Encode(rgba, w, h, encoded);
    }
    else if (codec == (uint8_t)VideoCodec::OpenH264) {
        if (m_openh264 == nullptr || m_openh264->Width() != w || m_openh264->Height() != h) {
            m_openh264 = std::make_unique<OpenH264Encoder>();
            if (!m_openh264->Init(w, h, fps)) {
                m_openh264.reset();
                return false;
            }
            m_openh264->GetSpsPps(sps, pps);
        }
        if (m_videoInitCodec != codec) { // live switch: new decoder needs IDR
            m_openh264->GetSpsPps(sps, pps);
            sendVideoInit();
            m_videoInitCodec = codec;
            m_openh264->RequestKeyFrame(); // force IDR so the decoder starts
        }
        ready = m_openh264->Encode(rgba, w, h, encoded);
    }
    else if (codec == (uint8_t)VideoCodec::Av1) {
        if (m_aom == nullptr || m_aom->Width() != w || m_aom->Height() != h) {
            m_aom = std::make_unique<AomEncoder>();
            if (!m_aom->Init(w, h, fps)) {
                m_aom.reset();
                return false;
            }
        }
        if (m_videoInitCodec != codec) {
            sendVideoInit(); // no sps/pps for AV1
            m_videoInitCodec = codec;
        }
        ready = m_aom->Encode(rgba, w, h, encoded);
    }
    else {
        return false;
    }

    if (!ready || encoded.empty()) {
        return false;
    }
    return SendFrameLockedStream(MsgType::VideoFrame, encoded);
}

// backpressure: pause briefly when the socket is congested (peer not reading),
// so the capture thread cannot monopolize the send lock forever. One frame
// period, not 250ms - a longer sleep made every network hiccup a visible
// multi-frame freeze.
static bool SendFrameLockedPaced(bool ok)
{
    if (!ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return ok;
}

void HostSession::CaptureLoop()
{
    FrameEncoder encoder(75);
    const int fps = std::max(1, std::min(60, m_peerFps.load()));
    const int periodMs = std::max(10, 1000 / fps);
    const int resTargetW = m_peerRes.load() == 1 ? 1280 : (m_peerRes.load() == 2 ? 1920 : 0);
    const int resTargetH = m_peerRes.load() == 1 ? 720 : (m_peerRes.load() == 2 ? 1080 : 0);
    int lastTargetW = -1, lastTargetH = -1;
    fprintf(stderr, "[host] capture loop: peer codec=%d fps=%d res=%dx%d\n",
            m_peerCodec.load(), fps, resTargetW, resTargetH);
    fflush(stderr);

    // A damage-driven Wayland stream produces no buffers while the remote
    // desktop is static; re-send the last frame once a second so the client
    // keeps refreshing (and stays visibly alive) instead of showing "receive 0".
    std::vector<uint8_t> keepaliveRgba;
    int keepaliveW = 0, keepaliveH = 0;
    auto lastFrameAt = std::chrono::steady_clock::time_point{};
    auto lastKeepaliveLog = std::chrono::steady_clock::time_point{};
    auto nextTick = std::chrono::steady_clock::now();
    int cursorTick = 0;
    // throttled diagnostics: real-time every 15s (NOT per-frame - the capture
    // rate can be far below the requested fps, e.g. 1fps portal screenshots)
    auto nextDiag = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int gotCount = 0, failCount = 0;
    while (m_captureRunning) {
        nextTick += std::chrono::milliseconds(periodMs);

        // switch the SCStream to the peer's requested resolution once
        // (aspect-preserving; the downscale below then becomes a no-op)
        if (resTargetW != lastTargetW || resTargetH != lastTargetH) {
            lastTargetW = resTargetW;
            lastTargetH = resTargetH;
            if (m_capturer) {
                m_capturer->SetTargetResolution(resTargetW, resTargetH);
            }
        }

        CaptureFrame frame;
        bool got = m_capturer && m_capturer->Capture(frame);
        if (!got && m_usingTestPattern && m_testPattern) {
            got = m_testPattern->Capture(frame, m_logicalW.load() > 0 ? m_logicalW.load() : 1280,
                                         m_logicalH.load() > 0 ? m_logicalH.load() : 720);
        }
        const auto loopNow = std::chrono::steady_clock::now();
        if (!got && !keepaliveRgba.empty() &&
            loopNow - lastFrameAt >= std::chrono::seconds(1)) {
            // static desktop: repeat the last frame once a second
            frame.rgba = keepaliveRgba;
            frame.width = keepaliveW;
            frame.height = keepaliveH;
            got = true;
            if (loopNow - lastKeepaliveLog >= std::chrono::seconds(10)) {
                lastKeepaliveLog = loopNow;
                fprintf(stderr, "[host] static desktop, re-sending the last frame (1 fps)\n");
                fflush(stderr);
            }
        }
        if (got) {
            keepaliveRgba = frame.rgba;
            keepaliveW = frame.width;
            keepaliveH = frame.height;
            lastFrameAt = std::chrono::steady_clock::now();
            // Keep the logical screen size up to date so input injection
            // can map normalised client coordinates to physical screen
            // pixels. Use the capturer's native resolution (1440×900), not
            // the downscaled frame size (1152×720 from EGL GPU blit).
            const int nw = m_capturer->GetWidth();
            const int nh = m_capturer->GetHeight();
            if (nw > 0 && nh > 0) {
                m_logicalW = nw;
                m_logicalH = nh;
            } else if (frame.width > 0 && frame.height > 0) {
                // fallback: capturer hasn't negotiated the format yet
                m_logicalW = frame.width;
                m_logicalH = frame.height;
            }
            const uint8_t* rgba = frame.rgba.data();
            int w = frame.width;
            int h = frame.height;
            // H.264 mode: encode the native frame directly (one VideoFrame
            // per frame; the client decodes via VideoToolbox). Falls back to
            // the tile/PNG path when x264 is unavailable.
            bool streamed = false;
            if (m_peerCodec.load() != 0) {
                // test hook: STARDESK_SOLID_COLOR=RRGGBB fills the frame with a
                // solid color before encoding - pixel-exact color verification
                // of the encode/decode pipeline without screen interference
                if (const char* sc = std::getenv("STARDESK_SOLID_COLOR")) {
                    int r = 0, g = 0, b = 0;
                    if (std::sscanf(sc, "%2x%2x%2x", &r, &g, &b) == 3) {
                        for (size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
                            frame.rgba[i] = (uint8_t)r;
                            frame.rgba[i + 1] = (uint8_t)g;
                            frame.rgba[i + 2] = (uint8_t)b;
                            frame.rgba[i + 3] = 255;
                        }
                    }
                }
                // test hook: STARDESK_TEST_QUADS=1 fills the frame with four
                // quadrants (TL red, TR green, BL blue, BR yellow)
                else if (std::getenv("STARDESK_TEST_QUADS") != nullptr) {
                    for (int yy = 0; yy < h; ++yy) {
                        for (int xx = 0; xx < w; ++xx) {
                            size_t o = ((size_t)yy * w + xx) * 4;
                            const bool top = yy < h / 2;
                            const bool left = xx < w / 2;
                            if (top && left) { frame.rgba[o] = 255; frame.rgba[o + 1] = 0; frame.rgba[o + 2] = 0; }
                            else if (top) { frame.rgba[o] = 0; frame.rgba[o + 1] = 255; frame.rgba[o + 2] = 0; }
                            else if (left) { frame.rgba[o] = 0; frame.rgba[o + 1] = 0; frame.rgba[o + 2] = 255; }
                            else { frame.rgba[o] = 255; frame.rgba[o + 1] = 255; frame.rgba[o + 2] = 0; }
                            frame.rgba[o + 3] = 255;
                        }
                    }
                }
                // downscale before encoding only when the peer asked for a
                // smaller-than-native resolution (resTargetW/H: 0=original,
                // 1=720p, 2=1080p). The old hardcoded 1280x720 made text look
                // blurry on 1080p hosts; with software x264 (CRF 22, veryfast)
                // native 1080p encodes fine on modern multi-core hosts.
                // (the buffer must outlive the encode - keep it in scope)
                std::vector<uint8_t> down;
                {
                    int dw = 0, dh = 0;
                    if (resTargetW > 0 &&
                        (w > resTargetW || h > resTargetH) &&
                        FrameEncoder::Downscale(rgba, w, h, resTargetW, resTargetH,
                                                down, dw, dh)) {
                        rgba = down.data();
                        w = dw;
                        h = dh;
                    }
                }
                streamed = EncodeAndSendVideo(m_peerCodec.load(), rgba, w, h, fps);
                if (!streamed) {
                    fprintf(stderr, "[host] codec=%d encode/send failed, using PNG tiles\n",
                            m_peerCodec.load());
                }
            }
            if (!streamed) {
                std::vector<uint8_t> down;
                int dw = 0, dh = 0;
                if (resTargetW > 0 &&
                    FrameEncoder::Downscale(rgba, w, h, resTargetW, resTargetH, down, dw, dh)) {
                    rgba = down.data();
                    w = dw;
                    h = dh;
                }
                std::vector<EncodedTile> tiles;
                const bool sizeChanged = encoder.EncodeFrame(rgba, w, h, tiles);
                if (sizeChanged) {
                    m_logicalW = w;
                    m_logicalH = h;
                    PayloadWriter wInit;
                    wInit.PutU32((uint32_t)w);
                    wInit.PutU32((uint32_t)h);
                    // cursorInStream: 1 when the capture already contains the
                    // real pointer (Wayland ScreenCast embedded)
                    wInit.PutU8(m_capturer && m_capturer->CursorEmbedded() ? 1 : 0);
                    if (!SendFrameLockedStream(MsgType::ScreenInit, wInit.Result())) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    }
            }
            // send the frame's tiles as one (or a few <=64KB) batched
            // ScreenTiles messages instead of one send() per tile - the old
            // path made hundreds of syscalls per frame and (with Nagle off)
            // flooded the wire with small packets; batching cuts it to a
            // handful of messages while keeping the ScreenEnd frame boundary
            constexpr size_t kTileChunk = 64 * 1024;
            std::vector<uint8_t> body; // tile entries of the current message
            body.reserve(16 * 1024);
            size_t msgCount = 0;
            bool congested = false;
            auto flushBatch = [&]() {
                if (msgCount == 0) {
                    return;
                }
                PayloadWriter w;
                w.PutU32((uint32_t)msgCount);
                w.PutBytes(body.data(), body.size());
                const bool ok = SendFrameLockedStream(MsgType::ScreenTiles, w.Result());
                if (!ok) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    congested = true;
                }
                body.clear();
                msgCount = 0;
            };
            for (const EncodedTile& t : tiles) {
                PayloadWriter wt;
                wt.PutU16((uint16_t)t.x);
                wt.PutU16((uint16_t)t.y);
                wt.PutU16((uint16_t)t.w);
                wt.PutU16((uint16_t)t.h);
                wt.PutU32((uint32_t)t.webp.size());
                wt.PutBytes(t.webp.data(), t.webp.size());
                const std::vector<uint8_t>& entry = wt.Result();
                body.insert(body.end(), entry.begin(), entry.end());
                ++msgCount;
                if (body.size() >= kTileChunk) {
                    flushBatch();
                    if (congested) {
                        break; // socket is congested; drop the rest of this batch
                    }
                }
            }
            flushBatch();
            // end-of-frame marker: the client renders only on this, so a
            // frame never mixes tiles from two batches (seams on fast scroll)
            if (!congested) {
                SendFrameLockedStream(MsgType::ScreenEnd, {});
            }
            }
            ++gotCount;
        }
        else {
            ++failCount;
        }
        // throttled diagnostics: every ~15s summarize what the loop did
        if (std::chrono::steady_clock::now() >= nextDiag) {
            nextDiag = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            fprintf(stderr, "[host] capture diag: got=%d fail=%d\n", gotCount, failCount);
            gotCount = 0;
            failCount = 0;
            fflush(stderr);
        }

        // cursor position at ~20fps (normalized to the logical screen)
        if (++cursorTick >= std::max(1, periodMs / 50)) {
            cursorTick = 0;
            int cx = 0, cy = 0;
            bool visible = true;
            bool havePos = false;
            // Re-align the tracker with the real pointer while the client is
            // idle (same quiet-window rule as the event path - the meta is
            // settled then, and physical-mouse motion gets absorbed). During
            // an active burst the tracker is the injected position and
            // re-merging a lagging meta here would cause the overshoot path.
            const bool clientIdle =
                NowUs() - (int64_t)m_lastInputUs.load() > kReanchorIdleUs;
            if (clientIdle && m_capturer != nullptr &&
                m_capturer->GetCursor(cx, cy, visible) && cx >= 0 && cy >= 0) {
                InputInjector::ResetCursorTo(cx, cy);
                m_cursorCalibrated.store(true);
            }
            // Wayland ScreenCast metadata (cursor_mode=4): the capturer
            // forwards KWin's REAL pointer - preferred over the injector
            // tracker, which on Wayland tracks only our own injected motions
            // and stays stale for physically moved pointers.
            if (m_capturer != nullptr && m_capturer->GetCursor(cx, cy, visible)) {
                havePos = true;
            }
            else if (m_capturer && InputInjector::GetCursorPos(cx, cy)) {
                havePos = true;
            }
            if (havePos) {
                const int ox = m_capturer->GetOriginX();
                const int oy = m_capturer->GetOriginY();
                // GetCursorPos returns PHYSICAL point coordinates; the
                // normalized position must use the physical screen size too
                // (the logical frame size is the captured pixel size, e.g.
                // 2x Retina - dividing by it would put the cursor at half
                // the true distance)
                const int pw = m_capturer->GetWidth();
                const int ph = m_capturer->GetHeight();
                 if (pw > 0 && ph > 0) {
                    static int sCursorDiag = 0;
                    if ((++sCursorDiag & 0x1F) == 1) {
                        fprintf(stderr, "[host] cursor source=(%d,%d) origin=(%d,%d) size=%dx%d\n",
                                cx, cy, ox, oy, pw, ph);
                    }
                    PayloadWriter wc;
                    wc.PutU16((uint16_t)std::max(0, std::min(65535,
                        (int)((double)(cx - ox) / pw * 65535.0))));
                    wc.PutU16((uint16_t)std::max(0, std::min(65535,
                        (int)((double)(cy - oy) / ph * 65535.0))));
                    wc.PutU8(visible ? 1 : 0);
                    SendFrameLockedStream(MsgType::CursorPos, wc.Result());
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_for(nextTick - now);
        }
        else {
            nextTick = now; // capture is slower than the requested fps: don't catch up
        }
    }
}

// ---------------------------------------------------------------- HostService

HostService::HostService()
{
}

HostService::~HostService()
{
    Stop();
}

bool HostService::Start(uint16_t port, const HostSession::Callbacks& cb)
{
    if (m_running) {
        return true;
    }
    m_cb = cb;
    // bind the host port; when it is taken (e.g. a second StarDesk instance
    // on this machine) fall back to the next free port in a small range so
    // both instances can host at once. Port() reports what was bound.
    m_port = 0;
    m_server = std::make_unique<TcpServer>();
    for (int i = 0; i < 10 && m_port == 0; ++i) {
        const uint32_t p = (uint32_t)port + (uint32_t)i;
        if (p > 65535) {
            break;
        }
        if (m_server->Start((uint16_t)p, [this](Socket::sock_t fd, const std::string& ip) {
                OnAccept(fd, ip);
            })) {
            m_port = (uint16_t)p;
        }
    }
    m_running = m_port != 0;
    return m_running;
}

void HostService::Stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    if (m_server) {
        m_server->Stop();
        m_server.reset();
    }
    std::vector<HostSessionPtr> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions.swap(m_sessions);
    }
    for (auto& s : sessions) {
        s->Stop();
    }
}

int HostService::SessionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_sessions.size();
}

HostSessionPtr HostService::FirstConnectedSession() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const HostSessionPtr& s : m_sessions) {
        if (s->IsConnected()) {
            return s;
        }
    }
    return nullptr;
}

bool HostService::IsRunning() const
{
    return m_running;
}

void HostService::OnAccept(Socket::sock_t fd, const std::string& peerIp)
{
    // the advertised file port is the app's actual bound file port
    // (HostService may have fallen back to a free port on this machine)
    HostSessionPtr session =
        std::make_shared<HostSession>(fd, peerIp, App::Instance().FileTx().BoundPort(), m_cb);
    session->Start();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // prune finished sessions (their threads have exited; safe to drop)
        m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
                                        [](const HostSessionPtr& s) { return s->IsFinished(); }),
                         m_sessions.end());
        m_sessions.push_back(session);
    }
}

} // namespace sdk
