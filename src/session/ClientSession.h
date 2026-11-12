#ifndef STARDESK_SESSION_CLIENT_SESSION_H_
#define STARDESK_SESSION_CLIENT_SESSION_H_

#include "../net/Protocol.h"
#include "../net/Socket.h"
#include "../codec/VideoDecoder.h"
#ifdef STARDESK_HAS_OPUS
#include "../codec/OpusAudio.h"
#include "../platform/AudioPlayback.h"
#endif

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sdk {

// directory entry returned by the host's file browser (DirListResp)
struct FileEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
};

/** Client-side connection: connect + auth handshake, then the receive loop
 *  (screen stream in step 4, input sending in step 5). Owns its socket.
 */
class ClientSession {
public:
    enum class State {
        Idle,
        Connecting,     // TCP connect + Hello
        WaitingAccept,  // host is showing the manual-accept dialog
        Authenticating, // waiting for the challenge/auth result
        Connected,
        Failed,         // see LastError()
        Closed,
    };

    struct Options {
        std::string host;
        uint16_t port = kDefaultPort;
        std::string password;
        ConnMode mode = ConnMode::Control;
        int resCode = 0; // 0=original 1=720p 2=1080p
        int fps = 30;    // 24/30/60
        /** Reverse remote-assistance ("request help"): the host that invited us gave
         *  us a one-time token; it is sent in the Hello trailer instead of
         *  using a password. */
        bool useInvite = false;
        uint8_t inviteToken[32] = {0};
    };

    struct Callbacks {
        /** UI thread (posted): state transition, including failures.
         *  text is the human-readable state/error in the current language. */
        std::function<void(State state, const std::string& text)> onState;        /** Session thread: a new decoded frame is available. The callback
         *  is a SIGNAL (no payload): latest-wins rendering - the UI fetches
         *  the newest frame via TakeLatestFrame() and intermediate frames
         *  are dropped when the UI cannot keep up, so a slow renderer never
         *  blocks the receive thread (which would cap the stream rate). */
        std::function<void()> onFrame;
        /** Session thread: a new server-reported cursor position. */
        std::function<void(int cursorXN, int cursorYN)> onCursor;
        /** Session thread: ~1/s stream statistics. */
        std::function<void(int renderFps, int rxFps, int latencyMs)> onStats;
        /** Session thread: directory listing reply (file browser). */
        std::function<void(bool ok, const std::string& path,
                           const std::vector<FileEntry>& entries)> onDirList;
    };

    /** Latest complete decoded frame (latest-wins): the UI calls this from
     *  the posted onFrame task; returns false when no newer frame exists.
     *  The pending flag is cleared so the session posts again for the next
     *  frame. */
    bool TakeLatestFrame(int& w, int& h, std::vector<uint8_t>& rgba,
                         int& cursorXN, int& cursorYN);

    /** UI display size in logical pixels (0 = disabled). Frames are scaled
     *  to this size on the RECEIVE thread before being published, so the UI
     *  thread only blits 1:1 in paint (a per-frame full-window resize on the
     *  UI thread starves input events). Call after layout/resize. */
    void SetDisplaySize(int w, int h);

    explicit ClientSession(const Options& opt, Callbacks cb);
    ~ClientSession();

    void Start();
    void Stop();

    State GetState() const { return m_state; }
    const std::string& LastError() const { return m_lastError; }
    bool IsConnected() const { return m_state == State::Connected; }

    const std::string& GetHost() const { return m_opt.host; }
    const std::string& RemoteName() const { return m_remoteName; }
    uint16_t FilePort() const { return m_filePort; }
    const uint8_t* Token() const { return m_token; }
    bool IsViewMode() const { return m_mode == ConnMode::View; }
    int PeerFps() const { return m_peerFps; }
    int PeerRes() const { return m_peerRes; }
    /** True when the host's capture already contains the real pointer
     *  (Wayland ScreenCast cursor_mode=2 embedded): the UI must not draw
     *  an overlay from CursorPos (that position tracks the injection-only
     *  pointer on Wayland, not the real one). Set from the trailing byte of
     *  VideoInit/ScreenInit (absent = false for older hosts). */
    bool CursorInStream() const { return m_cursorInStream.load(); }
    /** Last CursorPos visible flag from the host (0 = the host's pointer is
     *  hidden, e.g. the Wayland compositor currently shows no cursor). */
    bool CursorVisible() const { return m_cursorVisible.load(); }

    /** Send one input event (control mode only; no-op in view mode). */
    void SendInput(const InputEventMsg& ev);
    /** Tell the host whether the remote cursor should be hidden in the
     *  stream (true in local-cursor mode; Wayland embedded hosts re-capture
     *  the screen with cursor_mode=1, so the real pointer is not painted). */
    void SendCursorMode(bool hide);
    // switch the stream codec live (H.264/VP9/AV1); the host re-sends
    // VideoInit and this session re-creates its decoder
    void RequestCodec(uint8_t codec);
    /** Register only the directory-listing callback (does not touch the
     *  frame/stats callbacks owned by the remote window). */
    void SetDirListCallback(
        std::function<void(bool, const std::string&, const std::vector<FileEntry>&)> cb);
    /** Ask the host to list a directory ("" = its home). Reply arrives via
     *  the onDirList callback. */
    void RequestDirList(const std::string& path);
    /** Ask the host to push `remotePath` to our file channel, saved into
     *  `localDir` ("" = our default receive dir). Progress shows up in the
     *  global FileTransfer item callback. */
    void RequestDownload(const std::string& remotePath, const std::string& localDir);

    /** Replace the onFrame/onStats callbacks at runtime (thread-safe; the
     *  remote window registers them after the connection is established). */
    void SetFrameCallbacks(Callbacks cb);

private:
    void Run();
    void RunImpl();
    void Cleanup();
    void SetState(State st, const std::string& text);
    void FailNet(const std::string& text);

    Options m_opt;
    Callbacks m_cb;
    mutable std::mutex m_cbMutex;

    std::thread m_thread;
    std::thread m_renderThread; // Parsec-style: repaints the latest frame at
                                // the negotiated rate even when the stream is
                                // idle (fps shows the render rate, not 0/1)
    std::atomic<bool> m_running{false};
    std::atomic<State> m_state{State::Idle};

    Socket::sock_t m_fd = Socket::kInvalid;
    std::string m_lastError;
    std::string m_remoteName;
    uint16_t m_filePort = 0;

    // latest-wins frame delivery (session thread writes, UI thread takes)
    std::mutex m_latestMutex;
    bool m_latestPending = false;
    std::vector<uint8_t> m_latestFrame;
    int m_latestW = 0;
    int m_latestH = 0;
    // receive-thread frame scaling target (UI display size; 0 = disabled)
    std::atomic<int> m_displayW{0};
    std::atomic<int> m_displayH{0};
    int m_latestCxn = -1;
    int m_latestCyn = -1;
    uint8_t m_token[32] = {0};
    ConnMode m_mode = ConnMode::Control;
    int m_peerFps = 0;
    int m_peerRes = 0;

    std::mutex m_sendMutex;

    // ---- decoded frame state (receive thread) ----
    std::mutex m_frameMutex;
    int m_frameW = 0;
    int m_frameH = 0;
    std::vector<uint8_t> m_frame;   // RGBA
    bool m_frameDirty = false;      // new tiles since the last onFrame
    int m_cursorXN = -1;
    int m_cursorYN = -1;
    std::atomic<bool> m_cursorInStream{false};
    std::atomic<bool> m_cursorVisible{true};

    // ---- video stream (VideoInit/VideoFrame) ----
    std::unique_ptr<VideoDecoder> m_decoder;
    bool m_dumpedFrame = false; // test hook: first-frame dump done

#ifdef STARDESK_HAS_OPUS
    // ---- audio stream (AudioInit/AudioFrame) ----
    std::unique_ptr<OpusAudioDecoder> m_audioDecoder;
    std::unique_ptr<AudioPlayback> m_audioPlayback;
    bool m_audioInit = false;
#endif

    // ---- stats (receive thread) ----
    std::chrono::steady_clock::time_point m_statsAt;
    int m_framesThisSecond = 0;
    int m_latencyMs = 0;
    std::chrono::steady_clock::time_point m_lastPingAt;
    std::chrono::steady_clock::time_point m_lastNotifyAt;

    void HandleScreenInit(const Frame& frame);
    void HandleDirListResp(const Frame& frame);
    void HandleScreenTile(const Frame& frame);
    void HandleScreenTiles(const Frame& frame);
    void HandleVideoInit(const Frame& frame);
    void HandleVideoFrame(const Frame& frame);
    void HandleAudioInit(const Frame& frame);
    void HandleAudioFrame(const Frame& frame);
    void HandleScreenEnd(const Frame& frame);
    void PublishFrame(int w, int h, std::vector<uint8_t>&& snapshot, int cxn, int cyn);
    void HandleCursorPos(const Frame& frame);
    void QueueTile(int x, int y, std::vector<uint8_t> png);

    // ---- parallel tile decode (worker pool, like the host encoder) ----
    struct DecodeJob {
        int x = 0;
        int y = 0;
        std::vector<uint8_t> png;
    };
    void DecodeWorkerMain();
    void StartDecodeWorkers();
    void StopDecodeWorkers();

    std::vector<std::thread> m_decodeWorkers;
    std::mutex m_decodeMutex;
    std::condition_variable m_decodeCv;
    std::condition_variable m_decodeDoneCv;
    std::vector<DecodeJob> m_decodeJobs;
    size_t m_decodePos = 0;
    int m_decodeLeft = 0;
    bool m_decodeShutdown = false;
    uint8_t* m_decodeFrame = nullptr; // points into m_frame during a batch
    int m_decodeW = 0;
    int m_decodeH = 0;
    void NotifyFrame();
    void UpdateStats();
};

} // namespace sdk

#endif // STARDESK_SESSION_CLIENT_SESSION_H_
