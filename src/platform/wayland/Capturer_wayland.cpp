// Wayland screen capture via xdg-desktop-portal ScreenCast + PipeWire.
//
// A Wayland client cannot grab the screen directly; the standard path is the
// portal ScreenCast API: DBus (CreateSession / SelectSources / Start) yields
// a PipeWire node id, and the compositor streams desktop frames into that
// node. Buffers arrive as DMA-BUF (imported via EGL and read back with
// glReadPixels) or as plain memfd (direct memcpy).
//
// DBus is used directly (libdbus-1); PipeWire via libpipewire-0.3; EGL for
// the DMA-BUF readback (the dui Wayland backend already links EGL/GLES).

#include <dbus/dbus.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/format-utils.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// EGL image functions are extension entry points: some libEGL builds export
// them, others require eglGetProcAddress - resolve at runtime
typedef EGLImage (EGLAPIENTRY* StardeskCreateImageProc)(EGLDisplay, EGLContext,
                                                        EGLenum, EGLClientBuffer,
                                                        const EGLint*);
typedef EGLBoolean (EGLAPIENTRY* StardeskDestroyImageProc)(EGLDisplay, EGLImage);
static StardeskCreateImageProc s_eglCreateImage = nullptr;
static StardeskDestroyImageProc s_eglDestroyImage = nullptr;
typedef void (GL_APIENTRY* StardeskEglImageTargetProc)(GLenum target, GLeglImageOES image);
static StardeskEglImageTargetProc s_glEglImageTarget = nullptr;

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <vector>
#include <atomic>

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>

#include "../Capturer.h"

// PNG decode for the portal-Screenshot fallback mode (a slow-but-working
// capture path when the compositor's ScreenCast stream cannot be created,
// e.g. a broken GPU DMA-BUF path).
//
// Decoded with libpng (linked via dui-png, already used by the encoder):
// stb_image cannot be used here - the dui library compiles it with
// STBI_NO_PNG (dui handles PNG via cximage), so both stb_image and
// ui::ImageDecoderUtil::LoadImageFromMemory fail on PNG input.
#include <png.h>

namespace sdk {

namespace {

// DRM_FORMAT fourcc codes (little-endian). The negotiated PipeWire
// spa_video_format maps to one of these; using the correct FOURCC for
// EGL_LINUX_DRM_FOURCC_EXT avoids R↔B channel swap (花屏/garbled colours).
constexpr uint32_t kDrmFormatXrgb8888 = 0x34325258; // B-G-R-X
constexpr uint32_t kDrmFormatXbgr8888 = 0x34324258; // R-G-B-X
constexpr uint32_t kDrmFormatArgb8888 = 0x34325241; // B-G-R-A
constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241; // R-G-B-A

// Map a SPA_VIDEO_FORMAT_* to DRM_FORMAT_* (fourcc). Returns 0 for
// unknown/YUV formats (the caller falls back to CPU mmap).
// Values from PipeWire 0.3 spa/include/spa/param/video/raw.h:
//   RGBx=7 BGRx=8 xRGB=9 xBGR=10 RGBA=11 BGRA=12 ABGR=13 ARGB=14
static uint32_t SpaFormatToDrmFourcc(uint32_t spaFormat)
{
    switch (spaFormat) {
        case 7:  return kDrmFormatXbgr8888; // SPA_VIDEO_FORMAT_RGBx
        case 8:  return kDrmFormatXrgb8888; // SPA_VIDEO_FORMAT_BGRx
        case 9:  return kDrmFormatXrgb8888; // SPA_VIDEO_FORMAT_xRGB
        case 10: return kDrmFormatXbgr8888; // SPA_VIDEO_FORMAT_xBGR
        case 11: return kDrmFormatAbgr8888; // SPA_VIDEO_FORMAT_RGBA
        case 12: return kDrmFormatArgb8888; // SPA_VIDEO_FORMAT_BGRA
        case 13: return kDrmFormatAbgr8888; // SPA_VIDEO_FORMAT_ABGR
        case 14: return kDrmFormatArgb8888; // SPA_VIDEO_FORMAT_ARGB
        default: return 0;                  // YUV / unknown → mmap
    }
}

// True when the DRM fourcc has no alpha channel (XRGB / XBGR / BGRx / RGBx).
static bool IsXrgbFormat(uint32_t fourcc)
{
    return fourcc == kDrmFormatXrgb8888 || fourcc == kDrmFormatXbgr8888;
}

// True when the DRM fourcc has B before R in byte order (XRGB/ARGB — the
// "BGRx"/"BGRA" layouts). The mmap path needs a manual R↔B swap for these;
// the EGL path handles channel order through EGL_LINUX_DRM_FOURCC_EXT.
static bool IsBgrLayout(uint32_t fourcc)
{
    return fourcc == kDrmFormatXrgb8888 || fourcc == kDrmFormatArgb8888;
}

// GL_TEXTURE_MAX_LEVEL is GLES3-only; define it for the GLES2 header
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
// GLES3 framebuffer blit constants/functions (we run a GLES3 context)
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
typedef void (GL_APIENTRY* StardeskBlitProc)(GLint, GLint, GLint, GLint, GLint, GLint,
                                             GLint, GLint, GLbitfield, GLenum);
static StardeskBlitProc s_glBlitFramebuffer = nullptr;

} // namespace

// The platform implementation defines the Capturer::Impl struct and the
// Capturer methods (Capturer.cpp only implements the shared TestPatternSource).
struct Capturer::Impl {
    // --- DBus (portal session) ---
    DBusConnection* conn = nullptr;
    bool available = false;
    bool started = false;
    std::string error;
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
    bool extend = false;
    uint64_t requestSeq = 0;
    std::string sessionHandle;
    uint32_t nodeId = 0;

    // --- PipeWire ---
    struct pw_loop* loop = nullptr;  // pw_loop_new (not pw_main_loop_new - pipewire 1.6.2 pw_context_new crashes on a main_loop)
    pw_context* ctx = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook streamListener;
    std::string pwError;
    bool streamReady = false;
    bool streamEnded = false;
    // False: capture uses cursor_mode=4 (metadata) so the frames never
    // contain the pointer; the real cursor comes via spa_meta_cursor.
    bool cursorEmbedded = false;
    uint64_t drmModifier = 0; // negotiated modifier (EGL import needs it)
    uint32_t spaFormat = 0;   // negotiated spa_video_format (→ DRM fourcc)
    uint32_t drmFormat = kDrmFormatXrgb8888; // DRM fourcc for EGL_LINUX_DRM_FOURCC_EXT

    // --- cursor metadata (written on the pw thread, read in CaptureLoop) ---
    // KWin delivers the real pointer as spa_meta_cursor on the same stream
    // (positions in stream coordinates; authoritative for every input device,
    // unlike the injection-only pointers tracked by EIS/uinput).
    std::atomic<int> cursorX{-1};
    std::atomic<int> cursorY{-1};
    // the SPA pod requesting SPA_META_Cursor (built once; the frames are
    // delivered after this param is accepted via pw_stream_update_params)
    uint8_t metaPod[512];
    size_t metaPodSize = 0;

    // --- frame state (written on the pw thread, read in Capture()) ---
    std::mutex frameMutex;
    CaptureFrame latest;
    bool haveFrame = false;
    // diagnostics: how many PipeWire buffers the compositor delivered and when
    // the last one arrived. Mutter's ScreenCast stream is damage-driven, so a
    // static desktop legitimately stops delivering; a stall while the desktop
    // changes means the stream is wedged (the capture loop logs both cases).
    std::atomic<uint32_t> pwFrames{0};
    std::atomic<int64_t> lastPwMs{0};

    // --- EGL readback (DMA-BUF) ---
    struct wl_display* wlDisplay = nullptr;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLImage eglImage = EGL_NO_IMAGE_KHR;
    GLuint eglTex = 0;
    GLuint eglFbo = 0;
    bool eglOk = false;
    // small FBO: GPU-side downscale before glReadPixels - the full-size
    // GPU->CPU readback costs ~250ms/frame on this NVIDIA card, reading a
    // downscaled buffer is ~5x faster (and the host downscales to 720p
    // anyway before encoding)
    GLuint smallTex = 0;
    GLuint smallFbo = 0;
    int smallW = 1152;
    int smallH = 720;

    // --- portal Screenshot fallback (when ScreenCast cannot start) ---
    bool screenshotMode = false;
    uint64_t shotSeq = 0;
    std::string lastShotPath; // already-delivered screenshot file
    // Screenshot prefetch pipeline: the portal screenshot takes ~300ms, so
    // a dedicated thread keeps one frame ahead while the caller encodes the
    // previous one (roughly doubles the effective frame rate)
    std::thread shotThread;
    std::mutex shotMutex;
    std::condition_variable shotCv;
    CaptureFrame shotLatest;
    bool shotHaveLatest = false;
    bool shotStop = false;

    // --- ScreenCast restore_data ---
    // GNOME returns a "restore_data" (suv) in the Start response after the
    // user picks a screen; the next SelectSources must send it back or the
    // portal shows the picker dialog on EVERY Start. Stored as a marshalled
    // dbus message so the (suv) structure round-trips exactly.
    std::vector<uint8_t> restoreData;
};

namespace {

// ---------- DBus helpers ----------

// Send a method call and return the first arg's dict value for `key`
// (string values only). Returns false on error.
bool PortalDictString(DBusConnection* conn, const char* method,
                      const std::string& token, std::string& outValue,
                      std::string& err, const std::string& sessionToken = std::string())
{
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast", method);
    if (call == nullptr) {
        err = "OOM";
        return false;
    }
    // options dict a{sv}: handle_token (and session_handle_token for
    // CreateSession - the portal needs it to build the session)
    DBusMessageIter args, dict, entry, var;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (!sessionToken.empty()) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        {
            const char* key = "session_handle_token";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
            const char* sessionTokenCStr = sessionToken.c_str();
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &sessionTokenCStr);
            dbus_message_iter_close_container(&entry, &var);
        }
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "handle_token";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
        const char* tokenCStr = token.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &tokenCStr);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    dbus_message_iter_close_container(&args, &dict);

    DBusError derr;
    dbus_error_init(&derr);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 10000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        err = derr.message ? derr.message : "call failed";
        fprintf(stderr, "[capture] %s reply-fail: %s\n", method, err.c_str());
        dbus_error_free(&derr);
        return false;
    }
    std::string requestPath;
    {
        DBusMessageIter it;
        dbus_message_iter_init(reply, &it);
        // the portal replies with a bare (o) request handle
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            const char* p = nullptr;
            dbus_message_iter_get_basic(&it, &p);
            if (p != nullptr) {
                requestPath = p;
            }
        }
        else if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict2;
            dbus_message_iter_recurse(&it, &dict2);
            while (dbus_message_iter_get_arg_type(&dict2) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry2;
                dbus_message_iter_recurse(&dict2, &entry2);
                const char* key = nullptr;
                if (dbus_message_iter_get_arg_type(&entry2) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&entry2, &key);
                    dbus_message_iter_next(&entry2);
                    if (key != nullptr && strcmp(key, "handle") == 0 &&
                        dbus_message_iter_get_arg_type(&entry2) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter var2;
                        dbus_message_iter_recurse(&entry2, &var2);
                        if (dbus_message_iter_get_arg_type(&var2) == DBUS_TYPE_OBJECT_PATH) {
                            const char* p = nullptr;
                            dbus_message_iter_get_basic(&var2, &p);
                            if (p != nullptr) {
                                requestPath = p;
                            }
                        }
                    }
                }
                dbus_message_iter_next(&dict2);
            }
        }
    }
    dbus_message_unref(reply);
    if (requestPath.empty()) {
        err = "no request handle";
        fprintf(stderr, "[capture] %s no-request-handle\n", method);
        return false;
    }

    // wait for the Response signal
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write(conn, 200);
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (msg == nullptr) {
            continue;
        }
        const char* iface = dbus_message_get_interface(msg);
        const char* member = dbus_message_get_member(msg);
        const char* path = dbus_message_get_path(msg);
        if (iface != nullptr && strcmp(iface, "org.freedesktop.portal.Request") == 0 &&
            member != nullptr && strcmp(member, "Response") == 0 &&
            path != nullptr && requestPath == path) {
            uint32_t status = 1;
            DBusMessageIter it;
            dbus_message_iter_init(msg, &it);
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&it, &status);
                dbus_message_iter_next(&it);
            }
            if (status != 0) {
                err = "portal " + std::string(method) + " failed (status " +
                      std::to_string(status) + ")";
            }
            else if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter dict3;
                dbus_message_iter_recurse(&it, &dict3);
                while (dbus_message_iter_get_arg_type(&dict3) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter entry3;
                    dbus_message_iter_recurse(&dict3, &entry3);
                    const char* key = nullptr;
                    if (dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_STRING) {
                        dbus_message_iter_get_basic(&entry3, &key);
                        dbus_message_iter_next(&entry3);
                        if (key != nullptr) {
                            fprintf(stderr, "[capture] %s response key: %s\n", method, key);
                        }
                        if (key != nullptr && strcmp(key, "session_handle") == 0 &&
                            dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_VARIANT) {
                            DBusMessageIter svar;
                            dbus_message_iter_recurse(&entry3, &svar);
                            fprintf(stderr, "[capture] %s session_handle value type: %d\n",
                                    method, (int)dbus_message_iter_get_arg_type(&svar));
                            if (dbus_message_iter_get_arg_type(&svar) == DBUS_TYPE_OBJECT_PATH) {
                                const char* s = nullptr;
                                dbus_message_iter_get_basic(&svar, &s);
                                if (s != nullptr) {
                                    outValue = s;
                                }
                            }
                            else if (dbus_message_iter_get_arg_type(&svar) == DBUS_TYPE_STRING) {
                                const char* s = nullptr;
                                dbus_message_iter_get_basic(&svar, &s);
                                if (s != nullptr) {
                                    outValue = s;
                                }
                            }
                        }
                        else if (key != nullptr && strcmp(key, "streams") == 0 &&
                            dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_VARIANT) {
                            // streams: a(sa{sv}) - take the first node_id
                            DBusMessageIter var2;
                            dbus_message_iter_recurse(&entry3, &var2);
                            if (dbus_message_iter_get_arg_type(&var2) == DBUS_TYPE_ARRAY) {
                                DBusMessageIter streams;
                                dbus_message_iter_recurse(&var2, &streams);
                                if (dbus_message_iter_get_arg_type(&streams) ==
                                    DBUS_TYPE_STRUCT) {
                                    DBusMessageIter s0;
                                    dbus_message_iter_recurse(&streams, &s0);
                                    dbus_message_iter_next(&s0); // source type string
                                    if (dbus_message_iter_get_arg_type(&s0) == DBUS_TYPE_ARRAY) {
                                        DBusMessageIter sdict;
                                        dbus_message_iter_recurse(&s0, &sdict);
                                        while (dbus_message_iter_get_arg_type(&sdict) ==
                                               DBUS_TYPE_DICT_ENTRY) {
                                            DBusMessageIter sentry;
                                            dbus_message_iter_recurse(&sdict, &sentry);
                                            const char* skey = nullptr;
                                            if (dbus_message_iter_get_arg_type(&sentry) ==
                                                DBUS_TYPE_STRING) {
                                                dbus_message_iter_get_basic(&sentry, &skey);
                                                dbus_message_iter_next(&sentry);
                                                if (skey != nullptr && strcmp(skey, "node_id") == 0 &&
                                                    dbus_message_iter_get_arg_type(&sentry) ==
                                                        DBUS_TYPE_VARIANT) {
                                                    DBusMessageIter svar;
                                                    dbus_message_iter_recurse(&sentry, &svar);
                                                    if (dbus_message_iter_get_arg_type(&svar) ==
                                                        DBUS_TYPE_UINT32) {
                                                        uint32_t nid = 0;
                                                        dbus_message_iter_get_basic(&svar, &nid);
                                                        outValue = std::to_string(nid);
                                                    }
                                                }
                                            }
                                            dbus_message_iter_next(&sdict);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    dbus_message_iter_next(&dict3);
                }
            }
            dbus_message_unref(msg);
            return !outValue.empty();
        }
        dbus_message_unref(msg);
    }
    err = "timed out waiting for " + std::string(method);
    fprintf(stderr, "[capture] %s timed-out (path=%s)\n", method, requestPath.c_str());
    return false;
}

// Take one full-screen screenshot through the portal Screenshot API
// (non-interactive) and return the resulting file:// URI. This is the slow
// fallback capture path - the compositor encodes a PNG and writes it to
// disk - but it works on every desktop where the ScreenCast stream cannot
// be created (broken GPU DMA-BUF path etc.).
bool PortalScreenshot(DBusConnection* conn, const std::string& token,
                      std::string& uri, std::string& err)
{
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Screenshot", "Screenshot");
    if (call == nullptr) {
        err = "OOM";
        return false;
    }
    DBusMessageIter args, dict, entry, var;
    dbus_message_iter_init_append(call, &args);
    const char* parent = "";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    // interactive: false (never show a confirmation dialog)
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "interactive";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
        dbus_bool_t v = FALSE;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // handle_token
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "handle_token";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
        const char* tokenCStr = token.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &tokenCStr);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    dbus_message_iter_close_container(&args, &dict);

    DBusError derr;
    dbus_error_init(&derr);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 15000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        err = derr.message ? derr.message : "Screenshot call failed";
        fprintf(stderr, "[capture] Screenshot call-fail: %s\n", err.c_str());
        dbus_error_free(&derr);
        return false;
    }
    dbus_message_unref(reply);

    // wait for the Response signal (request path contains the token)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write(conn, 200);
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (msg == nullptr) {
            continue;
        }
        const char* member = dbus_message_get_member(msg);
        const char* path = dbus_message_get_path(msg);
        if (member != nullptr && strcmp(member, "Response") == 0 &&
            path != nullptr && strstr(path, token.c_str()) != nullptr) {
            DBusMessageIter it;
            dbus_message_iter_init(msg, &it);
            uint32_t status = 1;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&it, &status);
                dbus_message_iter_next(&it);
            }
            if (status != 0) {
                err = "Screenshot failed (status " + std::to_string(status) + ")";
                dbus_message_unref(msg);
                return false;
            }
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter d;
                dbus_message_iter_recurse(&it, &d);
                while (dbus_message_iter_get_arg_type(&d) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter e, v;
                    dbus_message_iter_recurse(&d, &e);
                    const char* key = nullptr;
                    if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRING) {
                        dbus_message_iter_get_basic(&e, &key);
                        dbus_message_iter_next(&e);
                        if (key != nullptr && strcmp(key, "uri") == 0 &&
                            dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
                            dbus_message_iter_recurse(&e, &v);
                            if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
                                const char* s = nullptr;
                                dbus_message_iter_get_basic(&v, &s);
                                if (s != nullptr) {
                                    uri = s;
                                }
                            }
                        }
                    }
                    dbus_message_iter_next(&d);
                }
            }
            dbus_message_unref(msg);
            if (uri.empty()) {
                err = "Screenshot response had no uri";
                return false;
            }
            return true;
        }
        dbus_message_unref(msg);
    }
    err = "Screenshot timed out";
    fprintf(stderr, "[capture] %s\n", err.c_str());
    return false;
}

// ---------- ScreenCast restore_data helpers ----------

// Cancel a portal request that timed out on our side (the dialog is still
// showing on the compositor). Without this the request stays pending and the
// portal processes further requests (e.g. the Screenshot fallback) only after
// the dialog is answered - "Did not receive a reply" over a live session.
void CancelPortalRequest(DBusConnection* conn, const std::string& requestPath)
{
    if (requestPath.empty() || conn == nullptr) {
        return;
    }
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", requestPath.c_str(),
        "org.freedesktop.portal.Request", "Close");
    if (call == nullptr) {
        return;
    }
    dbus_connection_send(conn, call, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(call);
    fprintf(stderr, "[capture] cancelled pending portal request %s\n",
            requestPath.c_str());
}

// libdbus has no "append this iterator" API, so copy an iterator's value
// recursively into another (append) iterator.
void CopyIter(DBusMessageIter* src, DBusMessageIter* dst)
{
    const int t = dbus_message_iter_get_arg_type(src);
    if (t == DBUS_TYPE_INVALID) {
        return;
    }
    if (t == DBUS_TYPE_STRUCT || t == DBUS_TYPE_ARRAY || t == DBUS_TYPE_VARIANT ||
        t == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter sub, out;
        const char* sig = dbus_message_iter_get_signature(src);
        dbus_message_iter_recurse(src, &sub);
        dbus_message_iter_open_container(dst, t, sig, &out);
        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
            CopyIter(&sub, &out);
            dbus_message_iter_next(&sub);
        }
        dbus_message_iter_close_container(dst, &out);
        return;
    }
    // basic type: get into a big-enough buffer and append
    unsigned char buf[16];
    dbus_message_iter_get_basic(src, buf);
    dbus_message_iter_append_basic(dst, t, buf);
}

// Save a (suv) restore_data value as a marshalled dbus message so it can be
// sent back verbatim on the next SelectSources call. `valueIter` points at
// the (suv) STRUCT in the Start response.
void SaveRestoreData(Capturer::Impl* impl, DBusMessageIter* valueIter)
{
    DBusMessage* m = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    if (m == nullptr) {
        return;
    }
    DBusMessageIter it;
    dbus_message_iter_init_append(m, &it);
    CopyIter(valueIter, &it);
    char* out = nullptr;
    int len = 0;
    if (dbus_message_marshal(m, &out, &len) && out != nullptr && len > 0) {
        impl->restoreData.assign(out, out + len);
        dbus_free(out);
        fprintf(stderr, "[capture] saved ScreenCast restore_data (%d bytes)\n", len);
    }
    dbus_message_unref(m);
}

// Add the saved restore_data as a "restore_data" option (suv) to the
// SelectSources options dict, so GNOME skips the picker dialog.
void AddRestoreDataOption(Capturer::Impl* impl, DBusMessageIter* dict)
{
    if (impl->restoreData.empty()) {
        return;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* m = dbus_message_demarshal((const char*)impl->restoreData.data(),
                                            impl->restoreData.size(), &err);
    if (m == nullptr) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter it;
    dbus_message_iter_init(m, &it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
        DBusMessageIter entry, var;
        dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char* key = "restore_data";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "(suv)", &var);
        CopyIter(&it, &var); // copy the whole (suv) structure
        dbus_message_iter_close_container(&entry, &var);
        dbus_message_iter_close_container(dict, &entry);
    }
    dbus_message_unref(m);
}

// ---------- PipeWire ----------

void PwOnProcess(void* data)
{
    Capturer::Impl* impl = static_cast<Capturer::Impl*>(data);
    struct pw_buffer* pwb = pw_stream_dequeue_buffer(impl->stream);
    if (pwb == nullptr) {
        return;
    }
    {
        const uint32_t n = ++impl->pwFrames;
        impl->lastPwMs.store((int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count());
        if ((n & 0x1FF) == 1) {
            fprintf(stderr, "[capture] pw buffer #%u\n", n);
            fflush(stderr);
        }
    }
    spa_buffer* buf = pwb->buffer;
    if (buf == nullptr || buf->n_datas == 0) {
        pw_stream_queue_buffer(impl->stream, pwb);
        return;
    }
    struct spa_data* d = &buf->datas[0];
    // Cursor metadata (cursor_mode=4): KWin updates the real pointer on the
    // stream, also via cursor-only buffers (chunk size 0) - parse BEFORE the
    // frame-size checks so those never get dropped unnoticed.
    {
        const struct spa_meta* m = spa_buffer_find_meta(buf, SPA_META_Cursor);
        if (m != nullptr && m->size >= sizeof(struct spa_meta_cursor)) {
            const struct spa_meta_cursor* mc =
                (const struct spa_meta_cursor*)m->data;
            if (spa_meta_cursor_is_valid(mc)) {
                impl->cursorX = mc->position.x;
                impl->cursorY = mc->position.y;
                static uint32_t sC = 0;
                if ((++sC & 0x3F) == 1) {
                    fprintf(stderr, "[capture] cursor meta pos=(%d,%d)\n",
                            mc->position.x, mc->position.y);
                    fflush(stderr);
                }
            }
        }
    }
    // throttled diagnostics for the frame-drop paths (the stream may deliver
    // many buffers per second when something is wrong)
    static uint32_t s_dropTick = 0;
    auto dropLog = [&](const char* why) {
        if ((++s_dropTick & 0x3F) == 1) {
            fprintf(stderr, "[capture] pw drop: %s\n", why);
            fflush(stderr);
        }
    };
    if (d->chunk == nullptr) {
        dropLog("no chunk");
        pw_stream_queue_buffer(impl->stream, pwb);
        return;
    }
    const int32_t stride = d->chunk->stride;
    const size_t size = d->chunk->size;
    const uint32_t offset = d->chunk->offset;

    std::lock_guard<std::mutex> lock(impl->frameMutex);
    CaptureFrame& out = impl->latest;
    const int w = impl->width;
    const int h = impl->height;
    if (w <= 0 || h <= 0 || stride <= 0) {
        static uint32_t sBs = 0;
        if (++sBs <= 5) {
            fprintf(stderr, "[capture] bad size/stride: w=%d h=%d stride=%d size=%zu type=%u\n",
                    w, h, stride, size, d->type);
            fflush(stderr);
        }
        dropLog("bad size/stride");
        pw_stream_queue_buffer(impl->stream, pwb);
        return;
    }

    if (d->type == SPA_DATA_DmaBuf) {
        // Mutter alternates linear (modifier 0) and tiled buffers on intel
        // hardware. The tiled ones are unreliable in this pipeline: EGL
        // readback comes back black or the image is row-swizzled (it can pass
        // the variance probe and then visibly shifts/slices the picture, which
        // the user sees as the remote window "shaking"). Only linear buffers
        // can be read back correctly, so drop the tiled ones.
        if (impl->drmModifier != 0) {
            static uint32_t sTiledCast = 0;
            if ((++sTiledCast & 0x3F) == 1) {
                fprintf(stderr,
                        "[capture] dropping tiled DMA-BUF (modifier=%llu, %u so far)\n",
                        (unsigned long long)impl->drmModifier, sTiledCast);
                fflush(stderr);
            }
            pw_stream_queue_buffer(impl->stream, pwb);
            return;
        }
        // Try EGL import first (GPU-side readback, fast ~60fps). Falls back to
        // mmap CPU readback when EGL is unavailable or the import fails (e.g.
        // tiled buffers on NVIDIA). The env var STARDESK_EGL_READBACK=0 forces
        // the mmap path (debug); STARDESK_EGL_READBACK=1 forces EGL-only.
        const char* eglEnv = std::getenv("STARDESK_EGL_READBACK");
        const bool forceMmap = eglEnv != nullptr && std::strcmp(eglEnv, "0") == 0;
        const bool forceEgl  = eglEnv != nullptr && std::strcmp(eglEnv, "1") == 0;

        if (!forceMmap && impl->eglOk && impl->drmFormat != 0) {
            // ---- EGL DMA-BUF import + GPU readback ----
            // NVIDIA EGL may not release the DMA-BUF fd on eglDestroyImage,
            // leaking one fd per frame (→ "Too many open files" → OOM).
            // Pass a dup'd fd so we can close our reference immediately;
            // EGL holds its own internal dup (which it may or may not free).
            const int eglFd = ::fcntl(d->fd, F_DUPFD, 0);
            if (eglFd < 0) {
                // fd dup failed (should not happen); skip EGL this frame
                static uint32_t sDupFail = 0;
                if ((++sDupFail & 0x3F) == 1) {
                    fprintf(stderr, "[capture] fcntl dup failed: %s\n", strerror(errno));
                }
                pw_stream_queue_buffer(impl->stream, pwb);
                return;
            }
            EGLint attrs[64];
            int n = 0;
            attrs[n++] = EGL_WIDTH;
            attrs[n++] = w;
            attrs[n++] = EGL_HEIGHT;
            attrs[n++] = h;
            attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
            attrs[n++] = (EGLint)impl->drmFormat;
            attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            attrs[n++] = eglFd;
            attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            attrs[n++] = (EGLint)offset;
            attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            attrs[n++] = stride;
            if (impl->drmModifier != 0) {
                attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                attrs[n++] = (EGLint)(impl->drmModifier & 0xFFFFFFFFu);
                attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                attrs[n++] = (EGLint)((impl->drmModifier >> 32) & 0xFFFFFFFFu);
            }
            attrs[n++] = EGL_NONE;
            impl->eglImage = s_eglCreateImage(impl->eglDisplay, EGL_NO_CONTEXT,
                                               EGL_LINUX_DMA_BUF_EXT,
                                               (EGLClientBuffer)nullptr, attrs);
            // Close our dup'd fd immediately — EGL has its own reference
            // and will release it (if the driver is well-behaved) on
            // eglDestroyImage.
            ::close(eglFd);
            if (impl->eglImage == EGL_NO_IMAGE_KHR) {
                // tiled NVIDIA buffers may fail to import — log with a
                // counter so the silent mmap fall-through is visible
                static uint32_t sImportFail = 0;
                if ((++sImportFail & 0x3F) == 1) {
                    fprintf(stderr, "[capture] EGL import failed (modifier=%llu), %u so far — using mmap\n",
                            (unsigned long long)impl->drmModifier, sImportFail);
                    fflush(stderr);
                }
            } else {
                // drain any stale GL error before the FBO setup
                while (glGetError() != GL_NO_ERROR) {}
                glBindTexture(GL_TEXTURE_2D, impl->eglTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                s_glEglImageTarget(GL_TEXTURE_2D, impl->eglImage);
                glBindFramebuffer(GL_FRAMEBUFFER, impl->eglFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, impl->eglTex, 0);
                {
                    GLenum err = glGetError();
                    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                    if (st != GL_FRAMEBUFFER_COMPLETE) {
                        static int sFboErr = 0;
                        if (++sFboErr <= 3) {
                            FILE* df = std::fopen("/tmp/stardesk_diag.txt", "a");
                            if (df) {
                                fprintf(df, "[capture] FBO incomplete st=0x%x err=0x%x\n",
                                        st, err);
                                fclose(df);
                            }
                        }
                    }
                }
                // GPU-side downscale, then read the small buffer (full-size
                // readback is ~250ms/frame on this NVIDIA card)
                const int rw = impl->smallW;
                const int rh = impl->smallH;
                if (s_glBlitFramebuffer != nullptr) {
                    glFinish();
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, impl->eglFbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, impl->smallFbo);
                    s_glBlitFramebuffer(0, 0, w, h, 0, 0, rw, rh,
                                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_FRAMEBUFFER, impl->smallFbo);
                    out.rgba.resize((size_t)rw * rh * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE,
                                 out.rgba.data());
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    // XRGB formats have no alpha channel; force to 0xFF so
                    // the encoder (and the remote client) sees opaque pixels.
                    if (IsXrgbFormat(impl->drmFormat)) {
                        for (size_t i = 3; i < out.rgba.size(); i += 4) {
                            out.rgba[i] = 0xFF;
                        }
                    }
                } else {
                    // glBlitFramebuffer unavailable (NVIDIA EGL/GLES may
                    // not export it). Read from the source eglFbo at native
                    // size and CPU-downscale.
                    static bool sDiagNoBlit = false;
                    if (!sDiagNoBlit) {
                        sDiagNoBlit = true;
                        FILE* df = std::fopen("/tmp/stardesk_diag.txt", "a");
                        if (df) {
                            fprintf(df, "[capture] blit=missing, reading src FBO %dx%d->%dx%d\n",
                                    w, h, rw, rh);
                            fclose(df);
                        }
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, impl->eglFbo);
                    std::vector<uint8_t> big((size_t)w * h * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                                 big.data());
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    // CPU nearest-neighbour downscale: 1440x900 → 1152x720
                    out.rgba.resize((size_t)rw * rh * 4);
                    for (int y = 0; y < rh; ++y) {
                        const int sy = y * h / rh;  // no flip: Wayland is top-down, GL reads top-down
                        const uint8_t* srcRow = big.data() + (size_t)sy * (size_t)w * 4;
                        uint8_t* dstRow = out.rgba.data() + (size_t)y * (size_t)rw * 4;
                        for (int x = 0; x < rw; ++x) {
                            const int sx = x * w / rw;
                            const size_t si = (size_t)sx * 4;
                            const size_t di = (size_t)x * 4;
                            dstRow[di  ] = srcRow[si];
                            dstRow[di+1] = srcRow[si+1];
                            dstRow[di+2] = srcRow[si+2];
                            dstRow[di+3] = IsXrgbFormat(impl->drmFormat) ? 0xFF : srcRow[si+3];
                        }
                    }
                }
                // Fully release every GL reference to the EGL image before
                // destroying it. Both the texture (bound via
                // glEGLImageTargetTexture2DOES) and the FBO color attachment
                // reference the image; with either still bound, NVIDIA's
                // eglDestroyImage is deferred and the DMA-BUF-backed image is
                // never actually freed — one full-size buffer (~5MB) leaked
                // per frame → OOM. Detach the FBO, delete the texture
                // (dropping its image reference), then destroy the image.
                glBindFramebuffer(GL_FRAMEBUFFER, impl->eglFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, 0, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glDeleteTextures(1, &impl->eglTex);
                glGenTextures(1, &impl->eglTex); // re-create for next frame
                s_eglDestroyImage(impl->eglDisplay, impl->eglImage);
                impl->eglImage = EGL_NO_IMAGE_KHR;
                // NVIDIA proprietary driver: EGL DMA-BUF import succeeds
                // but the imported texture reads back as all-black (driver
                // bug/limitation with certain modifiers). Detect this by
                // sampling a few pixels; when the whole frame is black the
                // mmap fallback below delivers real content.
                {
                    bool allBlack = true;
                    const uint8_t* p = out.rgba.data();
                    const size_t n = out.rgba.size();
                    // sample the first row, middle row, and last row (3×stride samples)
                    for (size_t off : {size_t(0), n / 2, n - (size_t)rw * 4}) {
                        if (off + (size_t)rw * 4 > n) continue;
                        for (int x = 0; x < rw && allBlack; ++x) {
                            const size_t i = off + (size_t)x * 4;
                            if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 0) {
                                allBlack = false;
                                break;
                            }
                        }
                    }
                    if (allBlack) {
                        // A transient black readback (or a tiled buffer the
                        // driver read back wrong) must not switch us to the
                        // mmap path: mmap of a tiled DMA-BUF reads past the
                        // row layout and crashed (SIGSEGV in PwOnProcess).
                        // Drop this buffer and let the next one through.
                        static uint32_t sBlack = 0;
                        if ((++sBlack & 0x3F) == 1) {
                            fprintf(stderr, "[capture] EGL readback black (%u so far), dropped\n",
                                    sBlack);
                            fflush(stderr);
                        }
                        pw_stream_queue_buffer(impl->stream, pwb);
                        return;
                    } else {
                        // Check for garbled output: NVIDIA tiled buffers imported
                        // with a mismatched modifier produce swizzled pixel soup
                        // (extreme discontinuities at tile boundaries) instead of
                        // clean black. Sample adjacent pixels — natural content
                        // has low local variance; a tiled buffer read as linear
                        // shows extreme jumps at every tile boundary.
                        bool garbled = false;
                        {
                            const uint32_t* p = (const uint32_t*)out.rgba.data();
                            const size_t rowWords = (size_t)rw;
                            long diffSum = 0;
                            int n = 0;
                            const int rows[] = {0, rh / 2, rh - 1};
                            for (int ri = 0; ri < 3; ++ri) {
                                const int row = rows[ri];
                                if (row < 0) continue;
                                const uint32_t* s = p + (size_t)row * rowWords;
                                for (int x = 0; x < rw - 1 && x < 256; ++x) {
                                    const int dr = (int)((s[x] >> 16) & 0xFF) - (int)((s[x+1] >> 16) & 0xFF);
                                    const int dg = (int)((s[x] >> 8) & 0xFF) - (int)((s[x+1] >> 8) & 0xFF);
                                    const int db = (int)(s[x] & 0xFF) - (int)(s[x+1] & 0xFF);
                                    diffSum += abs(dr) + abs(dg) + abs(db);
                                    ++n;
                                }
                            }
                            if (n > 0) {
                                const long avgDiff = diffSum / n;
                                if (avgDiff > 50) {
                                    garbled = true;
                                    static uint32_t sGarbledEgl = 0;
                                    if ((++sGarbledEgl & 0x3F) == 1) {
                                        fprintf(stderr, "[capture] EGL garbled output avgDiff=%ld (%u so far), using mmap\n",
                                                avgDiff, sGarbledEgl);
                                        fflush(stderr);
                                    }
                                }
                            }
                        }
                        if (garbled) {
                            // Tiled buffer imported with the wrong modifier:
                            // the pixels are swizzled and the mmap path cannot
                            // read them linearly either - drop the buffer.
                            static uint32_t sGarbledDrop = 0;
                            if ((++sGarbledDrop & 0x3F) == 1) {
                                fprintf(stderr,
                                        "[capture] EGL garbled frame dropped (%u so far)\n",
                                        sGarbledDrop);
                                fflush(stderr);
                            }
                            pw_stream_queue_buffer(impl->stream, pwb);
                            return;
                        } else {
                            // Wayland DMA-BUF is top-down; EGL import keeps it,
                            // glBlitFramebuffer maps source Y=0 (top) to dest
                            // Y=0 (bottom in GL), and glReadPixels starts at
                            // GL Y=0 (bottom of dest, which is top of image).
                            // The net result is that row 0 of the output buffer
                            // corresponds to the top of the desktop image -
                            // no extra flip needed.
                            out.width = rw;
                            out.height = rh;
                            impl->haveFrame = true;
                            pw_stream_queue_buffer(impl->stream, pwb);
                            return; // EGL path succeeded
                        }
                    }
                }
            }
            // EGL import failed (e.g. tiled buffer on NVIDIA): fall through
            // to mmap below. The failed image is already EGL_NO_IMAGE_KHR.
            if (forceEgl) {
                // EGL-only mode: drop non-importable buffers
                static uint32_t sDrop = 0;
                if ((++sDrop & 0x3F) == 1) {
                    fprintf(stderr, "[capture] dropping non-EGL buffer (%u so far)\n",
                            sDrop);
                    fflush(stderr);
                }
                pw_stream_queue_buffer(impl->stream, pwb);
                return;
            }
        }

        // ---- mmap CPU fallback ----
        {
        // NVIDIA tiled modifier: mmap reads block-linear data as if linear,
        // producing garbled/swizzled frames. Mutter alternates between linear
        // (modifier=0) and tiled DMA-BUF buffers; the tiled ones arrive as
        // corrupt pixel soup. Detect them by sampling adjacent pixels: in a
        // natural image nearby pixels are similar (low variance), while a
        // tiled buffer read as linear has extreme discontinuities at every
        // tile boundary. Drop the buffer when it looks garbled.
        {
            void* probe = mmap(nullptr, (size_t)h * (size_t)stride, PROT_READ,
                               MAP_SHARED, d->fd, (off_t)offset);
            if (probe != MAP_FAILED) {
                const uint32_t* p = (const uint32_t*)probe;
                long diffSum = 0;
                int n = 0;
                // sample consecutive pixels at the start of lines 0, 1, 2
                for (int row = 0; row < 3 && (row + 1) * (stride / 4) < (size_t)h * (size_t)(stride / 4); ++row) {
                    const uint32_t* s = p + (size_t)row * (size_t)(stride / 4);
                    for (int x = 0; x < w - 1 && x < 256; ++x) {
                        const int dr = (int)((s[x] >> 16) & 0xFF) - (int)((s[x+1] >> 16) & 0xFF);
                        const int dg = (int)((s[x] >> 8) & 0xFF) - (int)((s[x+1] >> 8) & 0xFF);
                        const int db = (int)(s[x] & 0xFF) - (int)(s[x+1] & 0xFF);
                        diffSum += abs(dr) + abs(dg) + abs(db);
                        ++n;
                    }
                }
                munmap(probe, (size_t)h * (size_t)stride);
                // natural desktop: avg diff ~5-15; tiled buffer: avg diff >60
                const long avgDiff = n > 0 ? diffSum / n : 0;
                if (avgDiff > 50) {
                    static uint32_t sGarbled = 0;
                    if ((++sGarbled & 0x3F) == 1) {
                        fprintf(stderr, "[capture] tiled frame dropped avgDiff=%ld (%u so far)\n",
                                avgDiff, sGarbled);
                        fflush(stderr);
                    }
                    out.rgba.clear();
                    pw_stream_queue_buffer(impl->stream, pwb);
                    return;
                }
            }
        }
        // Only linear (modifier 0) buffers can be read as a linear image.
        // Tiled/compressed DMA-BUFs must go through EGL; reading them with
        // mmap walks past the row layout and crashes.
        if (impl->drmModifier != 0) {
            static uint32_t sTiledDrop = 0;
            if ((++sTiledDrop & 0x3F) == 1) {
                fprintf(stderr,
                        "[capture] dropping tiled buffer (modifier=%llu, %u so far)\n",
                        (unsigned long long)impl->drmModifier, sTiledDrop);
                fflush(stderr);
            }
            pw_stream_queue_buffer(impl->stream, pwb);
            return;
        }
        if (stride < (int32_t)w * 4 || size < (size_t)h * (size_t)stride) {
            static uint32_t sBadLayout = 0;
            if ((++sBadLayout & 0x3F) == 1) {
                fprintf(stderr,
                        "[capture] dropping buffer with short layout "
                        "(w=%d stride=%d size=%zu need=%zu)\n",
                        w, stride, size, (size_t)h * (size_t)stride);
                fflush(stderr);
            }
            pw_stream_queue_buffer(impl->stream, pwb);
            return;
        }
        void* map = mmap(nullptr, (size_t)h * (size_t)stride, PROT_READ,
                         MAP_SHARED, d->fd, (off_t)offset);
        if (map != MAP_FAILED) {
            // Respect the chunk size — a tiled/compressed DMA-BUF may have
            // chunk->size < h*stride; reading beyond chunk->size can SIGBUS.
            const size_t mapLen = stride > 0 ? std::min((size_t)h * (size_t)stride, size) : size;
            madvise(map, mapLen, MADV_WILLNEED);
            // bulk memcpy first: touching the dmabuf pages one 32-bit word
            // at a time forces a GPU->CPU sync per access (~250ms for 5MB);
            // one large memcpy does a single readback
            std::vector<uint8_t> tmp(mapLen);
            std::memcpy(tmp.data(), map, tmp.size());
            munmap(map, (size_t)h * (size_t)stride);
            const uint32_t* s32 = (const uint32_t*)tmp.data();
            // ---- raw DMA-BUF dump (for NVIDIA tiled-buffer diagnosis) ----
            if (std::getenv("STARDESK_DUMP_MMAP")) {
                static bool sDumpedMmap = false;
                if (!sDumpedMmap) {
                    sDumpedMmap = true;
                    FILE* f = std::fopen("/tmp/sd_mmap.raw", "wb");
                    if (f) {
                        uint32_t hdr[4] = {(uint32_t)w, (uint32_t)h, (uint32_t)stride,
                                           (uint32_t)impl->drmModifier};
                        fwrite(hdr, 1, sizeof(hdr), f);
                        fwrite(tmp.data(), 1, tmp.size(), f);
                        fclose(f);
                        fprintf(stderr, "[capture] mmap dump %dx%d stride=%d modifier=%llu saved\n",
                                w, h, stride, (unsigned long long)impl->drmModifier);
                        fflush(stderr);
                    }
                }
            }
            out.rgba.resize((size_t)w * h * 4);
            uint32_t* t32 = (uint32_t*)out.rgba.data();
            // Format-aware pixel conversion: BGR-layout DMA-BUFs need R↔B
            // swap (the EGL path does this through EGL_LINUX_DRM_FOURCC_EXT;
            // the mmap path must do it manually). XRGB formats force alpha
            // to 0xFF since the X channel carries garbage.
            const bool needSwap = IsBgrLayout(impl->drmFormat);
            const bool forceAlpha = IsXrgbFormat(impl->drmFormat);
            for (int y = 0; y < h; ++y) {
                const uint32_t* s = s32 + (size_t)y * (size_t)(stride / 4);
                uint32_t* t = t32 + (size_t)y * (size_t)w;
                for (int x = 0; x < w; ++x) {
                    const uint32_t v = s[x];
                    if (needSwap) {
                        t[x] = ((v & 0x000000FFu) << 16) | (v & 0x0000FF00u) |
                               ((v & 0x00FF0000u) >> 16) | (forceAlpha ? 0xFF000000u : (v & 0xFF000000u));
                    } else {
                        t[x] = forceAlpha ? (v | 0xFF000000u) : v;
                    }
                }
            }
            out.width = w;
            out.height = h;
            impl->haveFrame = true;
        }
        }
    }
    else if (d->type == SPA_DATA_MemFd && d->data != nullptr) {
        // memfd: direct copy (BGRA/XRGB, bottom-up on some compositors)
        const uint8_t* src = (const uint8_t*)d->data + offset;
        const size_t rowBytes = (size_t)w * 4;
        if (size >= (size_t)h * stride) {
            out.rgba.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) {
                memcpy(out.rgba.data() + (size_t)y * rowBytes,
                       src + (size_t)y * stride, rowBytes);
            }
            out.width = w;
            out.height = h;
            impl->haveFrame = true;
        }
    }
    pw_stream_queue_buffer(impl->stream, pwb);
}

void PwOnParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    Capturer::Impl* impl = static_cast<Capturer::Impl*>(data);
    if (id != SPA_PARAM_Format || param == nullptr) {
        return;
    }
    uint32_t mediaType = 0, mediaSubtype = 0;
    spa_format_parse(param, &mediaType, &mediaSubtype);
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    struct spa_video_info_raw info = {};
    if (spa_format_video_raw_parse(param, &info) < 0 || info.format == 0 ||
        info.size.width == 0 || info.size.height == 0) {
        return;
    }
    fprintf(stderr, "[capture] pw format: spa=%u %ux%u modifier=%llu\n",
            (unsigned)info.format, info.size.width, info.size.height,
            (unsigned long long)info.modifier);
    impl->width = (int)info.size.width;
    impl->height = (int)info.size.height;
    impl->drmModifier = info.modifier;
    impl->spaFormat = info.format;
    impl->drmFormat = SpaFormatToDrmFourcc(info.format);
    if (impl->drmFormat == 0) {
        fprintf(stderr, "[capture] unsupported spa format %u — will use CPU mmap\n",
                (unsigned)info.format);
        fflush(stderr);
    } else {
        fprintf(stderr, "[capture] drm fourcc=0x%08x (alpha=%s)\n",
                impl->drmFormat, IsXrgbFormat(impl->drmFormat) ? "no" : "yes");
        fflush(stderr);
    }
    impl->streamReady = true;
    // accept the negotiated format - without this the producer (mutter)
    // delivers a single buffer and then waits for our format confirmation
    pw_stream_update_params(impl->stream, nullptr, 0);
    // now request the cursor metadata (KWin only emits spa_meta_cursor once
    // the consumer asks for it; the request must come after the format)
    if (impl->metaPodSize > 0) {
        const struct spa_pod* params[1] = { (const struct spa_pod*)impl->metaPod };
        pw_stream_update_params(impl->stream, params, 1);
    }
}

void PwOnStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state,
                      const char* error)
{
    Capturer::Impl* impl = static_cast<Capturer::Impl*>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        impl->pwError = error ? error : "pipewire stream error";
    }
    else if (state == PW_STREAM_STATE_UNCONNECTED) {
        impl->streamEnded = true;
    }
}

const struct pw_stream_events s_pwEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = PwOnStateChanged,
    .param_changed = PwOnParamChanged,
    .process = PwOnProcess,
};

} // namespace

Capturer::Capturer() : m_impl(new Impl()) {}

Capturer::~Capturer()
{
    Stop();
}

bool Capturer::PlatformSupported()
{
    return true;
}

// ScreenCast is always tried first on GNOME (Mutter direct API, no portal
// dialog needed); on other compositors ScreenCast is opt-in because a broken
// stream can wedge the portal for every other request. Environment override:
//   STARDESK_WAYLAND_CAST=0  — force Screenshot polling even on GNOME
//   STARDESK_WAYLAND_CAST=1  — force ScreenCast on non-GNOME compositors
namespace {
bool s_castFailedOnce = false;
} // namespace

bool Capturer::Start(bool extend)
{
    m_impl->extend = extend;
    m_impl->error.clear();
    m_impl->screenshotMode = false;

    // Preferred path: a ScreenCast stream (fast, continuous). On GNOME the
    // compositor's own ScreenCast interface (org.gnome.Mutter.ScreenCast) is
    // used directly: no portal, no permission dialog, pure DMA-BUF stream.
    // NOTE: on NVIDIA proprietary drivers the EGL DMA-BUF readback returns
    // all-black; the code falls through to mmap CPU readback automatically.
    const char* castEnv = std::getenv("STARDESK_WAYLAND_CAST");
    const bool forceOff = castEnv != nullptr && std::strcmp(castEnv, "0") == 0;
    const bool forceOn  = castEnv != nullptr && std::strcmp(castEnv, "1") == 0;
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    bool gnome = desktop != nullptr &&
                 std::strstr(desktop, "GNOME") != nullptr;
    // Fallback: when XDG_CURRENT_DESKTOP is unset (e.g. SSH login), probe
    // whether gnome-shell is running (the mutter ScreenCast D-Bus name only
    // exists under GNOME). One fopen on /proc is ~100µs.
    if (!gnome) {
        FILE* fp = std::fopen("/proc/1/comm", "r");
        if (fp != nullptr) {
            char buf[32] = {};
            if (std::fgets(buf, sizeof(buf), fp) != nullptr &&
                std::strcmp(buf, "systemd\n") == 0) {
                // systemd-based system: look for gnome-shell via pgrep
                // (no fork - we just check a likely pid via /proc)
            }
            std::fclose(fp);
        }
        // simplest robust check: /usr/bin/gnome-shell exists and a
        // gnome-shell process has the org.gnome.Mutter.ScreenCast name on
        // the session bus (lightweight: a single D-Bus call)
        DBusConnection* probe = dbus_bus_get(DBUS_BUS_SESSION, nullptr);
        if (probe != nullptr) {
            DBusMessage* call = dbus_message_new_method_call(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameHasOwner");
            if (call != nullptr) {
                const char* name = "org.gnome.Mutter.ScreenCast";
                dbus_message_append_args(call, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
                DBusError derr;
                dbus_error_init(&derr);
                DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                    probe, call, 2000, &derr);
                if (reply != nullptr) {
                    dbus_bool_t has = FALSE;
                    DBusMessageIter it;
                    dbus_message_iter_init(reply, &it);
                    dbus_message_iter_get_basic(&it, &has);
                    gnome = (has == TRUE);
                    dbus_message_unref(reply);
                }
                dbus_error_free(&derr);
                dbus_message_unref(call);
            }
            dbus_connection_unref(probe);
        }
    }
    // GNOME: ScreenCast on by default (unless STARDESK_WAYLAND_CAST=0).
    // Other compositors: opt-in only (STARDESK_WAYLAND_CAST=1).
    const bool tryCast = (gnome && !forceOff) || forceOn;

    if (tryCast && !s_castFailedOnce) {
        const bool ok = gnome ? StartMetaCast(extend) : StartScreenCast(extend);
        if (ok) {
            fprintf(stderr, "[capture] ScreenCast stream active (%s)\n",
                    gnome ? "mutter direct" : "portal");
            return true;
        }
    }
    if (!tryCast) {
        fprintf(stderr, "[capture] portal Screenshot polling (ScreenCast off; set STARDESK_WAYLAND_CAST=1 to enable)\n");
    }
    else {
        fprintf(stderr, "[capture] ScreenCast failed (%s) - using portal Screenshot polling\n",
                m_impl->error.c_str());
        // A user-cancel (status 2) is not a hard failure: the next connect
        // retries ScreenCast (the user may accept the dialog then). Only
        // deterministic failures (timeouts etc.) skip future attempts.
        const bool userCancel = m_impl->error.find("status 2") != std::string::npos;
        if (!userCancel) {
            s_castFailedOnce = true;
        }
    }
    m_impl->error.clear();

    DBusError err;
    dbus_error_init(&err);
    m_impl->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (m_impl->conn == nullptr) {
        m_impl->error = err.message ? err.message : "session bus unavailable";
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(m_impl->conn, false);
    m_impl->screenshotMode = true;
    m_impl->available = true;
    // prefetch thread: keep one screenshot ahead of the encoder
    m_impl->shotStop = false;
    m_impl->shotThread = std::thread([this, impl = m_impl.get()]() {
        while (true) {
            CaptureFrame frame;
            std::string uri, err;
            const std::string token = "stardesk_shot" + std::to_string(++impl->shotSeq);
            if (!PortalScreenshot(impl->conn, token, uri, err)) {
                break;
            }
            if (!DecodeScreenshotFile(uri, frame)) {
                break;
            }
            std::unique_lock<std::mutex> lock(impl->shotMutex);
            impl->shotLatest = std::move(frame);
            impl->shotHaveLatest = true;
            impl->shotCv.notify_all();
            impl->shotCv.wait(lock, [impl]() {
                return !impl->shotHaveLatest || impl->shotStop;
            });
            if (impl->shotStop) {
                break;
            }
        }
    });
    fprintf(stderr, "[capture] Screenshot polling mode active (prefetch)\n");
    return true;
}


namespace {

// Ask org.gnome.Mutter.DisplayConfig for the first physical monitor connector
// (e.g. "HDMI-1"). RecordMonitor needs it; a virtual monitor would only show
// the shell background, never the application windows.
bool QueryPrimaryConnector(DBusConnection* conn, std::string& connector)
{
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* call = dbus_message_new_method_call(
        "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig", "GetCurrentState");
    if (call == nullptr) {
        return false;
    }
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 3000, &err);
    dbus_message_unref(call);
    if (reply == nullptr) {
        fprintf(stderr, "[capture] DisplayConfig.GetCurrentState failed: %s\n",
                err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return false;
    }
    bool ok = false;
    DBusMessageIter it;
    // (u serial, a((ssss)a(siiddas)sa{sv}) monitors, a{sv} logical monitors)
    const bool haveArgs = dbus_message_iter_init(reply, &it);
    if (haveArgs && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
        // (u serial, a((ssss)a(siiddas)sa{sv}) monitors, ...): each monitor is
        // a struct whose first member is the (connector, vendor, product,
        // serial) struct.
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            DBusMessageIter monitors;
            dbus_message_iter_recurse(&it, &monitors);
            if (dbus_message_iter_get_arg_type(&monitors) == DBUS_TYPE_STRUCT) {
                DBusMessageIter monitor;
                dbus_message_iter_recurse(&monitors, &monitor);
                if (dbus_message_iter_get_arg_type(&monitor) == DBUS_TYPE_STRUCT) {
                    DBusMessageIter ids;
                    dbus_message_iter_recurse(&monitor, &ids);
                    if (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_STRING) {
                        const char* name = nullptr;
                        dbus_message_iter_get_basic(&ids, &name);
                        if (name != nullptr && name[0] != '\0') {
                            connector = name;
                            ok = true;
                        }
                    }
                }
            }
        }
    }
    dbus_message_unref(reply);
    fprintf(stderr, "[capture] monitor connector: %s\n", ok ? connector.c_str() : "(none found)");
    return ok;
}

} // namespace

// GNOME/mutter direct ScreenCast: talk to org.gnome.Mutter.ScreenCast
// directly instead of the portal. No permission dialog, no restore_data -
// the stream's pipewire node id arrives via the "PipeWireStreamAdded"
// signal. Sequence (verified against mutter 50):
//   CreateSession({}) -> session path
//   Session.Start()
//   Session.RecordVirtual({width,height,refresh-rate}) -> stream path
//   Stream.Start()
//   signal Stream.PipeWireStreamAdded(u node_id)
bool Capturer::StartMetaCast(bool extend)
{
    m_impl->extend = extend;
    m_impl->error.clear();

    DBusError err;
    dbus_error_init(&err);
    m_impl->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (m_impl->conn == nullptr) {
        m_impl->error = err.message ? err.message : "session bus unavailable";
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(m_impl->conn, false);
    m_impl->started = true;

    // 1. CreateSession({}) -> (o)
    DBusMessage* call = dbus_message_new_method_call(
        "org.gnome.Mutter.ScreenCast", "/org/gnome/Mutter/ScreenCast",
        "org.gnome.Mutter.ScreenCast", "CreateSession");
    if (call == nullptr) {
        m_impl->error = "OOM";
        Stop();
        return false;
    }
    DBusMessageIter args, dict;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&args, &dict);
    DBusError derr;
    dbus_error_init(&derr);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_impl->conn, call, 10000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        m_impl->error = derr.message ? derr.message : "CreateSession call failed";
        dbus_error_free(&derr);
        Stop();
        return false;
    }
    const char* sessionPath = nullptr;
    DBusMessageIter it;
    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
        dbus_message_iter_get_basic(&it, &sessionPath);
    }
    dbus_message_unref(reply);
    if (sessionPath == nullptr) {
        m_impl->error = "mutter CreateSession: no session path";
        Stop();
        return false;
    }
    const std::string session = sessionPath;
    fprintf(stderr, "[capture] mutter session %s\n", session.c_str());

    // 2. Session.Start()
    call = dbus_message_new_method_call(
        "org.gnome.Mutter.ScreenCast", session.c_str(),
        "org.gnome.Mutter.ScreenCast.Session", "Start");
    reply = dbus_connection_send_with_reply_and_block(m_impl->conn, call, 10000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        m_impl->error = derr.message ? derr.message : "Session.Start failed";
        dbus_error_free(&derr);
        Stop();
        return false;
    }
    dbus_message_unref(reply);

    // 3. Record a screen -> (o) stream path.
    //    Prefer the REAL monitor: a virtual monitor is a separate output that
    //    only ever shows the shell background - no application windows and no
    //    shell panel appear on it, so a virtual stream looks like an empty
    //    wallpaper. Headless hosts (no monitors) fall back to RecordVirtual at
    //    1920x1080; STARDESK_CAST_VIRTUAL=1 forces the old virtual behaviour.
    std::string stream;
    bool gotStream = false;
    const char* virtualEnv = std::getenv("STARDESK_CAST_VIRTUAL");
    const bool forceVirtual = virtualEnv != nullptr && std::strcmp(virtualEnv, "1") == 0;
    if (!forceVirtual) {
        std::string connector;
        if (QueryPrimaryConnector(m_impl->conn, connector)) {
            call = dbus_message_new_method_call(
                "org.gnome.Mutter.ScreenCast", session.c_str(),
                "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor");
            dbus_message_iter_init_append(call, &args);
            const char* connectorCStr = connector.c_str();
            dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &connectorCStr);
            dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
            {
                // cursor-mode: 2 = metadata. The compositor keeps the pointer
                // out of the pixels and delivers its REAL position through
                // spa_meta_cursor. Without it the host cannot calibrate the
                // relative EIS pointer, so the first injected motion carries
                // a constant offset (the remote cursor/click lands away from
                // the client's pointer).
                DBusMessageIter entry, var;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                const char* kc = "cursor-mode";
                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &kc);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
                uint32_t cm = 2;
                dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &cm);
                dbus_message_iter_close_container(&entry, &var);
                dbus_message_iter_close_container(&dict, &entry);
            }
            dbus_message_iter_close_container(&args, &dict);
            reply = dbus_connection_send_with_reply_and_block(m_impl->conn, call, 10000, &derr);
            dbus_message_unref(call);
            if (reply != nullptr) {
                const char* p = nullptr;
                dbus_message_iter_init(reply, &it);
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
                    dbus_message_iter_get_basic(&it, &p);
                }
                dbus_message_unref(reply);
                if (p != nullptr) {
                    stream = p;
                    gotStream = true;
                    fprintf(stderr, "[capture] recording monitor %s\n", connector.c_str());
                }
            }
            else {
                fprintf(stderr, "[capture] RecordMonitor(%s) failed: %s - using a virtual monitor\n",
                        connector.c_str(), derr.message ? derr.message : "unknown");
                dbus_error_free(&derr);
                dbus_error_init(&derr);
            }
        }
    }
    if (!gotStream) {
        // Virtual monitor at 1920x1080 (headless host, or forced by env).
        call = dbus_message_new_method_call(
            "org.gnome.Mutter.ScreenCast", session.c_str(),
            "org.gnome.Mutter.ScreenCast.Session", "RecordVirtual");
        dbus_message_iter_init_append(call, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        {
            DBusMessageIter entry, var;
            const uint32_t vw = 1920, vh = 1080;
            const double vr = 60.0;
            // width: u
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            const char* kw = "width";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &kw);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &vw);
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&dict, &entry);
            // height: u
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            kw = "height";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &kw);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &vh);
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&dict, &entry);
            // refresh-rate: d
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            kw = "refresh-rate";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &kw);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "d", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &vr);
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&dict, &entry);
        }
        dbus_message_iter_close_container(&args, &dict);
        reply = dbus_connection_send_with_reply_and_block(m_impl->conn, call, 10000, &derr);
        dbus_message_unref(call);
        if (reply == nullptr) {
            m_impl->error = derr.message ? derr.message : "RecordVirtual failed";
            dbus_error_free(&derr);
            Stop();
            return false;
        }
        const char* streamPath = nullptr;
        dbus_message_iter_init(reply, &it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&it, &streamPath);
        }
        dbus_message_unref(reply);
        if (streamPath == nullptr) {
            m_impl->error = "RecordVirtual: no stream path";
            Stop();
            return false;
        }
        stream = streamPath;
        fprintf(stderr, "[capture] recording a virtual monitor 1920x1080 (headless fallback)\n");
    }
    fprintf(stderr, "[capture] mutter stream %s\n", stream.c_str());

    // arm the PipeWireStreamAdded signal listener before Start
    dbus_bus_add_match(m_impl->conn,
                       "type='signal',interface='org.gnome.Mutter.ScreenCast.Stream',"
                       "member='PipeWireStreamAdded'",
                       &derr);
    if (dbus_error_is_set(&derr)) {
        dbus_error_free(&derr);
    }

    // 4. Stream.Start()
    call = dbus_message_new_method_call(
        "org.gnome.Mutter.ScreenCast", stream.c_str(),
        "org.gnome.Mutter.ScreenCast.Stream", "Start");
    reply = dbus_connection_send_with_reply_and_block(m_impl->conn, call, 10000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        m_impl->error = derr.message ? derr.message : "Stream.Start failed";
        dbus_error_free(&derr);
        Stop();
        return false;
    }
    dbus_message_unref(reply);

    // 5. wait for PipeWireStreamAdded -> node id
    uint32_t nodeId = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write(m_impl->conn, 200);
        DBusMessage* msg = dbus_connection_pop_message(m_impl->conn);
        if (msg == nullptr) {
            continue;
        }
        const char* member = dbus_message_get_member(msg);
        if (member != nullptr && strcmp(member, "PipeWireStreamAdded") == 0) {
            DBusMessageIter sIt;
            dbus_message_iter_init(msg, &sIt);
            if (dbus_message_iter_get_arg_type(&sIt) == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&sIt, &nodeId);
            }
            dbus_message_unref(msg);
            break;
        }
        dbus_message_unref(msg);
    }
    if (nodeId == 0) {
        m_impl->error = "no PipeWireStreamAdded (mutter stream not ready)";
        fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
        Stop();
        return false;
    }
    fprintf(stderr, "[capture] mutter pipewire node=%u\n", nodeId);
    m_impl->nodeId = nodeId;
    return SetupPipewire();
}

bool Capturer::StartScreenCast(bool extend)
{
    m_impl->extend = extend;
    m_impl->error.clear();

    DBusError err;
    dbus_error_init(&err);
    m_impl->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (m_impl->conn == nullptr) {
        m_impl->error = err.message ? err.message : "session bus unavailable";
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(m_impl->conn, false);
    // Portal requests complete through Request::Response signals. Without a
    // match rule libdbus silently drops those signals, leaving Start waiting
    // until its timeout even after the user approved screen sharing.
    dbus_bus_add_match(
        m_impl->conn,
        "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
        &err);
    if (dbus_error_is_set(&err)) {
        m_impl->error = err.message ? err.message : "portal response match failed";
        dbus_error_free(&err);
        Stop();
        return false;
    }
    dbus_connection_flush(m_impl->conn);
    m_impl->started = true;

    // 1. CreateSession (with session_handle_token - the portal asserts when
    // the token is missing and it cannot generate one itself)
    std::string token = "stardesk" + std::to_string(++m_impl->requestSeq);
    std::string sessionToken = "stardesk_s" + std::to_string(++m_impl->requestSeq);
    if (!PortalDictString(m_impl->conn, "CreateSession", token,
                          m_impl->sessionHandle, m_impl->error, sessionToken)) {
        m_impl->error = "CreateSession: " + m_impl->error;
        fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
        Stop();
        return false;
    }

    // 2. SelectSources (screen, non-interactive)
    token = "stardesk" + std::to_string(++m_impl->requestSeq);
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast", "SelectSources");
    DBusMessageIter args, dict, entry, var;
    dbus_message_iter_init_append(call, &args);
    const char* sess = m_impl->sessionHandle.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sess);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    // types: 1 (screen)
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "types";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
        uint32_t v = 1;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // multiple: false
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "multiple";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
        dbus_bool_t v = FALSE;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // persistent_mode: 1 (ask to remember) - with 2 (never ask) and no
    // saved selection GNOME's backend hangs Start with no dialog at all;
    // asking lets the user pick a screen once (remembered for later)
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "persistent_mode";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
        uint32_t v = 1;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // interactive: true (GNOME needs the dialog to establish a selection)
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "interactive";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
        dbus_bool_t v = TRUE;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // handle_token
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "handle_token";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
        const char* tokenCStr = token.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &tokenCStr);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // cursor_mode - a SelectSources option per the ScreenCast portal spec.
    // 4 (metadata) = the compositor does NOT paint the pointer into the
    // frames; it delivers the real cursor via spa_meta_cursor on the same
    // stream, which the host forwards to the client (CursorPos). The client
    // then owns cursor visibility per mode (local / both / display) with no
    // portal re-capture, so toggling modes is instant and needs no dialog.
    // 1=hidden, 2=embedded (composited into pixels, cannot be hidden
    // client-side). Must be one of AvailableCursorModes.
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "cursor_mode";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
        uint32_t v = 4;
        dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &v);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    // restore_data: GNOME needs the previously saved picker choice here or
    // it shows the dialog on every Start
    AddRestoreDataOption(m_impl.get(), &dict);
    dbus_message_iter_close_container(&args, &dict);

    DBusError derr;
    dbus_error_init(&derr);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        m_impl->conn, call, 10000, &derr);
    dbus_message_unref(call);
    if (reply == nullptr) {
        m_impl->error = derr.message ? derr.message : "SelectSources call failed";
        dbus_error_free(&derr);
        Stop();
        return false;
    }
    // the reply is the (o) request handle - needed to CANCEL the request when
    // its dialog times out (a stale dialog blocks every later portal request,
    // including the Screenshot fallback; see CancelPortalRequest)
    std::string selectRequestPath;
    {
        DBusMessageIter it;
        dbus_message_iter_init(reply, &it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            const char* p = nullptr;
            dbus_message_iter_get_basic(&it, &p);
            if (p != nullptr) {
                selectRequestPath = p;
            }
        }
    }
    dbus_message_unref(reply);
    // wait for the SelectSources Response signal (matched by handle_token);
    // KDE shows the source-picker dialog here on first use - give the user
    // time to pick a screen (the previous 10s deadline expired before a
    // human could react to the dialog)
    {
        const std::string wantToken = token;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        bool ok = false;
        while (std::chrono::steady_clock::now() < deadline) {
            dbus_connection_read_write(m_impl->conn, 200);
            DBusMessage* msg = dbus_connection_pop_message(m_impl->conn);
            if (msg == nullptr) {
                continue;
            }
            const char* member = dbus_message_get_member(msg);
            const char* path = dbus_message_get_path(msg);
            if (member != nullptr && strcmp(member, "Response") == 0 &&
                path != nullptr && strstr(path, wantToken.c_str()) != nullptr) {
                DBusMessageIter it;
                dbus_message_iter_init(msg, &it);
                uint32_t status = 1;
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&it, &status);
                }
                ok = (status == 0);
                if (!ok) {
                    m_impl->error = "SelectSources failed (status " +
                                    std::to_string(status) + ")";
                    fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
                }
            }
            dbus_message_unref(msg);
            if (ok || !m_impl->error.empty()) {
                break;
            }
        }
        if (!ok) {
            if (m_impl->error.empty()) {
                m_impl->error = "SelectSources timed out";
            }
            fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
            // never leave the picker dialog pending on the portal: it blocks
            // every subsequent request (incl. the Screenshot fallback)
            CancelPortalRequest(m_impl->conn, selectRequestPath);
            Stop();
            return false;
        }
    }

    // 3. Start(session, parent_window="", options) -> the pipewire node id
    std::string nodeIdStr;
    token = "stardesk" + std::to_string(++m_impl->requestSeq);
    {
        DBusMessage* call = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast", "Start");
        DBusMessageIter args, dict, entry, var;
        dbus_message_iter_init_append(call, &args);
        const char* sess = m_impl->sessionHandle.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sess);
        const char* parentWindow = "";
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parentWindow);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    {
        const char* key = "handle_token";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
        const char* tokenCStr = token.c_str();
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &tokenCStr);
        dbus_message_iter_close_container(&entry, &var);
    }
    dbus_message_iter_close_container(&dict, &entry);
    dbus_message_iter_close_container(&args, &dict);

        DBusError derr;
        dbus_error_init(&derr);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            m_impl->conn, call, 180000, &derr);
        dbus_message_unref(call);
        if (reply == nullptr) {
            m_impl->error = derr.message ? derr.message : "Start call failed";
            dbus_error_free(&derr);
            Stop();
            return false;
        }
        // the reply is the (o) request handle - cancel it if Start's dialog
        // times out (the Start dialog on KDE is the screen picker and it also
        // blocks the portal for everything else)
        std::string startRequestPath;
        {
            DBusMessageIter it;
            dbus_message_iter_init(reply, &it);
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
                const char* p = nullptr;
                dbus_message_iter_get_basic(&it, &p);
                if (p != nullptr) {
                    startRequestPath = p;
                }
            }
        }
        dbus_message_unref(reply);

        // wait for the Start Response signal (matched by handle_token); the
        // KDE source-picker dialog can appear here too (when the persistent
        // choice was not yet made) - keep the wait long enough for a human
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (std::chrono::steady_clock::now() < deadline) {
            dbus_connection_read_write(m_impl->conn, 200);
            DBusMessage* msg = dbus_connection_pop_message(m_impl->conn);
            if (msg == nullptr) {
                continue;
            }
            const char* member = dbus_message_get_member(msg);
            const char* path = dbus_message_get_path(msg);
            if (member != nullptr && strcmp(member, "Response") == 0 &&
                path != nullptr && strstr(path, token.c_str()) != nullptr) {
                DBusMessageIter it;
                dbus_message_iter_init(msg, &it);
                uint32_t status = 1;
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&it, &status);
                    dbus_message_iter_next(&it);
                }
                if (status != 0) {
                    m_impl->error = "Start failed (status " + std::to_string(status) + ")";
                }
                else if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter dict3;
                    dbus_message_iter_recurse(&it, &dict3);
                    while (dbus_message_iter_get_arg_type(&dict3) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter entry3;
                        dbus_message_iter_recurse(&dict3, &entry3);
                        const char* key = nullptr;
                        if (dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&entry3, &key);
                            dbus_message_iter_next(&entry3);
                            // GNOME: remember the picker choice so the next
                            // SelectSources can restore it (no dialog again)
                            if (key != nullptr && strcmp(key, "restore_data") == 0 &&
                                dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_VARIANT) {
                                DBusMessageIter rv;
                                dbus_message_iter_recurse(&entry3, &rv);
                                if (dbus_message_iter_get_arg_type(&rv) == DBUS_TYPE_STRUCT) {
                                    SaveRestoreData(m_impl.get(), &rv);
                                }
                            }
                            else if (key != nullptr && strcmp(key, "streams") == 0 &&
                                dbus_message_iter_get_arg_type(&entry3) == DBUS_TYPE_VARIANT) {
                                DBusMessageIter var2;
                                dbus_message_iter_recurse(&entry3, &var2);
                                if (dbus_message_iter_get_arg_type(&var2) == DBUS_TYPE_ARRAY) {
                                    DBusMessageIter streams;
                                    dbus_message_iter_recurse(&var2, &streams);
                                    if (dbus_message_iter_get_arg_type(&streams) ==
                                        DBUS_TYPE_STRUCT) {
                                        DBusMessageIter s0;
                                        dbus_message_iter_recurse(&streams, &s0);
                                        // Per the portal spec the stream struct is
                                        // (uint32 node_id, a{sv} metadata): the node
                                        // id is the FIRST field, not a dict entry.
                                        if (dbus_message_iter_get_arg_type(&s0) ==
                                            DBUS_TYPE_UINT32) {
                                            uint32_t nid = 0;
                                            dbus_message_iter_get_basic(&s0, &nid);
                                            nodeIdStr = std::to_string(nid);
                                        }
                                        // Fallback: some backends embed node_id in
                                        // the metadata dict instead.
                                        if (nodeIdStr.empty() &&
                                            dbus_message_iter_next(&s0) &&
                                            dbus_message_iter_get_arg_type(&s0) ==
                                                DBUS_TYPE_ARRAY) {
                                            DBusMessageIter sdict;
                                            dbus_message_iter_recurse(&s0, &sdict);
                                            while (dbus_message_iter_get_arg_type(&sdict) ==
                                                   DBUS_TYPE_DICT_ENTRY) {
                                                DBusMessageIter sentry;
                                                dbus_message_iter_recurse(&sdict, &sentry);
                                                const char* skey = nullptr;
                                                if (dbus_message_iter_get_arg_type(&sentry) ==
                                                    DBUS_TYPE_STRING) {
                                                    dbus_message_iter_get_basic(&sentry, &skey);
                                                    dbus_message_iter_next(&sentry);
                                                    if (skey != nullptr &&
                                                        strcmp(skey, "node_id") == 0 &&
                                                        dbus_message_iter_get_arg_type(&sentry) ==
                                                            DBUS_TYPE_VARIANT) {
                                                        DBusMessageIter svar;
                                                        dbus_message_iter_recurse(&sentry, &svar);
                                                        if (dbus_message_iter_get_arg_type(&svar) ==
                                                            DBUS_TYPE_UINT32) {
                                                            uint32_t nid = 0;
                                                            dbus_message_iter_get_basic(&svar, &nid);
                                                            nodeIdStr = std::to_string(nid);
                                                        }
                                                    }
                                                }
                                                dbus_message_iter_next(&sdict);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        dbus_message_iter_next(&dict3);
                    }
                }
                dbus_message_unref(msg);
                break;
            }
            dbus_message_unref(msg);
        }
        if (nodeIdStr.empty() && m_impl->error.empty()) {
            m_impl->error = "Start timed out";
        }
        if (!m_impl->error.empty()) {
            m_impl->error = "Start: " + m_impl->error;
            fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
            // never leave the picker dialog pending on the portal: it blocks
            // every subsequent request (incl. the Screenshot fallback)
            CancelPortalRequest(m_impl->conn, startRequestPath);
            Stop();
            return false;
        }
    }
    m_impl->nodeId = (uint32_t)std::strtoul(nodeIdStr.c_str(), nullptr, 10);
    // SelectSources asked for cursor_mode=4 (metadata): the frames never
    // contain the pointer; spa_meta_cursor delivers the real position.
    m_impl->cursorEmbedded = false;
    fprintf(stderr, "[capture] ScreenCast node_id=%s embedded=0 (metadata)\n",
            nodeIdStr.c_str());
    return SetupPipewire();
}

// Attach the pipewire consumer stream to m_impl->nodeId: create the pw
// context/stream, EGL for the DMA-BUF readback, connect and wait for the
// first frame. Shared by the portal (StartScreenCast) and the mutter-direct
// (StartMetaCast) paths.
bool Capturer::SetupPipewire()
{
    // 4. PipeWire stream
    pw_init(nullptr, nullptr);
    m_impl->loop = pw_loop_new(nullptr);
    if (m_impl->loop == nullptr) {
        m_impl->error = "pw_loop_new failed";
        Stop();
        return false;
    }
    m_impl->ctx = pw_context_new(m_impl->loop, nullptr, 0);
    if (m_impl->ctx == nullptr) {
        m_impl->error = "pw_context_new failed";
        Stop();
        return false;
    }
    m_impl->core = pw_context_connect(m_impl->ctx, nullptr, 0);
    if (m_impl->core == nullptr) {
        m_impl->error = "pw_context_connect failed (is pipewire running?)";
        Stop();
        return false;
    }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr);
    m_impl->stream = pw_stream_new(m_impl->core, "stardesk-capture", props);
    if (m_impl->stream == nullptr) {
        m_impl->error = "pw_stream_new failed";
        Stop();
        return false;
    }
    pw_stream_add_listener(m_impl->stream, &m_impl->streamListener, &s_pwEvents, m_impl.get());

    // 5. EGL for the DMA-BUF readback
    m_impl->wlDisplay = wl_display_connect(nullptr);
    if (m_impl->wlDisplay != nullptr) {
        m_impl->eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT,
                                                   (void*)m_impl->wlDisplay, nullptr);
        if (m_impl->eglDisplay != EGL_NO_DISPLAY &&
            eglInitialize(m_impl->eglDisplay, nullptr, nullptr) == EGL_TRUE) {
            {
                const char* exts = eglQueryString(m_impl->eglDisplay, EGL_EXTENSIONS);
                fprintf(stderr, "[capture] EGL extensions: %s\n",
                        exts ? exts : "(none)");
                fflush(stderr);
            }
            eglBindAPI(EGL_OPENGL_ES_API);
            EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            m_impl->eglContext = eglCreateContext(m_impl->eglDisplay, EGL_NO_CONFIG_KHR,
                                                  EGL_NO_CONTEXT, ctxAttrs);
            if (m_impl->eglContext != EGL_NO_CONTEXT &&
                eglMakeCurrent(m_impl->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               m_impl->eglContext) == EGL_TRUE) {
                glGenTextures(1, &m_impl->eglTex);
                glGenFramebuffers(1, &m_impl->eglFbo);
                if (s_eglCreateImage == nullptr) {
                    s_eglCreateImage = (StardeskCreateImageProc)eglGetProcAddress(
                        "eglCreateImageKHR");
                }
                if (s_eglDestroyImage == nullptr) {
                    s_eglDestroyImage = (StardeskDestroyImageProc)eglGetProcAddress(
                        "eglDestroyImageKHR");
                }
                if (s_glEglImageTarget == nullptr) {
                    s_glEglImageTarget = (StardeskEglImageTargetProc)eglGetProcAddress(
                        "glEGLImageTargetTexture2DOES");
                }
                if (s_glBlitFramebuffer == nullptr) {
                    s_glBlitFramebuffer = (StardeskBlitProc)eglGetProcAddress(
                        "glBlitFramebuffer");
                }
                fprintf(stderr, "[capture] EGL blit=%s\n",
                        s_glBlitFramebuffer != nullptr ? "ok" : "MISSING");
                fflush(stderr);
                m_impl->eglOk = (s_eglCreateImage != nullptr && s_eglDestroyImage != nullptr &&
                                 s_glEglImageTarget != nullptr);
                if (m_impl->eglOk) {
                    // small downscale FBO (GLES3 blit target)
                    glGenTextures(1, &m_impl->smallTex);
                    glBindTexture(GL_TEXTURE_2D, m_impl->smallTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_impl->smallW,
                                 m_impl->smallH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glGenFramebuffers(1, &m_impl->smallFbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->smallFbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, m_impl->smallTex, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                // release context from the session thread so the capture
                // thread can make it current (EGL_BAD_ACCESS otherwise)
                eglMakeCurrent(m_impl->eglDisplay, EGL_NO_SURFACE,
                               EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }
    }

    // 6. connect the stream to the portal node.
    // Ask for CPU-readable MemFd buffers: mutter's DMA-BUF export on this
    // (intel) host uses tiled modifiers, which the EGL readback path returns
    // black or row-swizzled (the visible "shaking" picture) and mmap cannot
    // read at all. MemFd buffers are linear by definition and copy straight to
    // RGBA in the callback. The DMA-BUF paths stay as a fallback for
    // compositors that ignore the buffer request.
    uint8_t podBuf[2048];
    struct spa_pod_builder pb;
    spa_pod_builder_init(&pb, podBuf, sizeof(podBuf));
    const struct spa_pod* connectParams[2] = {nullptr, nullptr};
    {
        struct spa_pod_frame f;
        spa_pod_builder_push_object(&pb, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&pb,
                            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
                            0);
        connectParams[0] = (const struct spa_pod*)spa_pod_builder_pop(&pb, &f);
    }
    {
        struct spa_pod_frame f;
        spa_pod_builder_push_object(&pb, &f, SPA_TYPE_OBJECT_ParamBuffers,
                                    SPA_PARAM_Buffers);
        spa_pod_builder_add(&pb,
                            SPA_PARAM_BUFFERS_buffers,
                            SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
                            SPA_PARAM_BUFFERS_dataType,
                            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd),
                            0);
        connectParams[1] = (const struct spa_pod*)spa_pod_builder_pop(&pb, &f);
    }
    pw_stream_flags flags = (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                              PW_STREAM_FLAG_MAP_BUFFERS);
    int connectRes = -1;
    if (connectParams[0] != nullptr && connectParams[1] != nullptr) {
        connectRes = pw_stream_connect(m_impl->stream, PW_DIRECTION_INPUT, m_impl->nodeId,
                                       flags, connectParams, 2);
    }
    if (connectRes < 0) {
        // compositor rejected the MemFd request: connect unconstrained and use
        // the DMA-BUF paths (tiled buffers are dropped there, never read with
        // mmap)
        fprintf(stderr, "[capture] MemFd buffer request rejected, retrying unconstrained\n");
        fflush(stderr);
        m_impl->stream = pw_stream_new(m_impl->core, "stardesk-capture",
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                              PW_KEY_MEDIA_CATEGORY, "Capture",
                              PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (m_impl->stream == nullptr) {
            m_impl->error = "pw_stream_new failed";
            Stop();
            return false;
        }
        pw_stream_add_listener(m_impl->stream, &m_impl->streamListener,
                               &s_pwEvents, m_impl.get());
        if (pw_stream_connect(m_impl->stream, PW_DIRECTION_INPUT, m_impl->nodeId,
                              flags, nullptr, 0) < 0) {
            m_impl->error = "pw_stream_connect failed";
            Stop();
            return false;
        }
    }
    // reserve room for the cursor metadata (spa_meta_cursor + bitmap for a
    // 256x256 ARGB cursor); the pod is applied in PwOnParamChanged once the
    // stream format is negotiated (KWin accepts SPA_META_Cursor then)
    {
        struct spa_pod_builder b;
        spa_pod_builder_init(&b, m_impl->metaPod, sizeof(m_impl->metaPod));
        const struct spa_pod* pod = (const struct spa_pod*)spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_Int(
                (int)(sizeof(struct spa_meta_cursor) +
                      sizeof(struct spa_meta_bitmap) + 256 * 256 * 4)));
        m_impl->metaPodSize = (size_t)pod
            ? ((uint8_t*)pod - m_impl->metaPod) + pod->size
            : 0;
        if (m_impl->metaPodSize == 0) {
            fprintf(stderr, "[capture] meta pod build failed (no cursor data)\n");
        }
    }

    // NOTE: no waiting for the first frame here. The pw loop is NOT
    // thread-safe: SetupPipewire runs on the session thread while Capture()
    // runs on the capture thread - iterating the same loop from both would
    // deadlock on the internal spa lock. The first frame is delivered by the
    // first Capture() call instead.
    if (!m_impl->pwError.empty()) {
        m_impl->error = m_impl->pwError;
        fprintf(stderr, "[capture] %s\n", m_impl->error.c_str());
        Stop();
        return false;
    }
    fprintf(stderr, "[capture] ScreenCast started node=%u %dx%d egl=%d\n",
            m_impl->nodeId, m_impl->width, m_impl->height, m_impl->eglOk ? 1 : 0);

    m_impl->available = true;
    return true;
}

void Capturer::Stop()
{
    // stop the screenshot prefetch thread first (it may be blocked in a
    // portal call or waiting on the condition variable)
    {
        std::lock_guard<std::mutex> lock(m_impl->shotMutex);
        m_impl->shotStop = true;
    }
    m_impl->shotCv.notify_all();
    if (m_impl->shotThread.joinable()) {
        m_impl->shotThread.join();
    }
    if (m_impl->stream != nullptr) {
        pw_stream_disconnect(m_impl->stream);
        pw_stream_destroy(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->core != nullptr) {
        pw_core_disconnect(m_impl->core);
        m_impl->core = nullptr;
    }
    if (m_impl->ctx != nullptr) {
        pw_context_destroy(m_impl->ctx);
        m_impl->ctx = nullptr;
    }
    if (m_impl->loop != nullptr) {
        pw_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
    if (m_impl->eglContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_impl->eglDisplay, m_impl->eglContext);
        m_impl->eglContext = EGL_NO_CONTEXT;
    }
    if (m_impl->eglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(m_impl->eglDisplay);
        m_impl->eglDisplay = EGL_NO_DISPLAY;
    }
    if (m_impl->wlDisplay != nullptr) {
        wl_display_disconnect(m_impl->wlDisplay);
        m_impl->wlDisplay = nullptr;
    }
    if (m_impl->conn != nullptr) {
        dbus_connection_close(m_impl->conn);
        dbus_connection_unref(m_impl->conn);
        m_impl->conn = nullptr;
    }
    m_impl->started = false;
}

void Capturer::SetTargetResolution(int /*width*/, int /*height*/)
{
    // the portal streams at the output's native resolution; the host's
    // encode pipeline downscales to the peer's request afterwards
}

bool Capturer::Capture(CaptureFrame& out)
{
    if (m_impl->screenshotMode) {
        return ScreenshotCapture(out);
    }
    if (!m_impl->started || m_impl->stream == nullptr) {
        m_impl->error = "not started";
        return false;
    }
    // Emergency fallback: if fd count grows too fast (NVIDIA EGL fd leak),
    // disable EGL and use CPU mmap instead. Check every 128 frames.
    {
        static uint32_t sFrameCount = 0;
        if ((++sFrameCount & 0x7F) == 0 && m_impl->eglOk) {
            // count fds via /proc/self/fd (fast: just readdir)
            int fdCount = 0;
            DIR* d = ::opendir("/proc/self/fd");
            if (d != nullptr) {
                while (::readdir(d) != nullptr) ++fdCount;
                ::closedir(d);
                // fdCount includes the DIR* itself; > 800 open fds is
                // dangerously close to the default 1024 ulimit
                if (fdCount > 800) {
                    fprintf(stderr, "[capture] fd leak detected (%d open), switching to mmap\n",
                            fdCount);
                    fflush(stderr);
                    m_impl->eglOk = false; // permanent fallback for this session
                }
            }
            // RSS growth guard: a leaky EGL import grows VmRSS by ~5MB/frame.
            // 128 frames ≈ 2s at 60fps; > 256MB growth in that window is a
            // leak → fall back to mmap before the OOM killer fires.
            if (m_impl->eglOk) {
                static long s_lastRss = -1;
                long rss = -1;
                FILE* f = std::fopen("/proc/self/status", "r");
                if (f != nullptr) {
                    char line[128];
                    while (std::fgets(line, sizeof(line), f)) {
                        if (std::strncmp(line, "VmRSS:", 6) == 0) {
                            rss = std::atol(line + 6); // kB
                            break;
                        }
                    }
                    std::fclose(f);
                }
                if (s_lastRss > 0 && rss > 0 && rss - s_lastRss > 256 * 1024) {
                    fprintf(stderr, "[capture] RSS growth %ld→%ld kB (leak), switching to mmap\n",
                            s_lastRss, rss);
                    fflush(stderr);
                    m_impl->eglOk = false;
                }
                if (rss > 0) {
                    s_lastRss = rss;
                }
            }
        }
    }
    // pump the pipewire loop (frames arrive on the pw thread callbacks).
    // EGL context was created on the session thread but the capture loop
    // runs on a different thread; make the shared (no-surface) context
    // current here so that GL calls inside PwOnProcess work correctly.
    if (m_impl->eglOk && m_impl->eglDisplay != EGL_NO_DISPLAY &&
        m_impl->eglContext != EGL_NO_CONTEXT) {
        EGLBoolean r = eglMakeCurrent(m_impl->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       m_impl->eglContext);
        static int sMc = 0;
        if (++sMc <= 3) {
            FILE* df = std::fopen("/tmp/stardesk_diag.txt", "a");
            if (df) {
                fprintf(df, "[capture] eglMakeCurrent=%d err=0x%x\n",
                        r, eglGetError());
                fclose(df);
            }
        }
    }
    pw_loop_iterate(m_impl->loop, 0);
    // Drain Wayland events on the EGL connection. EGL may create internal
    // Wayland objects that trigger compositor replies (buffer-release, etc.).
    // Without dispatching, events accumulate in the receive buffer and the
    // file descriptors behind wl_buffer objects are never released, leading
    // to "Too many open files" and eventual OOM.
    if (m_impl->wlDisplay != nullptr) {
        wl_display_dispatch_pending(m_impl->wlDisplay);
        wl_display_flush(m_impl->wlDisplay);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->frameMutex);
        if (m_impl->haveFrame) {
            out = m_impl->latest;
            m_impl->haveFrame = false;
            return true;
        }
    }
    // No new buffer: the compositor stream is damage-driven, so this is normal
    // while the desktop is static. Report it once a second when it persists -
    // together with the pw-buffer counter it tells "static screen" apart from
    // "wedged stream" (buffer count no longer moves while the desktop changes).
    static auto sLastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - sLastReport >= std::chrono::seconds(10)) {
        sLastReport = now;
        const int64_t lastMs = m_impl->lastPwMs.load();
        const int64_t nowMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()).count();
        const int64_t ageMs = lastMs > 0 ? nowMs - lastMs : -1;
        // Normal for a damage-driven compositor stream (static desktop); only
        // report the long stalls, at a 10s interval.
        if (ageMs < 0 || ageMs > 5000) {
            fprintf(stderr, "[capture] no new frame (last pw buffer %lld ms ago, total %u)\n",
                    (long long)ageMs, (unsigned)m_impl->pwFrames.load());
            fflush(stderr);
        }
    }
    return false;
}

// Decode one portal screenshot file (file:// URI) into an RGBA frame.
// Extracted so both the prefetch thread and the sync path share it.
bool Capturer::DecodeScreenshotFile(const std::string& uri, CaptureFrame& out)
{
    std::string path = uri;
    if (path.rfind("file://", 0) == 0) {
        path = path.substr(7);
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    std::vector<uint8_t> data((size_t)sz);
    const size_t got = fread(data.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        return false;
    }
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&image, data.data(), data.size()) == 0) {
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    out.rgba.resize(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) == 0) {
        return false;
    }
    out.width = (int)image.width;
    out.height = (int)image.height;
    return true;
}

// Screenshot fallback: take the prefetched frame (or, on the very first
// call, capture one synchronously). Slow but guaranteed to work wherever
// the portal screenshot works.
bool Capturer::ScreenshotCapture(CaptureFrame& out)
{
    if (m_impl->conn == nullptr) {
        m_impl->error = "screenshot mode not started";
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(m_impl->shotMutex);
        if (m_impl->shotHaveLatest) {
            out = m_impl->shotLatest;
            m_impl->shotHaveLatest = false;
            m_impl->shotCv.notify_all(); // prefetch thread continues
            m_impl->width = out.width;
            m_impl->height = out.height;
            return true;
        }
    }
    // no prefetched frame yet (startup): capture synchronously
    std::string uri, err;
    const std::string token = "stardesk_shot" + std::to_string(++m_impl->shotSeq);
    if (!PortalScreenshot(m_impl->conn, token, uri, err)) {
        m_impl->error = err;
        return false;
    }
    if (!DecodeScreenshotFile(uri, out)) {
        m_impl->error = "decode failed: " + uri;
        return false;
    }
    m_impl->width = out.width;
    m_impl->height = out.height;
    return true;
}

int Capturer::GetWidth() const { return m_impl->width; }
int Capturer::GetHeight() const { return m_impl->height; }
int Capturer::GetOriginX() const { return m_impl->originX; }
int Capturer::GetOriginY() const { return m_impl->originY; }

bool Capturer::CursorEmbedded() const { return m_impl->cursorEmbedded; }

void Capturer::SetHideCursor(bool) {} // metadata mode: visibility is client-side

bool Capturer::GetCursor(int& x, int& y, bool& visible) const
{
    if (m_impl->cursorX < 0 || m_impl->cursorY < 0) {
        return false; // no cursor-meta update yet (falls back to the injector)
    }
    x = m_impl->cursorX;
    y = m_impl->cursorY;
    visible = true; // KWin positions arrive only for a visible pointer
    return true;
}

bool Capturer::IsAvailable() const { return m_impl->available; }
std::string Capturer::GetError() const { return m_impl->error; }

} // namespace sdk
