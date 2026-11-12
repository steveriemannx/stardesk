#ifndef STARDESK_SESSION_HOST_SESSION_H_
#define STARDESK_SESSION_HOST_SESSION_H_

#include "../net/Protocol.h"
#include "../net/Socket.h"
#include "../platform/Capturer.h"
#include "../codec/H264Encoder.h"
#include "../codec/OpenH264Encoder.h"
#include "../codec/VpxEncoder.h"
#include "../codec/AomEncoder.h"
#ifdef STARDESK_HAS_AUDIO
#include "../platform/wayland/AudioCapture.h"
#endif

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sdk {

/** Info shown in the manual-accept dialog (UI thread). */
struct AskInfo {
    std::string deviceName;
    std::string ip;
    bool view = false; // true = view-only request
};

/** One accepted host connection: runs the auth handshake on its own thread,
 *  then the heartbeat/screen/input loop. The screen stream (step 4) and input
 *  injection (step 5) hook into Run().
 */
class HostSession;
typedef std::shared_ptr<HostSession> HostSessionPtr;

class HostSession : public std::enable_shared_from_this<HostSession>
{
public:
    struct Callbacks {
        /** Session thread: host-side status line. */
        std::function<void(const std::string& text, bool ok)> onStatus;
        /** UI thread (posted): show the manual-accept dialog. The session is
         *  passed so the dialog can report the decision back. */
        std::function<void(HostSession* session, const AskInfo& info)> onAsk;
        /** Session thread: connection closed / never established. */
        std::function<void()> onClosed;
    };

    HostSession(Socket::sock_t fd, std::string peerIp, uint16_t filePort,
                const Callbacks& cb);
    ~HostSession();

    void Start();
    void Stop();

    /** UI thread: user's decision for the pending manual-accept dialog. */
    void RespondAsk(bool accept);

    bool IsViewMode() const { return m_view; }
    const std::string& PeerIp() const { return m_peerIp; }
    bool IsConnected() const { return m_connected; }
    /** Session token (file channel); also registers it on the global
     *  FileTransfer service so the host can send files back. */
    const uint8_t* Token() const { return m_token; }

private:
    void Run();
    void RunImpl();
    void TakeFd();
    void CloseThreadFd();
    void Cleanup();

    // ---- screen stream ----
    void CaptureLoop();
    void HandleInputEvent(const std::vector<uint8_t>& payload);
    void SyncModifiers(uint8_t modifiers);
    void HandleDirList(const std::vector<uint8_t>& payload);
    void HandleFileDownload(const std::vector<uint8_t>& payload);
    bool SendFrameLocked(MsgType type, const std::vector<uint8_t>& payload);
    bool SendFrameLockedStream(MsgType type, const std::vector<uint8_t>& payload);
    bool EncodeAndSendVideo(uint8_t codec, const uint8_t* rgba, int w, int h, int fps);

    Socket::sock_t m_fd = Socket::kInvalid;   // owned by the accept thread until TakeFd()
    Socket::sock_t m_threadFd = Socket::kInvalid; // owned by the session thread
    std::string m_peerIp;
    /** True when the peer runs on this same machine (feedback-loop guard). */
    bool m_peerIsLocal = false;
    bool m_peerLocalChecked = false;
    uint16_t m_filePort = 0; // this app's file-channel port (advertised in AuthResult)
    uint16_t m_clientFilePort = 0; // peer's file-channel port (from Hello)
    // remote-cursor visibility: 1 = the peer wants the cursor hidden in the
    // stream (local-cursor mode); the wayland portal re-capture uses
    // cursor_mode 1 vs 2 accordingly. Set from Hello/used by CursorModeReq.
    std::atomic<bool> m_peerHideCursor{false};
    // true once the injector's relative-motion tracker was aligned to the
    // real cursor position (ResetCursorTo) - set once per client session so
    // a reconnect re-calibrates after the tracker reset
    std::atomic<bool> m_cursorCalibrated{false};
    // last mouse input event time (steady_clock microseconds, session thread
    // writes / capture thread reads) - the tracker re-anchor from the
    // compositor's meta is allowed only after this long without input, so a
    // lagging meta can never overshoot an active drag
    std::atomic<int64_t> m_lastInputUs{0};
    // the capture loop restarts the source with the new cursor mode
    std::atomic<bool> m_captureRestart{false};
    Callbacks m_cb;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_view{false};
    std::atomic<bool> m_finished{false};

    // capture/encode state (owned by the capture thread once started)
    std::unique_ptr<Capturer> m_capturer;
    std::unique_ptr<TestPatternSource> m_testPattern;
    bool m_usingTestPattern = false;
    std::thread m_captureThread;
    std::atomic<bool> m_captureRunning{false};
    std::mutex m_sendMutex; // serializes sends from session + capture threads
    std::atomic<int> m_logicalW{0};
    std::atomic<int> m_logicalH{0};

    // input state (session thread)
    uint8_t m_lastModifiers = 0;
    uint8_t m_token[32] = {0};

public:
    /** True once the session thread has fully exited (safe to prune). */
    bool IsFinished() const { return m_finished; }

private:

    // manual-accept handshake
    std::mutex m_askMutex;
    std::condition_variable m_askCv;
    bool m_askDecided = false;
    bool m_askAccept = false;

    // stream state (steps 4/5)
    std::atomic<int> m_peerFps{30};
    std::atomic<int> m_peerRes{0};
    std::atomic<uint8_t> m_peerCodec{0}; // VideoCodec: 0=PNG tiles, 1/2/3=video
    uint8_t m_videoInitCodec = 0xFF;     // codec the client's decoder is on;
                                         // re-send VideoInit when it changes
    std::unique_ptr<H264Encoder> m_h264; // capture-loop owned (codec 1)
    std::unique_ptr<OpenH264Encoder> m_openh264; // capture-loop owned (codec 4, BSD)
    std::unique_ptr<VpxEncoder> m_vpx;   // capture-loop owned (codec 2)
    std::unique_ptr<AomEncoder> m_aom;   // capture-loop owned (codec 3)

#ifdef STARDESK_HAS_AUDIO
    // desktop audio: PipeWire sink monitor -> Opus -> AudioFrame. Owned by
    // the session thread; the packet callback runs on the audio thread.
    std::unique_ptr<AudioCapture> m_audio;
    bool m_audioInitSent = false; // audio-thread owned
    uint16_t m_audioSeq = 0;      // audio-thread owned
#endif
};

/** Owns the host listener: one TcpServer + one HostSession per connection. */
class HostService {
public:
    HostService();
    ~HostService();

    /** Bind + start the listener; false when the port is unavailable. */
    bool Start(uint16_t port, const HostSession::Callbacks& cb);
    void Stop();

    bool IsRunning() const;
    uint16_t Port() const { return m_port; }
    int SessionCount() const;
    /** First connected session (for host-side file sends); null when idle. */
    HostSessionPtr FirstConnectedSession() const;

private:
    void OnAccept(Socket::sock_t fd, const std::string& peerIp);

    std::unique_ptr<class TcpServer> m_server;
    mutable std::mutex m_mutex;
    std::vector<HostSessionPtr> m_sessions;
    HostSession::Callbacks m_cb;
    uint16_t m_port = 0;
    std::atomic<bool> m_running{false};
};

} // namespace sdk

#endif // STARDESK_SESSION_HOST_SESSION_H_
