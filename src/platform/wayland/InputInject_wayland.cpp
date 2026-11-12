// Wayland input injection.
//
// A Wayland client cannot inject input via the protocol. There are two
// practical paths:
//
//  1. libei (EIS) through the xdg-desktop-portal RemoteDesktop interface —
//     the official, compositor-supported Wayland path (GNOME 45+/KDE/wlroots
//     with portals). The portal hands us an EIS socket fd, and a background
//     libei "sender" context injects absolute pointer / button / scroll /
//     keyboard events. The compositor shows a one-time approval dialog.
//  2. Linux uinput (/dev/uinput) — kernel-level virtual devices. Works on
//     most compositors, but GNOME/Mutter historically classifies EV_ABS
//     devices as tablets/touchscreens (pointer never moves) and only
//     delivers relative EV_REL mice; keyboard delivery is version-dependent.
//     Kept as a fallback when libei/portal is unavailable or denied.
//
// Both paths are implemented below; libei is preferred at runtime and the
// code falls back to uinput until the EIS session is approved and ready.

#include "../InputInject.h"
#include "../KeyMap.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#if defined(__linux__)
#include <linux/input.h>
#include <linux/uinput.h>
#elif defined(__FreeBSD__)
// FreeBSD: no uinput device; only the BTN_* button codes are needed by the
// libei (EIS) path below. FreeBSD ships them under dev/evdev.
#include <dev/evdev/input-event-codes.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef STARDESK_HAS_LIBEI
#include <dbus/dbus.h>
#include <libei.h>
#endif

namespace sdk {

namespace {

#ifdef STARDESK_HAS_LIBEI

// ---------------------------------------------------------------- EIS (libei)

// libei "sender" client connected to the compositor's EIS implementation via
// the xdg-desktop-portal RemoteDesktop v2 ConnectToEIS fd. A background
// thread drives ei_dispatch(); the static InputInjector API funnels events
// into the bound pointer/keyboard devices under a mutex (libei is not
// thread-safe).
class EisInjector {
public:
    enum class State { Idle, Starting, Ready, Failed };

    static EisInjector& Instance()
    {
        static EisInjector inst;
        return inst;
    }

    // Kick off the async portal+EIS setup (once). Returns immediately; the
    // setup thread blocks on the portal approval dialog until the user
    // responds. Never blocks the caller.
    void EnsureStarted()
    {
        const bool forced = IsForced();
        State expected = State::Idle;
        if (!m_state.compare_exchange_strong(expected, State::Starting)) {
            return;
        }
        if (forced && !WantEis()) {
            // STARDESK_EI=1 but libei unavailable at build time — unreachable
            // under #ifdef, kept for clarity
            m_state.store(State::Failed);
            return;
        }
        m_thread = std::thread([this] { Run(); });
    }

    bool Enabled() const
    {
        // honour STARDESK_EI=0 to force uinput
        const char* env = std::getenv("STARDESK_EI");
        return env == nullptr || std::strcmp(env, "0") != 0;
    }

    bool Ready() const { return m_state.load() == State::Ready; }

    bool Failed() const { return m_state.load() == State::Failed; }

    // Re-anchor the tracked cursor to the real pointer's origin. Call this
    // when a new client connects or the session (re)starts so the cached
    // position does not carry over a stale value from a previous session
    // (that stale value is what produced the intermittent mouse offset).
    // The tracked position is OUR belief of where the injected pointer is.
    // NEVER move the pointer here: pushing -100000 made the compositor clamp
    // the cursor into an unpredictable corner (observed bottom-right), so the
    // tracked (0,0) and the clamped real pointer disagreed and every later
    // relative motion carried a constant offset. Callers calibrate the
    // tracked position against the real cursor via ResetTo() instead.
    void Reanchor()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return;
        }
        m_cursorX = 0;
        m_cursorY = 0;
        fprintf(stderr, "[input] EIS cursor tracking reset (recalibration pending)\n");
    }

    // Set the tracked position to the real cursor position (no motion).
    void ResetTo(int x, int y)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cursorX = x;
        m_cursorY = y;
        fprintf(stderr, "[input] EIS tracking calibrated to (%d,%d)\n", x, y);
    }

    bool GetCursorPos(int& x, int& y)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        x = m_cursorX;
        y = m_cursorY;
        return true;
    }

    // ---- injection (called from the host session thread) ----
    // Returns false when the EIS path is not ready (caller falls back to
    // uinput). Absolute client coordinates (normalized 0..1) are converted
    // to relative deltas against an internally tracked position — the
    // relative pointer works without a device region (the absolute pointer
    // needs monitor viewports, which mutter did not provide on this host).
    bool MoveTo(double nx, double ny, int screenW, int screenH)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        if (screenW <= 0 || screenH <= 0) {
            return false;
        }
        // The tracked position is calibrated by the caller from the real
        // cursor position (ResetTo) - do not move the pointer here: a big
        // -100000 motion just clamps it into a corner (bottom-right observed)
        // and leaves a constant offset between the tracked and the real.
        m_cursorReset = true;
        const int x = (int)(nx * (double)screenW);
        const int y = (int)(ny * (double)screenH);
        const int dx = x - m_cursorX;
        const int dy = y - m_cursorY;
        m_cursorX = x;
        m_cursorY = y;
        if (dx != 0 || dy != 0) {
            ei_device_pointer_motion(m_pointer, dx, dy);
            ei_device_frame(m_pointer, ei_now(m_ei));
        }
        return true;
    }

    bool GetTrackedPos(int& x, int& y)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        x = m_cursorX;
        y = m_cursorY;
        return true;
    }

    bool MoveDelta(int dx, int dy)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        if (dx != 0 || dy != 0) {
            ei_device_pointer_motion(m_pointer, (double)dx, (double)dy);
            ei_device_frame(m_pointer, ei_now(m_ei));
            // keep the tracked position in lock-step with the injected cursor
            m_cursorX += dx;
            m_cursorY += dy;
        }
        else {
            // already at the target: an empty frame still queues behind any
            // in-flight motion, so the next (button) event fires after the
            // pointer settles there
            ei_device_frame(m_pointer, ei_now(m_ei));
        }
        return true;
    }

    bool Button(bool down, int button)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        uint32_t btn = BTN_LEFT;
        switch (button) {
        case 1: btn = BTN_RIGHT;  break;
        case 2: btn = BTN_MIDDLE; break;
        default: break;
        }
        ei_device_button_button(m_pointer, btn, down);
        ei_device_frame(m_pointer, ei_now(m_ei));
        return true;
    }

    bool Wheel(int deltaLines)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pointer == nullptr || !Ready()) {
            return false;
        }
        // one wheel click = 120 discrete units; positive y scrolls down
        // (matching the Wayland pointer axis convention)
        ei_device_scroll_discrete(m_pointer, 0, -deltaLines * 120);
        ei_device_frame(m_pointer, ei_now(m_ei));
        return true;
    }

    bool Key(uint16_t vkCode, bool down)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_keyboard == nullptr || !Ready()) {
            return false;
        }
        const int code = KeyMap::ToLinuxKeyCode(vkCode);
        if (code < 0) {
            return true; // unmapped key: not an error
        }
        ei_device_keyboard_key(m_keyboard, (uint32_t)code, down);
        ei_device_frame(m_keyboard, ei_now(m_ei));
        return true;
    }

private:
    static bool IsForced()
    {
        const char* env = std::getenv("STARDESK_EI");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }
    static bool WantEis() { return true; }

    // ------------------------------------------------------------ D-Bus utils

    // Append one {sv} dict entry (string value) to an open a{sv} dict iter.
    static void DictStr(DBusMessageIter* dict, const char* key, const char* value)
    {
        DBusMessageIter entry, var;
        dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
        dbus_message_iter_close_container(&entry, &var);
        dbus_message_iter_close_container(dict, &entry);
    }

    static void DictU32(DBusMessageIter* dict, const char* key, uint32_t value)
    {
        DBusMessageIter entry, var;
        dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &value);
        dbus_message_iter_close_container(&entry, &var);
        dbus_message_iter_close_container(dict, &entry);
    }

    // Walk an a{sv} dict (already recursed iterator) calling fn(key, valueIter)
    // where valueIter is the iterator INSIDE the variant.
    static void WalkDict(DBusMessageIter* dict,
                         const std::function<void(const std::string&, DBusMessageIter*)>& fn)
    {
        while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(dict, &entry);
            const char* key = nullptr;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&entry, &key);
                dbus_message_iter_next(&entry);
                if (key != nullptr && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var;
                    dbus_message_iter_recurse(&entry, &var);
                    fn(key, &var);
                }
            }
            dbus_message_iter_next(dict);
        }
    }

    // Send a portal method call that returns a Request object path, then wait
    // for org.freedesktop.portal.Request::Response and parse its result dict.
    // Returns true on status==0; the parsed keys are passed to onKey.
    static bool PortalRequestCall(
        DBusConnection* conn, const char* method,
        const std::function<void(DBusMessageIter*)>& appendArgs,
        const std::function<void(const std::string&, DBusMessageIter*)>& onKey)
    {
        DBusMessage* call = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop", method);
        DBusMessageIter args;
        dbus_message_iter_init_append(call, &args);
        appendArgs(&args);

        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 20000, &derr);
        dbus_message_unref(call);
        if (reply == nullptr) {
            fprintf(stderr, "[input] EIS portal %s call failed: %s\n", method,
                    derr.message ? derr.message : "unknown");
            dbus_error_free(&derr);
            return false;
        }

        // reply: o request_path
        std::string requestPath;
        DBusMessageIter it;
        dbus_message_iter_init(reply, &it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            const char* p = nullptr;
            dbus_message_iter_get_basic(&it, &p);
            if (p != nullptr) {
                requestPath = p;
            }
        }
        dbus_message_unref(reply);
        if (requestPath.empty()) {
            fprintf(stderr, "[input] EIS portal %s: no request handle\n", method);
            return false;
        }

        // wait for the Response signal on the request path
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            dbus_connection_read_write(conn, 200);
            DBusMessage* msg = dbus_connection_pop_message(conn);
            if (msg == nullptr) {
                continue;
            }
            const char* iface = dbus_message_get_interface(msg);
            const char* member = dbus_message_get_member(msg);
            const char* path = dbus_message_get_path(msg);
            if (iface != nullptr && std::strcmp(iface, "org.freedesktop.portal.Request") == 0 &&
                member != nullptr && std::strcmp(member, "Response") == 0 &&
                path != nullptr && requestPath == path) {
                uint32_t status = 1;
                DBusMessageIter rit;
                dbus_message_iter_init(msg, &rit);
                if (dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&rit, &status);
                    dbus_message_iter_next(&rit);
                }
                if (status != 0) {
                    fprintf(stderr, "[input] EIS portal %s failed (status %u)\n", method, status);
                } else if (dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter dict;
                    dbus_message_iter_recurse(&rit, &dict);
                    WalkDict(&dict, onKey);
                }
                dbus_message_unref(msg);
                return status == 0;
            }
            dbus_message_unref(msg);
        }
        fprintf(stderr, "[input] EIS portal %s timed out (path=%s)\n", method, requestPath.c_str());
        return false;
    }

    // ConnectToEIS returns the EIS fd synchronously.
    static int PortalConnectToEIS(DBusConnection* conn, const std::string& sessionHandle,
                                  const std::string& handleToken)
    {
        DBusMessage* call = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop", "ConnectToEIS");
        DBusMessageIter args;
        dbus_message_iter_init_append(call, &args);
        const char* sh = sessionHandle.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        DBusMessageIter dict;
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        DictStr(&dict, "handle_token", handleToken.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 20000, &derr);
        dbus_message_unref(call);
        if (reply == nullptr) {
            fprintf(stderr, "[input] EIS ConnectToEIS failed: %s\n",
                    derr.message ? derr.message : "unknown");
            dbus_error_free(&derr);
            return -1;
        }
        int fd = -1;
        DBusMessageIter it;
        dbus_message_iter_init(reply, &it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UNIX_FD) {
            dbus_message_iter_get_basic(&it, &fd);
        }
        dbus_message_unref(reply);
        return fd;
    }

    // ------------------------------------------------------------ setup flow

    void Run()
    {
        int eisFd = PortalSetup();
        if (eisFd < 0) {
            fprintf(stderr, "[input] EIS setup failed — falling back to uinput\n");
            m_state.store(State::Failed);
            return;
        }

        m_ei = ei_new_sender(this);
        if (m_ei == nullptr) {
            ::close(eisFd);
            ClosePortalConnection();
            m_state.store(State::Failed);
            return;
        }
        ei_configure_name(m_ei, "StarDesk");
        // ei_setup_backend_fd() takes ownership of the fd and initiates the
        // EIS conversation (there is no separate ei_connect in libei 1.5).
        if (ei_setup_backend_fd(m_ei, eisFd) < 0) {
            fprintf(stderr, "[input] EIS backend setup failed\n");
            ei_unref(m_ei);
            m_ei = nullptr;
            ClosePortalConnection();
            m_state.store(State::Failed);
            return;
        }
        fprintf(stderr, "[input] EIS connecting (fd=%d)\n", eisFd);

        // dispatch loop (runs until disconnect/failure)
        while (m_state.load() != State::Failed) {
            ei_dispatch(m_ei);
            struct ei_event* ev;
            bool stop = false;
            while ((ev = ei_get_event(m_ei)) != nullptr) {
                HandleEvent(ev);
                ei_event_unref(ev);
                if (m_state.load() == State::Failed) {
                    stop = true;
                    break;
                }
            }
            if (stop) {
                break;
            }
            if (ei_get_fd(m_ei) >= 0) {
                // brief sleep to avoid a busy loop when no events are queued
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        fprintf(stderr, "[input] EIS dispatch loop exited\n");
        if (m_ei != nullptr) {
            ei_unref(m_ei);
            m_ei = nullptr;
        }
        ClosePortalConnection();
        m_state.store(State::Failed);
    }

    // Release the portal session: closing the D-Bus connection tells the
    // portal to close the RemoteDesktop session (and mutter tears down the
    // EIS server with it).
    void ClosePortalConnection()
    {
        if (m_conn != nullptr) {
            dbus_connection_close(m_conn);
            dbus_connection_unref(m_conn);
            m_conn = nullptr;
        }
    }

    void HandleEvent(struct ei_event* ev)
    {
        switch (ei_event_get_type(ev)) {
        case EI_EVENT_CONNECT:
            fprintf(stderr, "[input] EIS connected\n");
            break;
        case EI_EVENT_DISCONNECT:
            fprintf(stderr, "[input] EIS disconnected\n");
            // forget the tracked cursor so a re-established session re-anchors
            // from the real pointer position instead of a stale one (this is
            // what caused the intermittent mouse offset after reconnects)
            m_cursorReset = false;
            m_cursorX = 0;
            m_cursorY = 0;
            m_pointer = nullptr;
            m_keyboard = nullptr;
            m_state.store(State::Failed);
            break;
        case EI_EVENT_SEAT_ADDED: {
            struct ei_seat* seat = ei_event_get_seat(ev);
            if (seat != nullptr) {
                fprintf(stderr, "[input] EIS seat '%s' — binding capabilities\n",
                        ei_seat_get_name(seat));
                ei_seat_bind_capabilities(seat,
                                          EI_DEVICE_CAP_POINTER,      // relative
                                          EI_DEVICE_CAP_BUTTON,
                                          EI_DEVICE_CAP_SCROLL,
                                          EI_DEVICE_CAP_KEYBOARD,
                                          NULL);
            }
            break;
        }
        case EI_EVENT_DEVICE_ADDED: {
            struct ei_device* dev = ei_event_get_device(ev);
            if (dev == nullptr) {
                break;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER) &&
                m_pointer == nullptr) {
                m_pointer = ei_device_ref(dev);
                fprintf(stderr, "[input] EIS relative pointer device (%s)\n",
                        ei_device_get_name(dev));
            }
            if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD) &&
                m_keyboard == nullptr) {
                m_keyboard = ei_device_ref(dev);
                fprintf(stderr, "[input] EIS keyboard device (%s)\n", ei_device_get_name(dev));
            }
            break;
        }
        case EI_EVENT_DEVICE_RESUMED: {
            struct ei_device* dev = ei_event_get_device(ev);
            if (dev != nullptr) {
                const uint32_t seq = ei_event_emulating_get_sequence(ev);
                ei_device_start_emulating(dev, seq);
                std::lock_guard<std::mutex> lock(m_mutex);
                if ((dev == m_pointer || dev == m_keyboard) &&
                    m_state.load() != State::Ready) {
                    m_state.store(State::Ready);
                    fprintf(stderr, "[input] EIS ready for injection\n");
                }
                // Relative motion is tracked from a locally cached position,
                // so the cached value must be re-calibrated against the real
                // pointer whenever the device resumes (e.g. after a pause or
                // a reconnect) - the caller pushes the real position with
                // ResetTo(). Never move the pointer here: a big motion clamps
                // the cursor into a corner and leaves a constant offset.
                if (dev == m_pointer) {
                    m_cursorReset = false;
                    m_cursorX = 0;
                    m_cursorY = 0;
                    fprintf(stderr, "[input] EIS cursor tracking reset (resume)\n");
                }
            }
            break;
        }
        case EI_EVENT_DEVICE_PAUSED: {
            // the compositor paused us (e.g. another client took over);
            // wait for a subsequent RESUMED before injecting again
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state.store(State::Starting);
            break;
        }
        default:
            break;
        }
    }

    // ---- restore_token persistence (RemoteDesktop portal persist_mode) ----
    // The Start() response carries a single-use restore_token when the user
    // granted persistence; passing it back in the next SelectDevices skips
    // the KDE approval dialog until explicitly revoked. Stored on disk so it
    // survives service restarts. A stale/consumed token is ignored by the
    // portal (falls back to prompt normally).
    static std::string RestoreTokenPath()
    {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.stardesk_eis_token";
    }

    static std::string LoadRestoreToken()
    {
        FILE* f = std::fopen(RestoreTokenPath().c_str(), "r");
        if (f == nullptr) {
            return std::string();
        }
        char buf[512] = {0};
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::string tok(buf, strnlen(buf, n));
        while (!tok.empty() && (tok.back() == '\n' || tok.back() == ' ')) {
            tok.pop_back();
        }
        return tok;
    }

    static void SaveRestoreToken(const std::string& tok)
    {
        if (tok.empty()) {
            return;
        }
        FILE* f = std::fopen(RestoreTokenPath().c_str(), "w");
        if (f != nullptr) {
            std::fprintf(f, "%s\n", tok.c_str());
            std::fclose(f);
        }
        fprintf(stderr, "[input] EIS restore token saved (%s)\n", RestoreTokenPath().c_str());
    }

    // Create the portal RemoteDesktop session and return the EIS fd.
    // Blocks on the portal approval dialog (background thread).
    int PortalSetup()
    {
        DBusError err;
        dbus_error_init(&err);
        DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (conn == nullptr) {
            fprintf(stderr, "[input] EIS dbus session failed: %s\n",
                    err.message ? err.message : "unknown");
            dbus_error_free(&err);
            return -1;
        }

        // ---- CreateSession
        std::string sessionHandle;
        bool ok = PortalRequestCall(
            conn, "CreateSession",
            [](DBusMessageIter* args) {
                DBusMessageIter dict;
                dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
                DictStr(&dict, "handle_token", "sd_eis_1");
                DictStr(&dict, "session_handle_token", "sd_eis_sess");
                dbus_message_iter_close_container(args, &dict);
            },
            [&sessionHandle](const std::string& key, DBusMessageIter* v) {
                if (key == "session_handle" &&
                    dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING) {
                    const char* s = nullptr;
                    dbus_message_iter_get_basic(v, &s);
                    if (s != nullptr) {
                        sessionHandle = s;
                    }
                }
            });
        if (!ok || sessionHandle.empty()) {
            fprintf(stderr, "[input] EIS CreateSession failed\n");
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return -1;
        }
        fprintf(stderr, "[input] EIS session %s\n", sessionHandle.c_str());

        // ---- SelectDevices: keyboard(1) | pointer(2).
        // persist_mode=2 (until explicitly revoked) with the restore_token
        // from the previous Start(): the portal restores the KDE approval
        // instead of re-prompting on every host start (the dialog used to
        // block the unattended session here for 60s per start). A stale
        // token is ignored by the portal and the dialog shows normally.
        const std::string restoreToken = LoadRestoreToken();
        ok = PortalRequestCall(
            conn, "SelectDevices",
            [&sessionHandle, &restoreToken](DBusMessageIter* args) {
                const char* sh = sessionHandle.c_str();
                dbus_message_iter_append_basic(args, DBUS_TYPE_OBJECT_PATH, &sh);
                DBusMessageIter dict;
                dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
                DictStr(&dict, "handle_token", "sd_eis_2");
                DictU32(&dict, "types", 1u | 2u); // KEYBOARD | POINTER
                DictU32(&dict, "persist_mode", 2u); // 2 = until explicitly revoked
                if (!restoreToken.empty()) {
                    DictStr(&dict, "restore_token", restoreToken.c_str());
                }
                dbus_message_iter_close_container(args, &dict);
            },
            [](const std::string&, DBusMessageIter*) {});
        if (!ok) {
            fprintf(stderr, "[input] EIS SelectDevices failed\n");
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return -1;
        }

        // ---- Start (shows the approval dialog once)
        ok = PortalRequestCall(
            conn, "Start",
            [&sessionHandle](DBusMessageIter* args) {
                const char* sh = sessionHandle.c_str();
                dbus_message_iter_append_basic(args, DBUS_TYPE_OBJECT_PATH, &sh);
                const char* parent = "";
                dbus_message_iter_append_basic(args, DBUS_TYPE_STRING, &parent);
                DBusMessageIter dict;
                dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict);
                DictStr(&dict, "handle_token", "sd_eis_3");
                dbus_message_iter_close_container(args, &dict);
            },
            [](const std::string& key, DBusMessageIter* v) {
                // Start() returns a single-use restore_token when the user
                // granted persistence (persist_mode); save it for the next
                // host start so the approval is skipped then.
                if (key == "restore_token" &&
                    dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING) {
                    const char* s = nullptr;
                    dbus_message_iter_get_basic(v, &s);
                    if (s != nullptr && s[0] != '\0') {
                        SaveRestoreToken(s);
                    }
                }
            });
        if (!ok) {
            fprintf(stderr, "[input] EIS Start failed/denied\n");
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return -1;
        }

        // ---- ConnectToEIS
        int fd = PortalConnectToEIS(conn, sessionHandle, "sd_eis_4");
        if (fd < 0) {
            fprintf(stderr, "[input] EIS ConnectToEIS failed\n");
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return -1;
        }
        // IMPORTANT: keep the D-Bus connection alive for the whole EIS
        // session. The RemoteDesktop session is owned by the D-Bus sender;
        // closing this connection makes the portal close the session, which
        // tears down mutter's EIS server and disconnects our client right in
        // the middle of the handshake "Disconnected by EIS".
        m_conn = conn;
        return fd;
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::atomic<State> m_state{State::Idle};

    struct ei* m_ei = nullptr;
    struct ei_device* m_pointer = nullptr;
    struct ei_device* m_keyboard = nullptr;
    int m_cursorX = 0;
    int m_cursorY = 0;
    bool m_cursorReset = false;
    DBusConnection* m_conn = nullptr;
};

EisInjector& EIS()
{
    return EisInjector::Instance();
}

#endif // STARDESK_HAS_LIBEI

#if defined(__linux__)
// ---------------------------------------------------------------- uinput

// The kernel's uinput key-code range landed at KEY_MAX=0x2ff in Linux 6.x
// (torvalds: "the uinput binary interface is stable"). Guard early so a
// kernel bump on the build machine does not silently truncate the bit array.
#ifndef KEY_MAX
#define KEY_MAX 0x2ff
#endif

// Low-level wrapper over /dev/uinput: opens the device, registers event
// bits, creates the kernel device. Subclasses declare the bits they need.
class UInputDevice {
public:
    bool Open()
    {
        m_fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (m_fd < 0) {
            m_fd = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        }
        if (m_fd < 0) {
            fprintf(stderr, "[input] /dev/uinput open failed: %s\n", strerror(errno));
            return false;
        }

        // Enable event types — subclasses set their specific bits.
        if (::ioctl(m_fd, UI_SET_EVBIT, EV_KEY) < 0) { goto fail; }
        if (::ioctl(m_fd, UI_SET_EVBIT, EV_SYN) < 0) { goto fail; }

        if (!SetupBits()) { goto fail; }

        // ---- device identity ----
        {
            struct uinput_setup usetup;
            std::memset(&usetup, 0, sizeof(usetup));
            // UINPUT_MAX_NAME_SIZE is 80
            std::snprintf(usetup.name, sizeof(usetup.name), "%s", DeviceName());
            usetup.id.bustype = BUS_VIRTUAL;
            usetup.id.vendor  = 0x1209; // generic "remote input" vendor id
            usetup.id.product = ProductId();
            usetup.id.version = 1;
            if (::ioctl(m_fd, UI_DEV_SETUP, &usetup) < 0) { goto fail; }
        }

        if (::ioctl(m_fd, UI_DEV_CREATE, 0) < 0) { goto fail; }

        m_open = true;
        fprintf(stderr, "[input] uinput device created: %s (fd=%d)\n", DeviceName(), m_fd);
        return true;

    fail:
        fprintf(stderr, "[input] uinput setup failed (%s): %s\n", DeviceName(), strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    void Close()
    {
        if (m_fd >= 0) {
            ::ioctl(m_fd, UI_DEV_DESTROY, 0);
            ::close(m_fd);
            m_fd = -1;
        }
        m_open = false;
    }

    bool IsOpen() const { return m_open; }

    void Emit(int type, int code, int value)
    {
        if (m_fd < 0) {
            return;
        }
        struct input_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ::gettimeofday(&ev.time, nullptr);
        ev.type  = (unsigned short)type;
        ev.code  = (unsigned short)code;
        ev.value = value;
        ssize_t n = ::write(m_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
            static int s_drops = 0;
            if (++s_drops <= 5) {
                fprintf(stderr, "[input] write failed (%s): %s\n", DeviceName(), strerror(errno));
            }
        }
    }

    void Sync() { Emit(EV_SYN, SYN_REPORT, 0); }

protected:
    // Register the device-specific bits (called before UI_DEV_SETUP).
    virtual bool SetupBits() = 0;
    virtual const char* DeviceName() const = 0;
    virtual unsigned short ProductId() const = 0;

    bool SetEvBit(int bit)
    {
        if (::ioctl(m_fd, UI_SET_EVBIT, bit) < 0) {
            fprintf(stderr, "[input] UI_SET_EVBIT %d failed: %s\n", bit, strerror(errno));
            return false;
        }
        return true;
    }
    bool SetKeyBit(int bit)
    {
        if (::ioctl(m_fd, UI_SET_KEYBIT, bit) < 0) {
            fprintf(stderr, "[input] UI_SET_KEYBIT 0x%x failed: %s\n", bit, strerror(errno));
            return false;
        }
        return true;
    }
    bool SetRelBit(int bit)
    {
        if (::ioctl(m_fd, UI_SET_RELBIT, bit) < 0) {
            fprintf(stderr, "[input] UI_SET_RELBIT %d failed: %s\n", bit, strerror(errno));
            return false;
        }
        return true;
    }

    int  m_fd = -1;
    bool m_open = false;
};

// A plain relative mouse: EV_REL motion + buttons, NO EV_ABS and NO
// INPUT_PROP_DIRECT so libinput classifies it as a pointer (not a
// touchscreen/tablet). This is the uinput class GNOME Wayland delivers.
class UInputPointer : public UInputDevice {
protected:
    bool SetupBits() override
    {
        // UI_SET_RELBIT only fills the relbit mask — the kernel does NOT
        // set the EV_REL event bit for it (unlike input_set_capability),
        // so EV_REL must be enabled explicitly or the device exposes no
        // relative axes and motion events are silently dropped.
        if (!SetEvBit(EV_REL)) return false;
        if (!SetRelBit(REL_X)) return false;
        if (!SetRelBit(REL_Y)) return false;
        if (!SetRelBit(REL_WHEEL)) return false;
        if (!SetRelBit(REL_HWHEEL)) return false;
        if (!SetKeyBit(BTN_LEFT)) return false;
        if (!SetKeyBit(BTN_RIGHT)) return false;
        if (!SetKeyBit(BTN_MIDDLE)) return false;
        if (!SetKeyBit(BTN_SIDE)) return false;
        if (!SetKeyBit(BTN_EXTRA)) return false;
        return true;
    }
    const char* DeviceName() const override { return "StarDesk Remote Pointer"; }
    unsigned short ProductId() const override { return 0x0001; }
};

// A plain keyboard: EV_KEY for the full key range. Nothing else — adding
// ABS axes to the same device made Mutter ignore the keyboard half.
class UInputKeyboard : public UInputDevice {
protected:
    bool SetupBits() override
    {
        SetEvBit(EV_REP); // key repeat capability (harmless)
        for (int k = 0; k <= KEY_MAX; ++k) {
            if (!SetKeyBit(k)) return false;
        }
        return true;
    }
    const char* DeviceName() const override { return "StarDesk Remote Keyboard"; }
    unsigned short ProductId() const override { return 0x0002; }
};

UInputPointer& PointerDevice()
{
    static UInputPointer s_dev;
    return s_dev;
}

UInputKeyboard& KeyboardDevice()
{
    static UInputKeyboard s_dev;
    return s_dev;
}

// Tracked cursor position in physical pixels. uinput relative motion is
// applied against this; Wayland clients cannot query the global cursor
// position, so we track it internally (every MoveTo updates it). When the
// session starts without a MoveTo the position is (0,0) — the first move
// therefore mirrors the client's cursor position onto the remote desktop.
std::atomic<int> s_cursorX{0};
std::atomic<int> s_cursorY{0};

} // namespace

// ---------------------------------------------------------------- public API

bool InputInjector::MoveTo(double nx, double ny, int screenW, int screenH)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().MoveTo(nx, ny, screenW, screenH)) {
            return true;
        }
        // Do not switch from a still-starting EIS device to uinput: the two
        // relative-pointer paths have independent cursor tracking, so that
        // handoff creates a startup offset. Fall back only after EIS failed.
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    UInputDevice& dev = PointerDevice();
    if (!dev.IsOpen()) {
        if (!dev.Open()) {
            return false;
        }
        // The tracked position starts at (0,0); the caller calibrates it to
        // the real cursor (ResetCursorTo) before the first motion - do NOT
        // push the pointer to a corner here (it gets clamped somewhere
        // unpredictable and every relative motion then carries the offset).
        s_cursorX.store(0);
        s_cursorY.store(0);
        fprintf(stderr, "[input] uinput pointer opened (calibration pending)\n");
    }
    if (screenW <= 0 || screenH <= 0) {
        return false;
    }
    const int x = (int)(nx * (double)screenW);
    const int y = (int)(ny * (double)screenH);
    const int dx = x - s_cursorX.load();
    const int dy = y - s_cursorY.load();
    s_cursorX.store(x);
    s_cursorY.store(y);
    if (dx != 0) {
        dev.Emit(EV_REL, REL_X, dx);
    }
    if (dy != 0) {
        dev.Emit(EV_REL, REL_Y, dy);
    }
    dev.Sync();
    return true;
}

bool InputInjector::Button(bool down, int button)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().Button(down, button)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    UInputDevice& dev = PointerDevice();
    if (!dev.IsOpen()) {
        if (!dev.Open()) {
            return false;
        }
    }
    int btn = BTN_LEFT;
    switch (button) {
    case 1: btn = BTN_RIGHT;  break;
    case 2: btn = BTN_MIDDLE; break;
    default: break;
    }
    dev.Emit(EV_KEY, btn, down ? 1 : 0);
    dev.Sync();
    return true;
}

bool InputInjector::MoveDelta(int dx, int dy)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().MoveDelta(dx, dy)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    return false; // no delta API on the other paths
}

bool InputInjector::GetTrackedPos(int& x, int& y)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().GetTrackedPos(x, y)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    // uinput path keeps its own tracker (s_cursorX/Y); expose it so the
    // host can verify/clip the relative-motion base
    x = s_cursorX.load();
    y = s_cursorY.load();
    return true;
}

bool InputInjector::Wheel(int deltaLines)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().Wheel(deltaLines)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    UInputDevice& dev = PointerDevice();
    if (!dev.IsOpen()) {
        if (!dev.Open()) {
            return false;
        }
    }
    dev.Emit(EV_REL, REL_WHEEL, deltaLines);
    dev.Sync();
    return true;
}

bool InputInjector::Key(uint16_t vkCode, bool down)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().Key(vkCode, down)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    UInputDevice& dev = KeyboardDevice();
    if (!dev.IsOpen()) {
        if (!dev.Open()) {
            return false;
        }
    }
    const int code = KeyMap::ToLinuxKeyCode(vkCode);
    if (code < 0) {
        // key not mapped: not an error (the remote keyboard layout may have
        // keys we do not care about controlling, e.g. media keys)
        return true;
    }
    dev.Emit(EV_KEY, code, down ? 1 : 0);
    dev.Sync();
    return true;
}

bool InputInjector::GetCursorPos(int& x, int& y)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready() && EIS().GetCursorPos(x, y)) {
            return true;
        }
        if (!EIS().Failed()) {
            return false;
        }
    }
#endif
    x = s_cursorX.load();
    y = s_cursorY.load();
    return true;
}

void InputInjector::ResetCursor()
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        EIS().Reanchor();
    }
#endif
    // uinput: tracking-only reset, no motion (see ResetCursorTo)
    s_cursorX.store(0);
    s_cursorY.store(0);
    fprintf(stderr, "[input] cursor tracking reset (recalibration pending)\n");
}

void InputInjector::ResetCursorTo(int x, int y)
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready()) {
            EIS().ResetTo(x, y);
            return;
        }
    }
#endif
    // set the tracked position regardless of the device state - the device
    // opens lazily on the first motion, but the tracked values must already
    // match the real cursor so that first delta is exact
    s_cursorX.store(x);
    s_cursorY.store(y);
    fprintf(stderr, "[input] uinput tracking calibrated to (%d,%d)\n", x, y);
}

bool InputInjector::PermissionGranted()
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled()) {
        EIS().EnsureStarted();
        if (EIS().Ready()) {
            return true;
        }
    }
#endif
    // uinput is available (or was already opened successfully)
    if (PointerDevice().IsOpen()) {
        return true;
    }
    // probe: try to open and immediately close (lightweight test)
    int probe = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (probe < 0) {
        probe = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    }
    if (probe >= 0) {
        ::close(probe);
        return true;
    }
    return false;
}

std::string InputInjector::PermissionHint()
{
#ifdef STARDESK_HAS_LIBEI
    if (EIS().Enabled() && EIS().Failed()) {
        return "Wayland input portal unavailable/denied — using uinput";
    }
#endif
    if (PointerDevice().IsOpen()) {
        return std::string();
    }
    // test write access
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    }
    if (fd >= 0) {
        ::close(fd);
        return std::string(); // accessible
    }
    if (errno == EACCES) {
        return "uinput permission denied (run: sudo chmod 666 /dev/uinput)";
    }
    if (errno == ENOENT) {
        return "uinput unavailable (run: sudo modprobe uinput)";
    }
    return "uinput unavailable: " + std::string(strerror(errno));
}

#elif !defined(__linux__)
// FreeBSD: no uinput device; input injection is libei(EIS) only.
// The anonymous namespace opened above (line ~50) is closed by the Linux
// uinput section; close it here too so the stubs land in sdk directly.
} // namespace
// These stubs keep the InputInjector vtable complete when libei is unavailable
// or the session is not yet approved.
bool InputInjector::MoveTo(double, double, int, int) { return false; }
bool InputInjector::Button(bool, int) { return false; }
bool InputInjector::Wheel(int) { return false; }
bool InputInjector::MoveDelta(int, int) { return false; } // no libei: uinput is absolute
bool InputInjector::GetTrackedPos(int&, int&) { return false; } // no libei: uinput is absolute
bool InputInjector::Key(uint16_t, bool) { return false; }
bool InputInjector::GetCursorPos(int& x, int& y) { x = 0; y = 0; return true; }
void InputInjector::ResetCursor() {}
void InputInjector::ResetCursorTo(int, int) {}
bool InputInjector::PermissionGranted() { return false; }
std::string InputInjector::PermissionHint() { return "uinput not available on FreeBSD"; }
#endif // __linux__

// On Wayland clients cannot query other windows' positions. The feedback-loop
// guard (IsOwnWindowAt / IsOwnAppFocused) is not available through the Wayland
// protocol. The guard only matters when connecting to localhost (self-connect):
// clicks aimed at the host's own StarDesk window would loop back infinitely.
// For remote connections (the typical use case) the guard is unnecessary.
//
// TODO: detect StarDesk windows that belong to this process through the dui
// WindowManager; the host process owns the dui windows and knows their rects.

bool InputInjector::IsOwnWindowAt(double, double) { return false; }
bool InputInjector::IsOwnAppFocused() { return false; }

} // namespace sdk
