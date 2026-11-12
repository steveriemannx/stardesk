#include "RemoteWindow.h"

#include "../app/Language.h"
#include "../app/Theme.h"
#include "../platform/InputInject.h"
#include "FileBrowserWindow.h"
#include "FileSendWindow.h"

#include <algorithm>

namespace sdk {

using ui::UiRect;
using ui::UiRectF;

namespace {

// 16x16 arrow cursor (1 = black, 2 = white outline, 0 = transparent)
const uint8_t kCursorMask[16][16] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,2,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,2,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,2,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,2,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,2,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,2,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,2,0,0,0,0,0},
    {1,1,1,2,1,1,1,1,1,1,1,2,0,0,0,0},
    {1,1,2,0,1,1,1,1,1,1,2,0,2,0,0,0},
    {1,2,0,0,1,1,1,1,1,2,0,0,2,2,0,0},
    {1,0,0,0,2,1,1,1,2,0,0,0,0,2,0,0},
    {0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0},
};

void DrawCursor(uint8_t* rgba, int w, int h, int cx, int cy, int scale = 1)
{
    if (cx < 0 || cy < 0) {
        return;
    }
    // 16x16 arrow, roughly the size of the local (macOS) cursor; the frame is
    // displayed scaled up so it reads slightly larger on screen
    const int cursorPx = 16 * scale;
    for (int sy = 0; sy < cursorPx; ++sy) {
        for (int sx = 0; sx < cursorPx; ++sx) {
            const int px = cx + sx;
            const int py = cy + sy;
            if (px < 0 || px >= w || py < 0 || py >= h) {
                continue;
            }
            const uint8_t m = kCursorMask[sy / scale][sx / scale];
            if (m == 0) {
                continue;
            }
            uint8_t* p = &rgba[(size_t)(py * w + px) * 4];
            if (m == 1) {
                p[0] = 0;
                p[1] = 0;
                p[2] = 0;
                p[3] = 255;
            }
            else {
                p[0] = 255;
                p[1] = 255;
                p[2] = 255;
                p[3] = 255;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------- ctor

RemoteWindow::RemoteWindow(std::unique_ptr<ClientSession> session)
    : m_session(std::move(session))
{
}

RemoteWindow::~RemoteWindow()
{
    SetLocalCursorHidden(false);
    App::Instance().RemoveListener(this);
    delete m_cursorBitmap;
    m_cursorBitmap = nullptr;
    if (m_session) {
        m_session->Stop();
    }
}

std::string RemoteWindow::GetSkinFolder()
{
    return "";
}

std::string RemoteWindow::GetSkinFile()
{
    return "";
}

void RemoteWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 1000;
    attrs.m_szInitSize.cy = 680;
    // system shadow: non-layered window + shadow attached, like the XML
    // configuration layered_window="false" shadow_attached="true" (two shadow
    // implementations exist: layered -> transparent self-drawn shadow,
    // normal -> shadow drawn on the window surface)
    attrs.m_bShadowAttached = true;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = false;
    attrs.m_bIsLayeredWindowDefined = true;
    attrs.m_rcSizeBox = UiRect(4, 4, 4, 4);
    attrs.m_bSizeBoxDefined = true;
    attrs.m_rcCaption = UiRect(0, 0, 0, 36);
    attrs.m_bCaptionDefined = true;
    // shadow_type="system_round": the OS provides the shadow, so there is no
    // self-drawn shadow border (corner is 0 -> window size stays as declared)
    ui::Shadow::ShadowType nShadowType = ui::Shadow::ShadowType::kShadowSystemRound;
    ui::UiSize szBorderRound;
    ui::UiPadding rcShadowCorner;
    std::string shadowImage;
    ui::Shadow::GetShadowParam(nShadowType, szBorderRound, rcShadowCorner, shadowImage);
    attrs.m_rcShadowCorner = rcShadowCorner;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void RemoteWindow::OnInitWindow()
{
    // pure-code windows must apply the caption/size-box rects themselves
    // (XML mode does this in WindowBuilder; attrs.m_rcCaption is not applied)
    SetSizeBox(ui::UiRect(4, 4, 4, 4), true);
    SetCaptionRect(ui::UiRect(0, 0, 0, 36), true);
    // system shadow (like shadow_type="system_round" in XML)
    SetShadowType(ui::Shadow::ShadowType::kShadowSystemRound);

    const Palette& p = Theme::Get();
    const bool viewOnly = m_session && m_session->IsViewMode();
    fprintf(stderr, "[client] RemoteWindow init: session=%p viewOnly=%d\n",
            (void*)m_session.get(), viewOnly ? 1 : 0);
    // initial codec matches the Hello preference (STARDESK_CODEC or the
    // macOS default H.264)
    if (const char* c = std::getenv("STARDESK_CODEC")) {
        const int v = std::atoi(c);
        if (v >= 1 && v <= 4) {
            m_currentCodec = v;
        }
    }

    ui::VBox* root = new ui::VBox(this);
    root->SetBkColor(p.windowBg);
    root->SetAttribute("width", "stretch");
    root->SetAttribute("height", "stretch");

    // ---- caption ----
    ui::HBox* caption = new ui::HBox(this);
    // macOS: name it as the dui caption bar so WindowImplBase adds the
    // self-drawn traffic lights (close/minimize/zoom) on this window too.
    caption->SetAttribute("name", "window_caption_bar");
    caption->SetAttribute("height", "36");
    caption->SetBkColor(p.captionBg);
    root->AddItem(caption);

    m_titleLabel = new ThemeLabel(this);
    m_titleLabel->SetRole(ThemeLabel::Role::Main);
    m_titleLabel->SetText(m_session ? ui::StringConvert::UTF8ToT(m_session->RemoteName()) : "");
    m_titleLabel->SetAttribute("font", "system_bold_12");
    m_titleLabel->SetAttribute("width", "auto");
    m_titleLabel->SetAttribute("margin", "12,0,0,0");
    caption->AddItem(m_titleLabel);

    if (viewOnly) {
        m_badgeLabel = new ThemeLabel(this);
        m_badgeLabel->SetRole(ThemeLabel::Role::Accent);
        m_badgeLabel->SetText(SDK_TR("remote.viewOnly"));
        m_badgeLabel->SetAttribute("width", "auto");
        m_badgeLabel->SetAttribute("margin", "10,0,0,0");
        caption->AddItem(m_badgeLabel);
    }

    ui::Control* spacer = new ui::Control(this);
    spacer->SetMouseEnabled(false);
    caption->AddItem(spacer);

    IconButton* closeBtn = new IconButton(this);
    closeBtn->SetIcon(VectorArt::Icon::Close);
    m_lang.BindToolTip(closeBtn, "window.close");
    closeBtn->SetName("closebtn");
    closeBtn->SetAttribute("width", "34");
    closeBtn->SetAttribute("height", "30");
    closeBtn->SetAttribute("margin", "0,3,4,3");
    caption->AddItem(closeBtn);

    // ---- screen ----
    m_bitmap = new ui::BitmapControl(this);
    m_bitmap->SetAttribute("name", "remote_bitmap");
    m_bitmap->SetAttribute("width", "stretch");
    m_bitmap->SetAttribute("height", "stretch");
    m_bitmap->SetBkColor(p.windowBg);
    m_bitmap->SetSupportMultiThread(false);
    m_bitmap->SetMouseEnabled(!viewOnly);
    root->AddItem(m_bitmap);
    // centered in all modes; default is FILL (stretch to the window - both
    // screens here are 16:10, so the stretch is invisible). The 适配 pill
    // toggles to FIT (proportional, centered) for exact aspect.
    ApplyDisplayMode();

    // ---- toolbar ----
    m_toolbar = new ui::VBox(this);
    m_toolbar->SetAttribute("height", "46");
    m_toolbar->SetBkColor(p.panelBg);
    root->AddItem(m_toolbar);

    ui::HBox* toolRow = new ui::HBox(this);
    toolRow->SetAttribute("child_margin_x", "10");
    toolRow->SetAttribute("padding", "12,8,12,8");
    m_toolbar->AddItem(toolRow);

    m_fitPill = new PillButton(this);
    m_fitPill->SetText(SDK_TR("remote.fit"));
    m_fitPill->SetAttribute("width", "86");
    m_fitPill->SetAttribute("height", "30");
    m_fitPill->SetSelected(m_fitMode); // fill mode by default
    m_fitPill->AttachClick([this](const ui::EventArgs&) {
        ToggleFit();
        return true;
    });
    toolRow->AddItem(m_fitPill);

    m_fullscreenPill = new PillButton(this);
    m_fullscreenPill->SetText(SDK_TR("remote.fullscreen"));
    m_fullscreenPill->SetAttribute("width", "86");
    m_fullscreenPill->SetAttribute("height", "30");
    m_fullscreenPill->AttachClick([this](const ui::EventArgs&) {
        ToggleFullscreen();
        return true;
    });
    toolRow->AddItem(m_fullscreenPill);

    m_cursorMode = App::Instance().Config().cursorMode;
    SetLocalCursorHidden(false);
    m_cursorPill = new PillButton(this);
    UpdateCursorPillText();
    m_cursorPill->SetAttribute("width", "104");
    m_cursorPill->SetAttribute("height", "30");
    m_cursorPill->SetSelected(m_cursorMode != 0);
    m_cursorPill->AttachClick([this](const ui::EventArgs&) {
        CycleCursorMode();
        return true;
    });
    toolRow->AddItem(m_cursorPill);

    // live codec switch (H.264 / VP9 / AV1)
    ui::Control* codecSep = new ui::Control(this);
    codecSep->SetAttribute("width", "6");
    codecSep->SetMouseEnabled(false);
    toolRow->AddItem(codecSep);

    m_h264Pill = new PillButton(this);
    m_h264Pill->SetText("H.264");
    m_h264Pill->SetAttribute("width", "56");
    m_h264Pill->SetAttribute("height", "30");
    m_h264Pill->SetSelected(m_currentCodec == 1);
    m_h264Pill->AttachClick([this](const ui::EventArgs&) {
        SwitchCodec(1);
        return true;
    });
    toolRow->AddItem(m_h264Pill);

    m_vp9Pill = new PillButton(this);
    m_vp9Pill->SetText("VP9");
    m_vp9Pill->SetAttribute("width", "56");
    m_vp9Pill->SetAttribute("height", "30");
    m_vp9Pill->SetSelected(m_currentCodec == 2);
    m_vp9Pill->AttachClick([this](const ui::EventArgs&) {
        SwitchCodec(2);
        return true;
    });
    toolRow->AddItem(m_vp9Pill);

    m_av1Pill = new PillButton(this);
    m_av1Pill->SetText("AV1");
    m_av1Pill->SetAttribute("width", "56");
    m_av1Pill->SetAttribute("height", "30");
    m_av1Pill->SetSelected(m_currentCodec == 3);
    m_av1Pill->AttachClick([this](const ui::EventArgs&) {
        SwitchCodec(3);
        return true;
    });
    toolRow->AddItem(m_av1Pill);

    m_openh264Pill = new PillButton(this);
    m_openh264Pill->SetText("OpenH.264");
    m_openh264Pill->SetAttribute("width", "80");
    m_openh264Pill->SetAttribute("height", "30");
    m_openh264Pill->SetSelected(m_currentCodec == 4);
    m_openh264Pill->AttachClick([this](const ui::EventArgs&) {
        SwitchCodec(4);
        return true;
    });
    toolRow->AddItem(m_openh264Pill);

    m_sendFileBtn = new PillButton(this);
    m_sendFileBtn->SetText(SDK_TR("remote.sendFile"));
    m_sendFileBtn->SetAttribute("width", "auto");
    m_sendFileBtn->SetAttribute("height", "30");
    m_sendFileBtn->SetAttribute("padding", "12,0,12,0");
    m_sendFileBtn->AttachClick([this](const ui::EventArgs&) {
        if (m_session && m_session->IsConnected()) {
            FileBrowserWindow* w = new FileBrowserWindow(m_session.get());
            w->CreateWnd(this, ui::WindowCreateParam("StarDesk", true));
            w->ShowWindow(ui::kSW_SHOW_NORMAL);
        }
        return true;
    });
    toolRow->AddItem(m_sendFileBtn);

    ui::Control* spacer2 = new ui::Control(this);
    spacer2->SetMouseEnabled(false);
    toolRow->AddItem(spacer2);

    m_statusLabel = new ThemeLabel(this);
    m_statusLabel->SetRole(ThemeLabel::Role::Sub);
    m_statusLabel->SetText(SDK_TR("remote.waitingStream"));
    m_statusLabel->SetAttribute("width", "auto");
    m_statusLabel->SetAttribute("margin", "0,0,4,0");
    toolRow->AddItem(m_statusLabel);

    m_disconnectBtn = new IconButton(this);
    m_disconnectBtn->SetIcon(VectorArt::Icon::Power);
    m_disconnectBtn->SetIconToolTip(SDK_TR("remote.disconnect"));
    m_disconnectBtn->SetAttribute("width", "30");
    m_disconnectBtn->SetAttribute("height", "30");
    m_disconnectBtn->AttachClick([this](const ui::EventArgs&) {
        Disconnect();
        return true;
    });
    toolRow->AddItem(m_disconnectBtn);

    AttachBox(root);
    App::Instance().AddListener(this);

    // ---- wire the session ----
    if (m_session) {
        ClientSession::Callbacks cb;
        cb.onFrame = [this]() {
            // latest-wins: fetch the newest complete frame when the UI runs
            PostToUI(ui::UiBind(this, [this]() {
                int w = 0, h = 0, cxn = -1, cyn = -1;
                std::vector<uint8_t> rgba;
                if (m_session && m_session->TakeLatestFrame(w, h, rgba, cxn, cyn)) {
                    OnFrameReady(w, h, rgba, cxn, cyn);
                }
                else {
                    // no new frame: keep the remote-cursor overlay tracking
                    // the local mouse (redraw the cached frame at the current
                    // cursor position) - this removes the "one frame behind"
                    // feel between streamed frames
                    RefreshCursorOverlay();
                }
            }));
        };
        cb.onCursor = [this](int cursorXN, int cursorYN) {
            PostToUI(ui::UiBind(this, [this, cursorXN, cursorYN]() {
                m_cursorXN = cursorXN;
                m_cursorYN = cursorYN;
                Invalidate(UiRect());
            }));
        };
        cb.onStats = [this](int renderFps, int rxFps, int latencyMs) {
            PostToUI(ui::UiBind(this, [this, renderFps, rxFps, latencyMs]() {
                OnStats(renderFps, rxFps, latencyMs);
            }));
        };
        m_session->SetFrameCallbacks(std::move(cb));
    }

    BaseClass::OnInitWindow();

    // macOS: WindowImplBase inserts the self-drawn traffic lights when the
    // caption bar control is present; log it so the self-drawn caption is
    // verifiable from the client log.
    if (FindControl("mac_traffic_lights") != nullptr) {
        fprintf(stderr, "[client] mac traffic lights: present\n");
    }
    else {
        fprintf(stderr, "[client] mac traffic lights: NOT added\n");
    }
}

// ---------------------------------------------------------------- frame display

void RemoteWindow::OnFrameReady(int w, int h, const std::vector<uint8_t>& rgba,
                                int cursorXN, int cursorYN)
{
    // keep the session's pre-scale target in sync with the bitmap dest
    m_cacheW = w;
    m_cacheH = h;
    UpdateDisplaySize();

    // test hook: STARDESK_TEST_POINTER_CLIENT=x,y sends a move plus click
    // through the REAL client->host mapping at these client-window pixels.
    // The host log then shows the injected target, so the mapping can be
    // verified end to end without GUI input.
    if (const char* tpc = std::getenv("STARDESK_TEST_POINTER_CLIENT")) {
        static bool sSentClient = false;
        if (!sSentClient && m_session != nullptr && m_session->IsConnected() && w > 0) {
            sSentClient = true;
            int px = 0, py = 0;
            std::sscanf(tpc, "%d,%d", &px, &py);
            ui::UiPoint pt;
            pt.x = px;
            pt.y = py;
            const UiRect dest = ComputeBitmapDest();
            double nx = 0.0, ny = 0.0;
            MapToRemote(pt, nx, ny);
            fprintf(stderr,
                    "[client] test pointer client=(%d,%d) dest=(%d,%d,%d,%d) n=(%.3f,%.3f)\n",
                    px, py, dest.left, dest.top, dest.right, dest.bottom, nx, ny);
            SendMouse(0, pt, 0);
            SendButtonMsg(true, 0, pt);
            SendButtonMsg(false, 0, pt);
        }
    }

    // test hook: STARDESK_TEST_POINTERS=nx,ny sends one pointer move plus an
    // absolute click at that normalized position once the first frame arrives.
    // Used to verify the client->host absolute-pointer mapping without GUI
    // interaction (the host logs the injected target and its cursor source).
    if (const char* tp = std::getenv("STARDESK_TEST_POINTERS")) {
        static bool sSent = false;
        if (!sSent && m_session != nullptr && m_session->IsConnected() && w > 0) {
            sSent = true;
            double nx = 0.5, ny = 0.5;
            std::sscanf(tp, "%lf,%lf", &nx, &ny);
            InputEventMsg ev;
            ev.kind = 0;
            ev.xNorm = (uint16_t)(nx * 65535.0);
            ev.yNorm = (uint16_t)(ny * 65535.0);
            m_session->SendInput(ev);
            InputEventMsg click = ev;
            click.kind = 1;
            click.buttons = 0x11; // bit4 = first press, bit0 = left button
            click.flags = 1;
            m_session->SendInput(click);
            fprintf(stderr, "[client] test pointer sent n=(%.3f,%.3f)\n", nx, ny);
        }
    }

    // The Wayland ScreenCast stream includes the compositor's actual cursor.
    // Keep the frame untouched so the remote desktop is mirrored faithfully.
    std::vector<uint8_t> frame = rgba;
    if (m_bitmap != nullptr && !frame.empty()) {
        ApplyDisplayMode();
        m_bitmap->SetBitmapData(w, h, frame.data(), (int)frame.size());
    }
    if (m_statusLabel != nullptr) {
        UpdateStatus();
    }
}

ui::UiPoint RemoteWindow::GetCursorClientPixels() const
{
    ui::UiPoint pt;
    GetCursorPos(pt);
    ScreenToClient(pt);
#if defined(DUI_BUILD_FOR_MACOS)
    // AppKit cursor coordinates are window POINTS; dui controls live in client
    // PIXELS (2x on Retina), so scale them to match ComputeBitmapDest().
    Dpi().ScalePoint(pt);
#endif
    return pt;
}

void RemoteWindow::RefreshCursorOverlay()
{
    if (m_bitmap == nullptr || m_cursorMode == 0 || !m_inputEngaged) {
        return;
    }
    if (m_session == nullptr || m_session->IsViewMode() || m_cursorMode == 2) {
        return;
    }
    const ui::UiPoint cursorPt = GetCursorClientPixels();
    if (cursorPt == m_lastOverlayPt) {
        return; // cursor unchanged: nothing to repaint
    }
    m_lastOverlayPt = cursorPt;
    // cheap: repaint only, the cursor layer is drawn at the current position
    Invalidate(UiRect());
}

void RemoteWindow::EnsureCursorBitmap()
{
    if (m_cursorBitmap != nullptr) {
        return;
    }
    ui::IRenderFactory* factory = ui::GlobalManager::Instance().GetRenderFactory();
    if (factory == nullptr) {
        return;
    }
    m_cursorBitmap = factory->CreateBitmap();
    if (m_cursorBitmap == nullptr) {
        return;
    }
    uint8_t rgba[16 * 16 * 4];
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint8_t m = kCursorMask[y][x];
            const size_t o = (size_t)(y * 16 + x) * 4;
            if (m == 1) { // red fill, distinct from the native local cursor
                rgba[o] = 230; rgba[o + 1] = 32; rgba[o + 2] = 32; rgba[o + 3] = 255;
            }
            else if (m == 2) { // white outline
                rgba[o] = 255; rgba[o + 1] = 255; rgba[o + 2] = 255; rgba[o + 3] = 255;
            }
            else { // transparent
                rgba[o] = 0; rgba[o + 1] = 0; rgba[o + 2] = 0; rgba[o + 3] = 0;
            }
        }
    }
    m_cursorBitmap->Init(16, 16, rgba);
}

LRESULT RemoteWindow::OnPaintMsg(const UiRect& rcPaint, const ui::NativeMsg& nativeMsg,
                                 bool& bHandled)
{
    LRESULT r = BaseClass::OnPaintMsg(rcPaint, nativeMsg, bHandled);
    // Remote-cursor overlay:
    //   dual mode (1): the remote follows the local input, so draw the
    //     remote indicator AT the local pointer position - zero latency, the
    //     two cursors are always visually together;
    //   display mode (2): watching only (no input forwarded) - draw at the
    //     REAL remote position reported by the host (CursorPos).
    // Hosts whose capture embeds the pointer (Wayland ScreenCast embedded)
    // are skipped - the cursor is in the frame and CursorPos tracks the
    // injection-only pointer there (stale).
    if (m_cursorMode == 1 && m_session != nullptr && !m_session->CursorInStream() &&
        m_bitmap != nullptr) {
        // dual mode: track the LOCAL cursor (instant - overlaps the native
        // pointer, no network-latency gap between the two cursors)
        const ui::UiPoint cursorPt = GetCursorClientPixels();
        const UiRect dest = ComputeBitmapDest();
        ui::IRender* render = GetRender();
        if (render != nullptr && dest.Width() > 0 && dest.Height() > 0 &&
            cursorPt.x >= dest.left && cursorPt.y >= dest.top &&
            cursorPt.x <= dest.left + dest.Width() &&
            cursorPt.y <= dest.top + dest.Height()) {
            EnsureCursorBitmap();
            if (m_cursorBitmap != nullptr) {
                const int curW = Dpi().GetScaleInt(16);
                UiRect rcDest(cursorPt.x, cursorPt.y, cursorPt.x + curW, cursorPt.y + curW);
                UiRect rcSrc(0, 0, 16, 16);
                render->DrawImage(rcPaint, m_cursorBitmap, rcDest, rcSrc, 255);
            }
        }
    }
    else if (m_cursorMode == 2 && m_session != nullptr && !m_session->CursorInStream() &&
             m_session->CursorVisible() && m_cursorXN >= 0 && m_cursorYN >= 0 &&
             m_bitmap != nullptr) {
        const UiRect dest = ComputeBitmapDest();
        ui::IRender* render = GetRender();
        if (render != nullptr && dest.Width() > 0 && dest.Height() > 0) {
            const int cx = dest.left + (int)((int64_t)m_cursorXN * dest.Width() / 65535);
            const int cy = dest.top + (int)((int64_t)m_cursorYN * dest.Height() / 65535);
            EnsureCursorBitmap();
            if (m_cursorBitmap != nullptr) {
                const int curW = Dpi().GetScaleInt(16);
                UiRect rcDest(cx, cy, cx + curW, cy + curW);
                UiRect rcSrc(0, 0, 16, 16);
                render->DrawImage(rcPaint, m_cursorBitmap, rcDest, rcSrc, 255);
            }
        }
    }
    return r;
}

void RemoteWindow::UpdateDisplaySize()
{
    if (m_session == nullptr || m_bitmap == nullptr) {
        return;
    }
    const UiRect dest = ComputeBitmapDest();
    if (dest.Width() > 0 && dest.Height() > 0) {
        m_session->SetDisplaySize(dest.Width(), dest.Height());
    }
}

LRESULT RemoteWindow::OnSizeMsg(ui::WindowSizeType sizeType, const ui::UiSize& newWindowSize,
                                const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    UpdateDisplaySize();
    return BaseClass::OnSizeMsg(sizeType, newWindowSize, nativeMsg, bHandled);
}

void RemoteWindow::CycleCursorMode()
{
    m_cursorMode = (m_cursorMode + 1) % 3;
    // Display mode hides the native cursor only while it is over the remote
    // image; keep it visible over the toolbar so the local controls remain
    // usable and the pointer does not appear to disappear from the window.
    SetLocalCursorHidden(false);
    if (m_cursorPill != nullptr) {
        UpdateCursorPillText();
        m_cursorPill->SetSelected(m_cursorMode != 0);
    }
    AppConfig& cfg = App::Instance().Config();
    cfg.cursorMode = m_cursorMode;
    cfg.Save();
    // local-cursor mode asks the host to hide the real pointer from the
    // stream (Wayland embedded hosts re-capture with cursor_mode=1); the
    // other modes leave it embedded so the remote cursor is visible.
    if (m_session != nullptr) {
        m_session->SendCursorMode(m_cursorMode == 0);
    }
}

void RemoteWindow::SetLocalCursorHidden(bool hidden)
{
    if (m_localCursorHidden == hidden) {
        return;
    }
    ui::GlobalManager::Instance().Cursor().ShowCursor(!hidden);
    m_localCursorHidden = hidden;
}

void RemoteWindow::UpdateDisplayModeCursor(const ui::UiPoint& pt)
{
    if (m_cursorMode != 2) {
        SetLocalCursorHidden(false);
        return;
    }
    const UiRect dest = ComputeBitmapDest();
    const bool overRemote = pt.x >= dest.left && pt.y >= dest.top &&
                            pt.x <= dest.right && pt.y <= dest.bottom;
    SetLocalCursorHidden(overRemote);
}

void RemoteWindow::OnStats(int renderFps, int rxFps, int latencyMs)
{
    m_lastFps = renderFps;
    m_lastRxFps = rxFps;
    m_lastLatency = latencyMs;
    m_hasStats = true;
    UpdateStatus();
}

void RemoteWindow::UpdateStatus()
{
    if (m_statusLabel == nullptr) {
        return;
    }
    std::string text;
    if (m_hasStats) {
        // render rate (local refresh) | remote received rate | real latency
        text = ui::StringUtil::Printf("%s %d | %s %d | %s %d ms",
                                      SDK_TR("remote.fpsLabel").c_str(), m_lastFps,
                                      SDK_TR("remote.rxFpsLabel").c_str(), m_lastRxFps,
                                      SDK_TR("remote.latencyLabel").c_str(), m_lastLatency);
    }
    else {
        text = SDK_TR("remote.waitingStream");
    }
    m_statusLabel->SetText(text);
}

// ---------------------------------------------------------------- remote input

bool RemoteWindow::MapToRemote(const ui::UiPoint& pt, double& nx, double& ny)
{
    if (m_bitmap == nullptr || m_session == nullptr || !m_session->IsConnected()) {
        return false;
    }
    // ComputeBitmapDest() derives from m_bitmap->GetRect(), so `dest` is
    // already in the same (client) coordinates as `pt` - do NOT add
    // ctrl.left/top again (that double-counted the 72px caption and shifted
    // the cursor overlay / click mapping down by that amount)
    const UiRect dest = ComputeBitmapDest();
    if (dest.Width() <= 0 || dest.Height() <= 0) {
        return false;
    }
    nx = (double)(pt.x - dest.left) / dest.Width();
    ny = (double)(pt.y - dest.top) / dest.Height();
    return nx >= 0.0 && nx <= 1.0 && ny >= 0.0 && ny <= 1.0;
}

ui::UiRect RemoteWindow::ComputeBitmapDest() const
{
    // control area: the bitmap's rect minus its padding (no image margin set)
    UiRect rc = m_bitmap->GetRect();
    rc.Deflate(m_bitmap->GetControlPadding());
    const int fw = m_cacheW;
    const int fh = m_cacheH;
    if (fw <= 0 || fh <= 0 || rc.Width() <= 0 || rc.Height() <= 0) {
        return UiRect();
    }
    // The dest rect MUST match how BitmapControl paints the frame, or the
    // cursor overlay / input mapping drift from the visible image.
    //   fill mode (default): stretched over the whole control area
    //   fit mode: aspect-preserving, CENTERED (BitmapControl is configured
    //   with kAlignCenter in ApplyDisplayMode)
    if (!m_fitMode) {
        return rc;
    }
    const double scale = std::min((double)rc.Width() / fw, (double)rc.Height() / fh);
    const int dw = std::max(1, (int)(fw * scale));
    const int dh = std::max(1, (int)(fh * scale));
    UiRect dest;
    dest.left = rc.left + (rc.Width() - dw) / 2;  // centered, matching paint
    dest.top = rc.top + (rc.Height() - dh) / 2;
    dest.right = dest.left + dw;
    dest.bottom = dest.top + dh;
    return dest;
}

void RemoteWindow::SendMouse(int kind, const ui::UiPoint& pt, uint8_t buttons)
{
    if (m_session == nullptr || m_session->IsViewMode() || m_cursorMode == 2) {
        return;
    }
    double nx = 0.0, ny = 0.0;
    if (!MapToRemote(pt, nx, ny)) {
        return;
    }
    InputEventMsg ev;
    ev.kind = (uint8_t)kind;
    ev.xNorm = (uint16_t)(nx * 65535.0);
    ev.yNorm = (uint16_t)(ny * 65535.0);
    ev.buttons = buttons;
    m_session->SendInput(ev);
}

void RemoteWindow::SendButtonMsg(bool down, int button, const ui::UiPoint& pt)
{
    if (m_session == nullptr || m_session->IsViewMode() || m_cursorMode == 2) {
        return;
    }
    double nx = 0.0, ny = 0.0;
    if (!MapToRemote(pt, nx, ny)) {
        return;
    }
    InputEventMsg ev;
    ev.kind = 1; // button
    ev.xNorm = (uint16_t)(nx * 65535.0);
    ev.yNorm = (uint16_t)(ny * 65535.0);
    ev.buttons = (uint8_t)((1u << button) | (down ? 0x10u : 0u));
    m_session->SendInput(ev);
}

void RemoteWindow::SendKey(uint16_t vkCode, uint32_t modifierKey, bool down)
{
    if (m_session == nullptr || m_session->IsViewMode() || m_cursorMode == 2) {
        return;
    }
    InputEventMsg ev;
    ev.kind = down ? 3 : 4;
    ev.vkCode = vkCode;
    ev.modifiers = (uint8_t)(modifierKey & 0x0F);
    m_session->SendInput(ev);
}

LRESULT RemoteWindow::OnKeyDownMsg(ui::VirtualKeyCode vkCode, uint32_t modifierKey,
                                   const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    if (vkCode == ui::kVK_ESCAPE && m_fullscreen) {
        // let dui handle ESC to exit fullscreen
        bHandled = false;
        return BaseClass::OnKeyDownMsg(vkCode, modifierKey, nativeMsg, bHandled);
    }
    SendKey((uint16_t)vkCode, modifierKey, true);
    bHandled = true;
    return 0;
}

LRESULT RemoteWindow::OnKeyUpMsg(ui::VirtualKeyCode vkCode, uint32_t modifierKey,
                                 const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    SendKey((uint16_t)vkCode, modifierKey, false);
    bHandled = true;
    return 0;
}

LRESULT RemoteWindow::OnMouseMoveMsg(const ui::UiPoint& pt, uint32_t modifierKey, bool bFromNC,
                                     const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    static uint32_t sMoveCount = 0;
    if ((++sMoveCount & 0x7F) == 1) {
        fprintf(stderr, "[client] OnMouseMoveMsg #%u engaged=%d\n",
                sMoveCount, m_inputEngaged ? 1 : 0);
    }
    m_lastMousePt = pt; // the cursor overlay (control mode) tracks this point
    UpdateDisplayModeCursor(pt);
    // Dual-cursor mode is control mode: forward the move immediately so the
    // remote cursor follows the local one (the remote stays at its own
    // position until the user has engaged otherwise, which reads as a wrong
    // position). Local mode keeps the click-to-engage rule so the remote can
    // be observed independently before the first click; display mode never
    // forwards.
    if (m_inputEngaged || m_cursorMode == 1) {
        SendMouse(0, pt, 0);
    }
    return BaseClass::OnMouseMoveMsg(pt, modifierKey, bFromNC, nativeMsg, bHandled);
}

LRESULT RemoteWindow::OnMouseWheelMsg(int32_t wheelDelta, const ui::UiPoint& pt,
                                      uint32_t modifierKey,
                                      const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    if (m_session && !m_session->IsViewMode() && m_cursorMode != 2) {
        double nx = 0.0, ny = 0.0;
        if (MapToRemote(pt, nx, ny)) {
            InputEventMsg ev;
            ev.kind = 2;
            ev.xNorm = (uint16_t)(nx * 65535.0);
            ev.yNorm = (uint16_t)(ny * 65535.0);
            ev.wheelDelta = (int16_t)(wheelDelta / 120);
            m_session->SendInput(ev);
        }
    }
    return BaseClass::OnMouseWheelMsg(wheelDelta, pt, modifierKey, nativeMsg, bHandled);
}

LRESULT RemoteWindow::OnMouseLButtonDownMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                            const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    // Display mode: swallow only clicks over the remote image; let toolbar
    // clicks through so the local controls stay usable.
    if (m_cursorMode == 2) {
        const UiRect dest = ComputeBitmapDest();
        const bool overRemote = pt.x >= dest.left && pt.y >= dest.top &&
                                pt.x <= dest.right && pt.y <= dest.bottom;
        if (overRemote) {
            bHandled = true;
            return 0;
        }
        return BaseClass::OnMouseLButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);
    }
    if (!m_inputEngaged) {
        m_inputEngaged = true;
        SetLocalCursorHidden(false);
        fprintf(stderr, "[client] input engaged (first click on remote view)\n");
    }
    SendButtonMsg(true, 0, pt);
    return BaseClass::OnMouseLButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);
}

LRESULT RemoteWindow::OnMouseLButtonUpMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                          const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    SendButtonMsg(false, 0, pt);
    return BaseClass::OnMouseLButtonUpMsg(pt, modifierKey, nativeMsg, bHandled);
}

LRESULT RemoteWindow::OnMouseRButtonDownMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                            const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    // first click only ENGAGES remote control (consumed, not forwarded)
    if (m_cursorMode == 2) {
        const UiRect dest = ComputeBitmapDest();
        const bool overRemote = pt.x >= dest.left && pt.y >= dest.top &&
                                pt.x <= dest.right && pt.y <= dest.bottom;
        if (overRemote) {
            bHandled = true;
            return 0;
        }
        return BaseClass::OnMouseRButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);
    }
    if (!m_inputEngaged) {
        m_inputEngaged = true;
        SetLocalCursorHidden(false);
        bHandled = true;
        return 0;
    }
    SendButtonMsg(true, 1, pt);
    return BaseClass::OnMouseRButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);
}

LRESULT RemoteWindow::OnMouseRButtonUpMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                          const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    SendButtonMsg(false, 1, pt);
    return BaseClass::OnMouseRButtonUpMsg(pt, modifierKey, nativeMsg, bHandled);
}

// ---------------------------------------------------------------- actions

void RemoteWindow::ToggleFit()
{
    m_fitMode = !m_fitMode;
    m_fitPill->SetSelected(m_fitMode);
    ApplyDisplayMode();
    InvalidateAll();
}

void RemoteWindow::ApplyDisplayMode()
{
    if (m_bitmap == nullptr) {
        return;
    }
    // center the picture in every mode (the default left/top alignment left
    // a gap on the right whenever the window content area was wider than the
    // remote frame)
    m_bitmap->SetBitmapHAlignType(ui::HorAlignType::kAlignCenter);
    m_bitmap->SetBitmapVAlignType(ui::VerAlignType::kAlignCenter);
    if (m_fitMode) {
        m_bitmap->SetAdaptiveDestRect(true);  // proportional, centered
        m_bitmap->SetStretchedDrawing(false);
    }
    else {
        m_bitmap->SetAdaptiveDestRect(false); // fill: stretch to the window
        m_bitmap->SetStretchedDrawing(true);
    }
}

void RemoteWindow::SwitchCodec(int codec)
{
    if (codec == m_currentCodec) {
        return;
    }
    m_currentCodec = codec;
    if (m_h264Pill != nullptr) {
        m_h264Pill->SetSelected(codec == 1);
        m_vp9Pill->SetSelected(codec == 2);
        m_av1Pill->SetSelected(codec == 3);
        m_openh264Pill->SetSelected(codec == 4);
    }
    if (m_session && m_session->IsConnected()) {
        m_session->RequestCodec((uint8_t)codec);
    }
}

void RemoteWindow::ToggleFullscreen()
{
    m_fullscreen = !m_fullscreen;
    if (m_fullscreen) {
        EnterFullscreen();
    }
    else {
        ExitFullscreen();
    }
    // the pill label reflects the current state (全屏 <-> 退出全屏)
    if (m_fullscreenPill != nullptr) {
        m_fullscreenPill->SetText(m_fullscreen ? SDK_TR("remote.exitFullscreen")
                                               : SDK_TR("remote.fullscreen"));
    }
}

void RemoteWindow::Disconnect()
{
    if (m_disconnecting) {
        return;
    }
    m_disconnecting = true;
    SetLocalCursorHidden(false);
    if (m_session) {
        m_session->Stop();
        m_session.reset();
    }
    CloseWnd();
}

// ---------------------------------------------------------------- theme / language

void RemoteWindow::ApplyTheme()
{
    const Palette& p = Theme::Get();
    ui::VBox* root = dynamic_cast<ui::VBox*>(GetRoot());
    if (root != nullptr) {
        root->SetBkColor(p.windowBg);
    }
    if (m_bitmap != nullptr) {
        m_bitmap->SetBkColor(p.windowBg);
    }
    if (m_toolbar != nullptr) {
        m_toolbar->SetBkColor(p.panelBg);
    }
    InvalidateAll();
}

void RemoteWindow::UpdateCursorPillText()
{
    if (m_cursorPill == nullptr) {
        return;
    }
    m_cursorPill->SetText(m_cursorMode == 0 ? SDK_TR("remote.cursorLocal")
                          : (m_cursorMode == 1 ? SDK_TR("remote.cursorDual")
                                               : SDK_TR("remote.cursorDisplay")));
}

void RemoteWindow::ApplyLanguage()
{
    fprintf(stderr, "[lang] RemoteWindow::ApplyLanguage\n");
    m_lang.Apply();
    UpdateCursorPillText();
    m_fullscreenPill->SetText(m_fullscreen ? SDK_TR("remote.exitFullscreen")
                                           : SDK_TR("remote.fullscreen"));
    m_fitPill->SetText(SDK_TR("remote.fit"));
    m_sendFileBtn->SetText(SDK_TR("remote.sendFile"));
    m_disconnectBtn->SetIconToolTip(SDK_TR("remote.disconnect"));
    if (m_badgeLabel != nullptr) {
        m_badgeLabel->SetText(SDK_TR("remote.viewOnly"));
    }
    UpdateStatus();
    InvalidateAll();
}

} // namespace sdk
