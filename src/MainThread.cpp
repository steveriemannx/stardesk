#include "MainThread.h"

#include "app/AppConfig.h"
#include "session/HostSession.h"
#include "ui/MainWindow.h"
#include "TextProbe.h"

#include <csignal>
#include <cstring>
#include <thread>
#include <memory>


#if defined(__APPLE__)
#include <mach/mach.h>
#include <ctime>
#endif

namespace sdk {

namespace {

std::unique_ptr<HostService> g_headlessHost;

/** Compile-time theme bootstrap: register the fonts and colors the UI uses,
 *  so the app renders even when the dui resource directory is absent
 *  (standalone distribution). Mirrors what global.xml would provide.
 */
void InitFontsAndColors()
{
    using namespace ui;

    auto& fm = GlobalManager::Instance().Font();
    fm.SetDefaultFontFamilyNames(
        "PingFang SC,Microsoft YaHei,Noto Sans CJK SC,Helvetica Neue,Arial,sans-serif");

    UiFont f;
    f.m_fontName = "system"; f.m_fontSize = 12;
    fm.AddFont("system_12", f, true);   // default
    f.m_fontSize = 14; fm.AddFont("system_14", f, false);
    f.m_fontSize = 16; fm.AddFont("system_16", f, false);
    f.m_fontSize = 18; fm.AddFont("system_18", f, false);
    f.m_fontSize = 20; fm.AddFont("system_20", f, false);

    f.m_bBold = true;
    f.m_fontSize = 12; fm.AddFont("system_bold_12", f, false);
    f.m_fontSize = 14; fm.AddFont("system_bold_14", f, false);
    f.m_fontSize = 16; fm.AddFont("system_bold_16", f, false);
    f.m_fontSize = 18; fm.AddFont("system_bold_18", f, false);
    f.m_bBold = false;

    // base colors referenced by dui's default rendering (theme colors used by
    // the UI itself come from sdk::Theme)
    auto& cm = GlobalManager::Instance().Color();
    cm.AddColor("white",  UiColor(0xFFFFFFFF));
    cm.AddColor("black",  UiColor(0xFF000000));
    cm.AddColor("darkcolor", UiColor(0xFF333333));
    cm.AddColor("lightcolor", UiColor(0xFF888888));
    cm.SetDefaultTextColor("darkcolor");
}

} // namespace

// ---------------------------------------------------------------- MainThread

MainThread::MainThread() : BaseClass("StarDesk", ui::kThreadUI)
{
}

MainThread::~MainThread()
{
}

void MainThread::OnInit()
{
#if !defined(_WIN32)
    // A peer that dies mid-stream (crash / network drop / power loss) makes
    // the next send() raise SIGPIPE, which by default KILLS the process - a
    // remote-control host must survive that. Ignore it so send() returns
    // EPIPE instead and the existing "closed connection" handling kicks in.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // test hook: STARDESK_MEM_LOG=<path> logs the RSS every 2s (leak hunting)
    if (const char* memLog = std::getenv("STARDESK_MEM_LOG")) {
        std::thread([memLog]() {
#if defined(__APPLE__)
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                mach_task_basic_info info;
                mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
                if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                              (task_info_t)&info, &count) == KERN_SUCCESS) {
                    FILE* f = std::fopen(memLog, "a");
                    if (f != nullptr) {
                        std::fprintf(f, "%lld %llu\n",
                                     (long long)time(nullptr),
                                     (unsigned long long)(info.resident_size / 1024));
                        std::fclose(f);
                    }
                }
            }
#else
            (void)memLog;
#endif
        }).detach();
    }

    // resource root next to the executable (may be absent - all theme data is
    // registered below, so the app works without any resource files)
    ui::FilePath resourcePath = ui::FilePathUtil::GetCurrentModuleDirectory();
    resourcePath += "resources/";
    ui::GlobalManager::Instance().Startup(ui::LocalFilesResParam(resourcePath));

    InitFontsAndColors();

    // developer hook: rasterise a font-fallback probe and exit
    if (const char* probePath = std::getenv("STARDESK_TEXT_PROBE")) {
        RunTextProbe(probePath);
        std::exit(0);
    }

    // load config, apply theme + language
    App::Instance().Init();

    // file-transfer channel: listen on main port + 1 (both sides; falls back
    // +1..+9 when taken). The session token is refreshed on each auth, so a
    // zero initial token is fine. The main window replaces the item callback.
    uint8_t zeroToken[32] = {0};
    App::Instance().FileTx().Start(
        (uint16_t)(App::Instance().Config().port + 1), zeroToken,
        [](const FileTransfer::Item&) {});

    if (App::Instance().Config().headless) {
        g_headlessHost = std::make_unique<HostService>();
        HostSession::Callbacks callbacks;
        callbacks.onStatus = [](const std::string& text, bool ok) {
            fprintf(stderr, "[headless-host] %s: %s\n", ok ? "ok" : "error",
                    ui::StringConvert::TToUTF8(text).c_str());
        };
        callbacks.onAsk = [](HostSession* session, const AskInfo&) {
            if (session != nullptr) {
                session->RespondAsk(true);
            }
        };
        callbacks.onClosed = []() {};
        if (!g_headlessHost->Start(App::Instance().Config().port, callbacks)) {
            fprintf(stderr, "[headless-host] failed to bind port %u\n",
                    (unsigned)App::Instance().Config().port);
            g_headlessHost.reset();
        }
        else {
            fprintf(stderr, "[headless-host] listening on port %u\n",
                    (unsigned)g_headlessHost->Port());
        }
        return;
    }

    MainWindow* window = new MainWindow();
    window->CreateWnd(nullptr, ui::WindowCreateParam("StarDesk", true));
    window->PostQuitMsgWhenClosed(true);
    window->ShowWindow(ui::kSW_SHOW_NORMAL);

}

void MainThread::OnCleanup()
{
    if (g_headlessHost) {
        g_headlessHost->Stop();
        g_headlessHost.reset();
    }
    App::Instance().Cleanup();
    ui::GlobalManager::Instance().Shutdown();
}

} // namespace sdk
