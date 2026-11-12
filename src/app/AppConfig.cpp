#include "AppConfig.h"

#include "Language.h"
#include "Theme.h"

#include "dui/Utils/FileUtil.h"
#include "dui/Utils/StringCharset.h"
#include "dui/Utils/StringConvert.h"
#include "dui/Utils/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>

#ifdef DUI_BUILD_FOR_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sdk {

// ---------------------------------------------------------------- helpers

namespace {

std::string GetHomeDir()
{
#ifdef DUI_BUILD_FOR_WIN
    wchar_t buf[MAX_PATH] = {0};
    const size_t n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0) {
        return std::string();
    }
    return ui::StringConvert::WStringToT(std::wstring(buf));
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return ui::StringConvert::UTF8ToT(home);
    }
    return std::string();
#endif
}

std::string ReadFileUtf8(const ui::FilePath& path)
{
    std::vector<uint8_t> data;
    if (!ui::FileUtil::ReadFileData(path, data) || data.empty()) {
        return std::string();
    }
    std::wstring text;
    // UNKNOWN: auto-detect. Pure-ASCII data is detected as ANSI and decoded as
    // UTF-8 on non-Windows (dui's ANSI branch); UTF-8 text with or without BOM
    // works too. Passing UTF8 explicitly would REJECT pure-ASCII content.
    ui::StringCharset::GetDataAsString((const char*)data.data(), (uint32_t)data.size(),
                                       ui::CharsetType::UNKNOWN, text);
    return ui::StringConvert::WStringToT(text);
}

// Parse an integer from a std::string (UTF-8 on non-UNICODE builds, wstring on
// UNICODE Windows builds); avoid platform-specific helpers.
int ToInt(const std::string& s, int def)
{
    if (s.empty()) {
        return def;
    }
    const std::string utf8 = ui::StringConvert::TToUTF8(s);
    char* end = nullptr;
    long v = std::strtol(utf8.c_str(), &end, 10);
    if (end == utf8.c_str()) {
        return def;
    }
    return (int)v;
}

} // namespace

// ---------------------------------------------------------------- AppConfig

ui::FilePath AppConfig::GetFilePath()
{
    // Test hook: STARDESK_CONF points at an alternate config file so two
    // instances can run on one machine without overwriting each other
    // (e.g. `STARDESK_CONF=~/.stardesk-client.conf stardesk`).
    if (const char* p = std::getenv("STARDESK_CONF")) {
        return ui::FilePath(ui::StringConvert::UTF8ToT(p));
    }
    ui::FilePath p(GetHomeDir());
    if (p.IsExistsDirectory()) {
        p.JoinFilePath(ui::FilePath(".stardesk.conf"));
    }
    return p;
}

ui::FilePath AppConfig::GetDefaultReceiveDir()
{
    ui::FilePath p(GetHomeDir());
    if (p.IsExistsDirectory()) {
        p.JoinFilePath(ui::FilePath("Downloads"));
        p.JoinFilePath(ui::FilePath("StarDesk"));
    }
    return p;
}

void AppConfig::Load()
{
    const ui::FilePath path = GetFilePath();
    const std::string content = ReadFileUtf8(path);
    if (!content.empty()) {
        // key=value lines; '#' starts a comment
        std::map<std::string, std::string> kv;
        const std::list<std::string> lines = ui::StringUtil::Split(content, "\n");
        for (std::string l : lines) {
            if (!l.empty() && l.back() == ('\r')) {
                l.pop_back();
            }
            if (l.empty() || l[0] == ('#')) {
                continue;
            }
            const size_t eq = l.find(('='));
            if (eq == std::string::npos) {
                continue;
            }
            kv[l.substr(0, eq)] = l.substr(eq + 1);
        }

        // test hooks: a second file "auto.conf" next to the executable may
        // overlay the auto_* keys (usable when ~/.stardesk.conf is not
        // writable by the test harness, e.g. macOS TCC)
        ui::FilePath autoPath = ui::FilePathUtil::GetCurrentModuleDirectory();
        autoPath.JoinFilePath(ui::FilePath("auto.conf"));
        const std::string autoContent = ReadFileUtf8(autoPath);
        if (!autoContent.empty()) {
            const std::list<std::string> autoLines =
                ui::StringUtil::Split(autoContent, "\n");
            for (std::string l : autoLines) {
                if (!l.empty() && l.back() == ('\r')) {
                    l.pop_back();
                }
                if (l.empty() || l[0] == ('#')) {
                    continue;
                }
                const size_t eq = l.find(('='));
                if (eq == std::string::npos) {
                    continue;
                }
                kv[l.substr(0, eq)] = l.substr(eq + 1);
            }
        }

        auto getStr = [&](const char* key) {
            auto it = kv.find(ui::StringConvert::UTF8ToT(key));
            return it != kv.end() ? it->second : std::string();
        };
        auto getInt = [&](const char* key, int def) {
            auto it = kv.find(ui::StringConvert::UTF8ToT(key));
            return it != kv.end() ? ToInt(it->second, def) : def;
        };

        password = getStr("password");
        port = (uint16_t)getInt("port", 7456);
        manualAccept = getInt("manual_accept", 0) != 0;
        extendScreen = getInt("extend_screen", 0) != 0;
        receiveDir = getStr("receive_dir");
        darkTheme = getInt("dark_theme", 0) != 0;
        language = getInt("language", 0);
        wantFps = getInt("want_fps", 30);
        wantRes = getInt("want_res", 0);
        viewOnly = getInt("view_only", 0) != 0;
        cursorMode = getInt("cursor_mode", -1);
        if (cursorMode < 0) {
            const bool local = getInt("show_local_cursor", 1) != 0;
            const bool remote = getInt("show_remote_cursor", 0) != 0;
            cursorMode = remote ? (local ? 1 : 2) : 0;
        }
        if (cursorMode < 0 || cursorMode > 2) {
            cursorMode = 0;
        }
        deviceName = getStr("device_name");
        headless = getInt("headless", 0) != 0;
        autoHost = getStr("auto_host");
        autoPort = getInt("auto_port", 0);
        autoPassword = getStr("auto_password");
        autoRes = getInt("auto_res", -1);
        autoFps = getInt("auto_fps", -1);
        autoCodec = getInt("auto_codec", -1);
        autoOpenBrowser = getInt("auto_open_browser", 0) != 0;
    }

    if (password.empty()) {
        password = GeneratePassword();
    }
    if (deviceName.empty()) {
        deviceName = App::GetHostName();
    }
    if (language < 0 || language >= Language::Count()) {
        language = 0;
    }

    // Test hook: STARDESK_PORT overrides the listen port so two instances can
    // run on one machine (e.g. `STARDESK_PORT=7458 stardesk` as a client).
    if (const char* sp = std::getenv("STARDESK_PORT")) {
        const int p = std::atoi(sp);
        if (p >= 1 && p <= 65535) {
            port = (uint16_t)p;
        }
    }
    if (const char* hp = std::getenv("STARDESK_HEADLESS")) {
        headless = std::atoi(hp) != 0;
    }
}

void AppConfig::Save() const
{
    const ui::FilePath path = GetFilePath();
    std::vector<uint8_t> data;
    auto add = [&](const std::string& s) {
        const std::string utf8 = ui::StringConvert::TToUTF8(s);
        data.insert(data.end(), utf8.begin(), utf8.end());
    };
    add("# StarDesk configuration (UTF-8)\n");
    add("password="); add(password); add("\n");
    add(ui::StringUtil::Printf("port=%d\n", (int)port));
    add(ui::StringUtil::Printf("manual_accept=%d\n", manualAccept ? 1 : 0));
    add(ui::StringUtil::Printf("extend_screen=%d\n", extendScreen ? 1 : 0));
    add("receive_dir="); add(receiveDir); add("\n");
    add(ui::StringUtil::Printf("dark_theme=%d\n", darkTheme ? 1 : 0));
    add(ui::StringUtil::Printf("language=%d\n", language));
    add(ui::StringUtil::Printf("want_fps=%d\n", wantFps));
    add(ui::StringUtil::Printf("want_res=%d\n", wantRes));
    add(ui::StringUtil::Printf("view_only=%d\n", viewOnly ? 1 : 0));
    add(ui::StringUtil::Printf("cursor_mode=%d\n", cursorMode));
    add("device_name="); add(deviceName); add("\n");
    add(ui::StringUtil::Printf("headless=%d\n", headless ? 1 : 0));
    ui::FileUtil::WriteFileData(path, data);
}

std::string AppConfig::GeneratePassword()
{
    // 8 decimal digits (unambiguous characters, works in any locale)
    const uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t x = seed ^ 0x9E3779B97F4A7C15ull; // xorshift-ish
    char buf[16];
    for (int i = 0; i < 8; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        buf[i] = (char)('0' + (x % 10));
    }
    buf[8] = 0;
    return ui::StringConvert::UTF8ToT(buf);
}

// ---------------------------------------------------------------- App

App& App::Instance()
{
    static App s_app;
    return s_app;
}

std::string App::GetHostName()
{
#ifdef DUI_BUILD_FOR_WIN
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &size) != 0) {
        return ui::StringConvert::WStringToT(std::wstring(buf));
    }
    return "StarDesk";
#else
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) != 0) {
        return "StarDesk";
    }
    return ui::StringConvert::UTF8ToT(buf);
#endif
}

void App::AddListener(IAppListener* l)
{
    if (std::find(m_listeners.begin(), m_listeners.end(), l) == m_listeners.end()) {
        m_listeners.push_back(l);
    }
}

void App::RemoveListener(IAppListener* l)
{
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), l),
                      m_listeners.end());
}

void App::SetTheme(bool dark)
{
    if (m_config.darkTheme == dark) {
        return;
    }
    m_config.darkTheme = dark;
    Theme::Set(dark);
    for (IAppListener* l : m_listeners) {
        if (l != nullptr) {
            l->OnThemeChanged();
        }
    }
}

void App::SetLanguage(Lang lang)
{
    if ((int)m_config.language == (int)lang) {
        return;
    }
    m_config.language = (int)lang;
    Language::Set(lang);
    fprintf(stderr, "[app] language -> %s\n", Language::NativeName(lang).c_str());
    fflush(stderr);
    for (IAppListener* l : m_listeners) {
        if (l != nullptr) {
            l->OnLanguageChanged();
        }
    }
}

void App::Init()
{
    if (m_started) {
        return;
    }
    m_config.Load();
    Theme::Set(m_config.darkTheme);
    Language::Set((Lang)m_config.language);
    // file-transfer listener on the file port (token is set after auth)
    uint8_t zero[32] = {0};
    m_fileTx.Start(m_config.port + 1, zero, nullptr);
    m_started = true;
}

void App::Cleanup()
{
    if (!m_started) {
        return;
    }
    m_fileTx.Stop();
    m_disc.Stop();
    m_config.Save();
    m_started = false;
}

} // namespace sdk
