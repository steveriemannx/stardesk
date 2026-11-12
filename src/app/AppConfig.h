#ifndef STARDESK_APP_APP_CONFIG_H_
#define STARDESK_APP_APP_CONFIG_H_

#include "dui/dui.h"

#include "../filetx/FileTransfer.h"
#include "../net/Discovery.h"

#include <vector>

namespace sdk {

enum class Lang; // from Language.h

/** Persistent StarDesk settings, stored as a key=value text file at
 *  ~/.stardesk.conf (UTF-8). Loaded at startup, saved on change.
 */
struct AppConfig {
    std::string password;          // host connection password
    uint16_t port = 7456;      // host TCP port (file port = port + 1)
    bool manualAccept = false; // true = ask me before each connection
    bool extendScreen = false; // share mode: false = mirror primary, true = span all
    std::string receiveDir;        // folder for received files (empty = default)
    bool darkTheme = false; // light by default
    int language = 0;          // index into sdk::Language
    int wantFps = 60;          // default client frame-rate request (24/30/60)
    int wantRes = 0;           // default client resolution request (0=original 1=720p 2=1080p)
    bool viewOnly = false;     // default client mode: false=control, true=view only
    int cursorMode = 0;            // 0=local, 1=both, 2=remote
    std::string deviceName;        // device name shown to peers
    /** LAN peers hidden from the discovery list ("ip:port" strings). */
    std::vector<std::string> hiddenDevices;
    bool headless = false;      // host without creating a local control window

    // test/automation hooks (env STARDESK_AUTO_* takes precedence):
    // auto_host non-empty = connect automatically on startup
    std::string autoHost;
    int autoPort = 0;
    std::string autoPassword;
    int autoRes = 0;    // -1 = unset (use wantRes)
    int autoFps = 0;    // -1 = unset (use wantFps)
    int autoCodec = 0;  // -1 = unset
    bool autoOpenBrowser = false; // STARDESK_OPEN_BROWSER

    /** Path of the config file (~/.stardesk.conf). */
    static ui::FilePath GetFilePath();
    /** Default receive folder: ~/Downloads/StarDesk. */
    static ui::FilePath GetDefaultReceiveDir();

    void Load();
    void Save() const;

    /** Generate a random numeric password (e.g. 8 digits). */
    static std::string GeneratePassword();
};

/** Listener for global UI switches (theme / language). Windows register
 *  themselves so the switch is applied live everywhere.
 */
class IAppListener {
public:
    virtual ~IAppListener() = default;
    /** Called on the UI thread when the theme changed. */
    virtual void OnThemeChanged() = 0;
    /** Called on the UI thread when the language changed. */
    virtual void OnLanguageChanged() = 0;
};

/** App-wide runtime singleton: config + services registry.
 *  Owned by the UI thread; services are started/stopped in OnInit/OnCleanup.
 */
class App {
public:
    static App& Instance();

    AppConfig& Config() { return m_config; }
    const AppConfig& Config() const { return m_config; }

    /** True after a full startup. */
    bool IsStarted() const { return m_started; }

    /** Called from MainThread::OnInit (UI thread). */
    void Init();
    /** Called from MainThread::OnCleanup (UI thread). */
    void Cleanup();

    /** Register/unregister a theme/language listener (UI thread only). */
    void AddListener(IAppListener* l);
    void RemoveListener(IAppListener* l);

    /** Global file-transfer service (both directions share it; the session
     *  token is refreshed after each auth). Started in MainThread::OnInit. */
    FileTransfer& FileTx() { return m_fileTx; }
    /** LAN discovery (beacon + listener), started by the main window. */
    Discovery& Disc() { return m_disc; }

    /** Switch theme; persists to config, notifies listeners. */
    void SetTheme(bool dark);
    /** Switch language; persists to config, notifies listeners. */
    void SetLanguage(Lang lang);

    /** Physical machine name (hostname). */
    static std::string GetHostName();

private:
    App() = default;
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    AppConfig m_config;
    bool m_started = false;
    std::vector<IAppListener*> m_listeners;
    FileTransfer m_fileTx;
    Discovery m_disc;
};

/** Post a task to the UI thread; no-op while the app is shutting down.
 *  Call from any worker thread. */
inline void PostToUI(const std::function<void()>& fn)
{
    if (App::Instance().IsStarted()) {
        ui::GlobalManager::Instance().Thread().PostTask(ui::kThreadUI, fn);
    }
}

} // namespace sdk

#endif // STARDESK_APP_APP_CONFIG_H_
