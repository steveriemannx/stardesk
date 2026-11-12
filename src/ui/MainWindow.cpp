#include "MainWindow.h"

#include "../app/AppConfig.h"
#include "../app/Language.h"
#include "../app/Theme.h"
#include "../TextProbe.h"
#include "../net/Socket.h"
#include "../platform/InputInject.h"
#include "../ui/AskWindow.h"

#include "dui/Core/GlobalManager.h"
#include "dui/Utils/FilePathUtil.h"
#include "../ui/FileSendWindow.h"
#include "../ui/RemoteWindow.h"
#include "../ui/FileBrowserWindow.h"
#include "../ui/SettingsWindow.h"

#include <algorithm>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
// Screen Recording permission preflight/request (CoreGraphics).
extern "C" bool CGPreflightScreenCaptureAccess(void);
extern "C" bool CGRequestScreenCaptureAccess(void);
#endif

namespace sdk {

using ui::UiRect;
using ui::UiRectF;

// ---------------------------------------------------------------- ctor

MainWindow::MainWindow()
{
}

MainWindow::~MainWindow()
{
    // stop services first: their threads are joined here, so no callbacks can
    // fire after this point
    if (m_host) {
        m_host->Stop();
        m_host.reset();
    }
    if (m_client) {
        m_client->Stop();
        m_client.reset();
    }
    App::Instance().RemoveListener(this);
}

std::string MainWindow::GetSkinFolder()
{
    return "";
}

std::string MainWindow::GetSkinFile()
{
    // Pure code mode: no layout XML
    return "";
}

void MainWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
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

    // shadow_type="default" (system rounded shadow), same lookup as XML
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

// ---------------------------------------------------------------- helpers

ui::Label* MainWindow::MakeLabel(const char* key, ThemeLabel::Role role)
{
    ThemeLabel* label = new ThemeLabel(this);
    label->SetRole(role);
    if (key[0] != '\0') {
        m_lang.SetText(label, key);
    }
    return label;
}

ui::RichEdit* MainWindow::MakeEdit(const char* promptKey, bool password)
{
    ui::RichEdit* edit = new ui::RichEdit(this);
    m_lang.BindPrompt(edit, promptKey);
    edit->SetAttribute("single_line", "true");
    edit->SetAttribute("font", "system_12");
    edit->SetAttribute("text_align", "left,vcenter");
    SetTextPaddingForEdit(edit);
    if (password) {
        edit->SetAttribute("password", "true");
    }
    SetEditBk(edit);
    m_edits.push_back(edit);
    return edit;
}

void MainWindow::SetTextPaddingForEdit(ui::RichEdit* edit)
{
    if (edit != nullptr) {
        edit->SetTextPadding(ui::UiPadding(10, 0, 10, 0), true);
    }
}

void MainWindow::SetEditBk(ui::RichEdit* edit)
{
    const Palette& p = Theme::Get();
    edit->SetBkColor(p.inputBg);
    edit->SetAttribute("border_size", "1");
    edit->SetAttribute("border_color", Theme::Hex(p.border));
    edit->SetAttribute("border_round", "4,4");
    edit->SetAttribute("text_color", "sd_text_main");
    edit->SetAttribute("normal_text_color", "sd_text_main");
    edit->SetAttribute("prompt_color", "sd_text_sub");
}

PillButton* MainWindow::MakePill(const char* key)
{
    PillButton* pill = new PillButton(this);
    m_lang.SetText(pill, key);
    return pill;
}

IconButton* MainWindow::MakeIconButton(VectorArt::Icon icon, const char* tooltipKey,
                                       const std::string& name)
{
    IconButton* btn = new IconButton(this);
    btn->SetIcon(icon);
    if (tooltipKey[0] != '\0') {
        m_lang.BindToolTip(btn, tooltipKey);
    }
    btn->SetName(name);
    return btn;
}

// ---------------------------------------------------------------- window UI

void MainWindow::OnInitWindow()
{
    // pure-code windows must apply the caption/size-box rects themselves
    // (XML mode does this in WindowBuilder; attrs.m_rcCaption is not applied)
    SetSizeBox(ui::UiRect(4, 4, 4, 4), true);
    SetCaptionRect(ui::UiRect(0, 0, 0, 36), true);
    // system shadow (like shadow_type="system_round" in XML)
    SetShadowType(ui::Shadow::ShadowType::kShadowSystemRound);

#ifdef __APPLE__
    // Ask for Screen Recording permission once at startup: without it the
    // host falls back to the test pattern. CGRequestScreenCaptureAccess
    // shows the system dialog on the first call (main thread only).
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
    }
    // registering the app in the Accessibility / Input Monitoring lists needs
    // an explicit request, not just a preflight check
    if (!InputInjector::PermissionGranted()) {
        InputInjector::RequestPermission();
    }
#endif

    const Palette& p = Theme::Get();

    m_hiddenDevices.clear();
    for (const std::string& hidden : App::Instance().Config().hiddenDevices) {
        m_hiddenDevices.insert(ui::StringConvert::TToUTF8(hidden));
    }

    // ---- root ----
    ui::VBox* root = new ui::VBox(this);
    root->SetBkColor(p.windowBg);
    root->SetAttribute("width", "stretch");
    root->SetAttribute("height", "stretch");

    // ---- caption bar ----
    ui::HBox* caption = new ui::HBox(this);
    m_caption = caption;
    caption->SetAttribute("name", "window_caption_bar");
    caption->SetAttribute("height", "36");
    caption->SetBkColor(p.captionBg);
    root->AddItem(caption);

    // logo (drawn control)
    class LogoControl : public ui::Control {
        typedef ui::Control BaseClass;
    public:
        explicit LogoControl(ui::Window* w) : BaseClass(w) { SetMouseEnabled(false); }
        virtual void Paint(ui::IRender* render, const UiRect& rc) override {
            BaseClass::Paint(render, rc);
            VectorArt::DrawLogo(render, GetRect(), Theme::IsDark());
        }
    };
    LogoControl* logo = new LogoControl(this);
    logo->SetAttribute("width", "28");
    logo->SetAttribute("height", "28");
    logo->SetAttribute("margin", "8,4,4,4");
    caption->AddItem(logo);

    ThemeLabel* title = new ThemeLabel(this);
    title->SetRole(ThemeLabel::Role::Main);
    title->SetText("StarDesk");
    title->SetAttribute("font", "system_bold_14");
    title->SetAttribute("width", "auto");
    title->SetAttribute("height", "stretch");
    title->SetAttribute("margin", "2,0,0,0");
    caption->AddItem(title);

    ThemeLabel* subtitle = new ThemeLabel(this);
    subtitle->SetRole(ThemeLabel::Role::Sub);
    m_lang.SetText(subtitle, "app.subtitle");
    subtitle->SetAttribute("font", "system_12");
    subtitle->SetAttribute("width", "auto");
    subtitle->SetAttribute("height", "stretch");
    subtitle->SetAttribute("margin", "10,0,0,0");
    caption->AddItem(subtitle);

    // spacer
    ui::Control* spacer = new ui::Control(this);
    spacer->SetMouseEnabled(false);
    caption->AddItem(spacer);

    // language cycle button (shows the current language name)
    m_langBtn = new PillButton(this);
    m_langBtn->SetText(Language::NativeName(Language::Get()));
    m_langBtn->SetAttribute("width", "auto");
    m_langBtn->SetAttribute("padding", "12,0,12,0");
    m_langBtn->SetAttribute("height", "30");
    m_langBtn->SetAttribute("margin", "0,5,4,5");
    m_langBtn->AttachClick([this](const ui::EventArgs&) {
        CycleLanguage();
        return true;
    });
    caption->AddItem(m_langBtn);

    // theme toggle
    m_themeBtn = MakeIconButton(Theme::IsDark() ? VectorArt::Icon::Moon : VectorArt::Icon::Sun,
                                "settings.theme", "");
    m_themeBtn->SetAttribute("width", "30");
    m_themeBtn->SetAttribute("height", "30");
    m_themeBtn->SetAttribute("margin", "0,3,0,3");
    m_themeBtn->AttachClick([this](const ui::EventArgs&) {
        App::Instance().SetTheme(!Theme::IsDark());
        return true;
    });
    caption->AddItem(m_themeBtn);

#if !defined(__APPLE__)
    // min / max / restore / close (names drive dui's built-in caption behavior).
    // macOS gets the self-drawn traffic lights from dui instead: creating these
    // as well showed an unwanted maximize button at the right of the caption.
    IconButton* minBtn = MakeIconButton(VectorArt::Icon::Min, "window.min", "minbtn");
    minBtn->SetAttribute("width", "34");
    minBtn->SetAttribute("height", "30");
    minBtn->SetAttribute("margin", "0,3,0,3");
    caption->AddItem(minBtn);

    IconButton* maxBtn = MakeIconButton(VectorArt::Icon::Max, "window.max", "maxbtn");
    maxBtn->SetAttribute("width", "34");
    maxBtn->SetAttribute("height", "30");
    maxBtn->SetAttribute("margin", "0,3,0,3");
    caption->AddItem(maxBtn);

    IconButton* restoreBtn = MakeIconButton(VectorArt::Icon::Restore, "window.restore", "restorebtn");
    restoreBtn->SetAttribute("width", "34");
    restoreBtn->SetAttribute("height", "30");
    restoreBtn->SetAttribute("margin", "0,3,0,3");
    restoreBtn->SetVisible(false);
    caption->AddItem(restoreBtn);

    IconButton* closeBtn = MakeIconButton(VectorArt::Icon::Close, "window.close", "closebtn");
    closeBtn->SetAttribute("width", "34");
    closeBtn->SetAttribute("height", "30");
    closeBtn->SetAttribute("margin", "0,3,4,3");
    caption->AddItem(closeBtn);
#endif // !__APPLE__

    // ---- body ----
    ui::HBox* body = new ui::HBox(this);
    root->AddItem(body);

    // ================= left sidebar =================
    m_sidebar = new ui::VBox(this);
    m_sidebar->SetAttribute("width", "300");
    m_sidebar->SetAttribute("padding", "16,16,16,16");
    m_sidebar->SetAttribute("child_margin_y", "12");
    m_sidebar->SetBkColor(p.panelBg);
    body->AddItem(m_sidebar);

    // -- device card --
    CardBox* cardDevice = new CardBox(this);
    cardDevice->SetAttribute("padding", "16,12,16,14");
    cardDevice->SetAttribute("child_margin_y", "8");
    cardDevice->SetAttribute("height", "auto");
    m_sidebar->AddItem(cardDevice);

    ThemeLabel* t1 = static_cast<ThemeLabel*>(MakeLabel("host.device", ThemeLabel::Role::Title));
    cardDevice->AddItem(t1);

    m_deviceNameLabel = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Main));
    m_deviceNameLabel->SetText(App::Instance().Config().deviceName);
    m_deviceNameLabel->SetAttribute("font", "system_bold_12");
    m_deviceNameLabel->SetAttribute("width", "stretch");
    cardDevice->AddItem(m_deviceNameLabel);

    m_addrLabel = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Sub));
    m_addrLabel->SetText(ui::StringUtil::Printf("%d", (int)App::Instance().Config().port));
    m_addrLabel->SetAttribute("width", "stretch");
    m_addrLabel->SetAttribute("text_align", "left,top");
    m_addrLabel->SetAttribute("multi_line", "true");
    cardDevice->AddItem(m_addrLabel);

    // -- password card --
    CardBox* cardPassword = new CardBox(this);
    cardPassword->SetAttribute("padding", "16,12,16,14");
    cardPassword->SetAttribute("child_margin_y", "8");
    cardPassword->SetAttribute("height", "auto");
    m_sidebar->AddItem(cardPassword);

    cardPassword->AddItem(MakeLabel("host.password", ThemeLabel::Role::Title));

    ui::HBox* pwdRow = new ui::HBox(this);
    pwdRow->SetAttribute("height", "34");
    pwdRow->SetAttribute("child_margin_x", "4");
    cardPassword->AddItem(pwdRow);

    m_hostPwdEdit = MakeEdit("host.password", true);
    m_hostPwdEdit->SetText(App::Instance().Config().password);
    m_hostPwdEdit->SetAttribute("height", "34");
    pwdRow->AddItem(m_hostPwdEdit);

    IconButton* genBtn = MakeIconButton(VectorArt::Icon::Random, "host.genPassword", "");
    genBtn->SetAttribute("width", "30");
    genBtn->SetAttribute("height", "30");
    genBtn->AttachClick([this](const ui::EventArgs&) {
        const std::string pwd = AppConfig::GeneratePassword();
        m_hostPwdEdit->SetText(pwd);
        App::Instance().Config().password = pwd;
        App::Instance().Config().Save();
        return true;
    });
    pwdRow->AddItem(genBtn);

    m_pwdShowBtn = MakeIconButton(VectorArt::Icon::Eye, "host.showPassword", "");
    m_pwdShowBtn->SetAttribute("width", "30");
    m_pwdShowBtn->SetAttribute("height", "30");
    m_pwdShowBtn->AttachClick([this](const ui::EventArgs&) {
        m_pwdVisible = !m_pwdVisible;
        m_hostPwdEdit->SetAttribute("password", m_pwdVisible ? "false" : "true");
        m_pwdShowBtn->SetIconToolTip(SDK_TR(m_pwdVisible ? "host.hidePassword" : "host.showPassword"));
        return true;
    });
    pwdRow->AddItem(m_pwdShowBtn);

    // -- accept mode card --
    CardBox* cardAccept = new CardBox(this);
    cardAccept->SetAttribute("padding", "16,12,16,14");
    cardAccept->SetAttribute("child_margin_y", "8");
    cardAccept->SetAttribute("height", "auto");
    m_sidebar->AddItem(cardAccept);

    cardAccept->AddItem(MakeLabel("host.acceptMode", ThemeLabel::Role::Title));

    ui::HBox* acceptRow = new ui::HBox(this);
    acceptRow->SetAttribute("height", "30");
    acceptRow->SetAttribute("child_margin_x", "6");
    cardAccept->AddItem(acceptRow);

    m_autoAcceptPill = MakePill("host.autoAcceptShort");
    m_autoAcceptPill->SetAttribute("height", "30");
    m_autoAcceptPill->SetToolTipText(SDK_TR("host.autoAccept"));
    acceptRow->AddItem(m_autoAcceptPill);

    m_manualAcceptPill = MakePill("host.manualAcceptShort");
    m_manualAcceptPill->SetAttribute("height", "30");
    m_manualAcceptPill->SetToolTipText(SDK_TR("host.manualAccept"));
    acceptRow->AddItem(m_manualAcceptPill);

    const bool manual = App::Instance().Config().manualAccept;
    m_autoAcceptPill->SetSelected(!manual);
    m_manualAcceptPill->SetSelected(manual);

    auto selectAccept = [this](bool manual) {
        App::Instance().Config().manualAccept = manual;
        App::Instance().Config().Save();
        m_autoAcceptPill->SetSelected(!manual);
        m_manualAcceptPill->SetSelected(manual);
    };
    m_autoAcceptPill->AttachClick([selectAccept](const ui::EventArgs&) {
        selectAccept(false);
        return true;
    });
    m_manualAcceptPill->AttachClick([selectAccept](const ui::EventArgs&) {
        selectAccept(true);
        return true;
    });

    // -- share mode card --
    CardBox* cardShare = new CardBox(this);
    cardShare->SetAttribute("padding", "16,12,16,14");
    cardShare->SetAttribute("child_margin_y", "8");
    cardShare->SetAttribute("height", "auto");
    m_sidebar->AddItem(cardShare);

    cardShare->AddItem(MakeLabel("host.shareMode", ThemeLabel::Role::Title));

    ui::HBox* shareRow = new ui::HBox(this);
    shareRow->SetAttribute("height", "30");
    shareRow->SetAttribute("child_margin_x", "6");
    cardShare->AddItem(shareRow);

    m_mirrorPill = MakePill("host.shareMirrorShort");
    m_mirrorPill->SetToolTipText(SDK_TR("host.shareMirror"));
    m_mirrorPill->SetAttribute("height", "30");
    shareRow->AddItem(m_mirrorPill);

    m_extendPill = MakePill("host.shareExtendShort");
    m_extendPill->SetToolTipText(SDK_TR("host.shareExtend"));
    m_extendPill->SetAttribute("height", "30");
    shareRow->AddItem(m_extendPill);

    const bool extend = App::Instance().Config().extendScreen;
    m_mirrorPill->SetSelected(!extend);
    m_extendPill->SetSelected(extend);

    auto selectShare = [this](bool extend) {
        App::Instance().Config().extendScreen = extend;
        App::Instance().Config().Save();
        m_mirrorPill->SetSelected(!extend);
        m_extendPill->SetSelected(extend);
    };
    m_mirrorPill->AttachClick([selectShare](const ui::EventArgs&) {
        selectShare(false);
        return true;
    });
    m_extendPill->AttachClick([selectShare](const ui::EventArgs&) {
        selectShare(true);
        return true;
    });

    // -- host status card --
    CardBox* cardStatus = new CardBox(this);
    cardStatus->SetAttribute("padding", "16,12,16,14");
    cardStatus->SetAttribute("child_margin_y", "8");
    cardStatus->SetAttribute("height", "auto");
    m_sidebar->AddItem(cardStatus);

    cardStatus->AddItem(MakeLabel("host.status", ThemeLabel::Role::Title));

    m_hostStatusLabel = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Success));
    m_hostStatusLabel->SetText(SDK_TR("host.ready"));
    m_hostStatusLabel->SetAttribute("width", "stretch");
    m_hostStatusLabel->SetAttribute("text_align", "left,top");
    m_hostStatusLabel->SetAttribute("multi_line", "true");
    cardStatus->AddItem(m_hostStatusLabel);

    // permission warnings get their own line so the listening status is not
    // replaced by the hint
    m_permissionHint = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Danger));
    m_permissionHint->SetAttribute("width", "stretch");
    m_permissionHint->SetAttribute("text_align", "left,top");
    m_permissionHint->SetAttribute("multi_line", "true");
    m_permissionHint->SetVisible(false);
    cardStatus->AddItem(m_permissionHint);

    m_permissionBtn = new PillButton(this);
    m_lang.SetText(m_permissionBtn, "client.openSettings");
    m_permissionBtn->SetAttribute("width", "auto");
    m_permissionBtn->SetAttribute("padding", "12,0,12,0");
    m_permissionBtn->SetAttribute("height", "28");
    m_permissionBtn->SetVisible(false);
    m_permissionBtn->AttachClick([](const ui::EventArgs&) {
#ifdef __APPLE__
        // jump straight to the pane the user needs to change (modern pane id;
        // falls back to the legacy URL if the new one is rejected)
        const char* anchor = !InputInjector::PermissionGranted()
            ? "Privacy_Accessibility"
            : "Privacy_ScreenCapture";
        std::string url = std::string(
            "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?") + anchor;
        std::string command = std::string("open \"") + url + "\"";
        if (system(command.c_str()) != 0) {
            url = std::string("x-apple.systempreferences:com.apple.preference.security?") + anchor;
            command = std::string("open \"") + url + "\"";
            system(command.c_str());
        }
#endif
        return true;
    });
    cardStatus->AddItem(m_permissionBtn);

    // flexible spacer: cards keep their natural height, remaining sidebar space
    // stays empty instead of stretching every card
    ui::Control* sidebarSpacer = new ui::Control(this);
    sidebarSpacer->SetMouseEnabled(false);
    sidebarSpacer->SetAttribute("height", "stretch");
    m_sidebar->AddItem(sidebarSpacer);

    // ================= right side =================
    ui::VBox* mainArea = new ui::VBox(this);
    mainArea->SetAttribute("padding", "16,10,16,14");
    mainArea->SetAttribute("child_margin_y", "10");
    body->AddItem(mainArea);

    // -- tabs --
    ui::HBox* tabRow = new ui::HBox(this);
    tabRow->SetAttribute("child_margin_x", "6");
    // fixed height: the tabs must not stretch across the whole panel
    tabRow->SetAttribute("height", "32");
    mainArea->AddItem(tabRow);

    m_tabRemote = MakePill("client.title");
    m_tabRemote->SetAttribute("width", "104");
    m_tabRemote->SetAttribute("height", "32");
    tabRow->AddItem(m_tabRemote);

    m_tabFile = MakePill("file.title");
    m_tabFile->SetAttribute("width", "104");
    m_tabFile->SetAttribute("height", "32");
    tabRow->AddItem(m_tabFile);

    m_tabRemote->AttachClick([this](const ui::EventArgs&) {
        SelectTab(true);
        return true;
    });
    m_tabFile->AttachClick([this](const ui::EventArgs&) {
        SelectTab(false);
        return true;
    });

    // ---- remote tab ----
    m_panelRemote = new ui::VBox(this);
    m_panelRemote->SetAttribute("child_margin_y", "12");
    mainArea->AddItem(m_panelRemote);

    // LAN devices card
    CardBox* cardDevices = new CardBox(this);
    cardDevices->SetAttribute("padding", "16,12,16,14");
    cardDevices->SetAttribute("child_margin_y", "6");
    cardDevices->SetAttribute("height", "stretch");
    m_panelRemote->AddItem(cardDevices);

    ui::HBox* devicesHead = new ui::HBox(this);
    devicesHead->SetAttribute("height", "26");
    devicesHead->SetAttribute("child_margin_x", "8");
    cardDevices->AddItem(devicesHead);

    ThemeLabel* devicesTitle = static_cast<ThemeLabel*>(MakeLabel("client.devices", ThemeLabel::Role::Title));
    devicesTitle->SetAttribute("height", "22");
    devicesTitle->SetAttribute("width", "stretch");
    devicesHead->AddItem(devicesTitle);

    m_manageBtn = new PillButton(this);
    m_manageBtn->SetAttribute("width", "auto");
    m_manageBtn->SetAttribute("padding", "10,0,10,0");
    m_manageBtn->SetAttribute("height", "22");
    m_manageBtn->AttachClick([this](const ui::EventArgs&) {
        ToggleManageDevices();
        return true;
    });
    devicesHead->AddItem(m_manageBtn);
    UpdateManageButton();

    m_deviceList = new ui::VBox(this);
    m_deviceList->SetAttribute("child_margin_y", "4");
    cardDevices->AddItem(m_deviceList);

    m_deviceEmptyLabel = static_cast<ThemeLabel*>(MakeLabel("file.empty", ThemeLabel::Role::Sub));
    m_deviceEmptyLabel->SetText(SDK_TR("file.empty"));
    m_deviceEmptyLabel->SetAttribute("height", "26");
    m_deviceList->AddItem(m_deviceEmptyLabel);

    // connect card: compact form (advanced options moved to SettingsWindow)
    CardBox* cardConn = new CardBox(this);
    cardConn->SetAttribute("padding", "16,12,16,14");
    cardConn->SetAttribute("child_margin_y", "8");
    // hug the content: with every row at a fixed height the card is exactly
    // as tall as its fields, so it can never be squeezed into a collapse
    cardConn->SetAttribute("height", "auto");
    m_panelRemote->AddItem(cardConn);

    auto makeRow = [this](CardBox* parent, const char* height) {
        ui::HBox* row = new ui::HBox(this);
        row->SetAttribute("child_margin_x", "6");
        // fixed height rows: essential for stretch-free vertical layout
        row->SetAttribute("height", height);
        parent->AddItem(row);
        return row;
    };
    auto makeFieldLabel = [this](const char* key) {
        ThemeLabel* l = static_cast<ThemeLabel*>(MakeLabel(key, ThemeLabel::Role::Sub));
        // fixed width + stretch height: all labels share the same slot so the
        // input boxes line up, and the text stays vertically centred in the row.
        // The slot does not change with the language (long translations simply
        // ellipsize instead of moving the fields).
        l->SetAttribute("width", "140");
        l->SetAttribute("height", "stretch");
        l->SetAttribute("text_align", "left,vcenter");
        l->SetAttribute("margin", "0,0,8,0");
        return l;
    };

    // header: card title + settings button (mode/resolution/fps live there)
    ui::HBox* headRow = makeRow(cardConn, "32");
    m_connTitleLabel = static_cast<ThemeLabel*>(MakeLabel("client.title", ThemeLabel::Role::Title));
    m_connTitleLabel->SetAttribute("width", "auto");
    headRow->AddItem(m_connTitleLabel);
    ui::Control* headSpacer = new ui::Control(this);
    headSpacer->SetMouseEnabled(false);
    headRow->AddItem(headSpacer);
    m_settingsBtn = MakeIconButton(VectorArt::Icon::Gear, "settings.openSettings", "");
    m_settingsBtn->SetAttribute("width", "30");
    m_settingsBtn->SetAttribute("height", "30");
    m_settingsBtn->AttachClick([this](const ui::EventArgs&) {
        SettingsWindow* w = new SettingsWindow();
        w->CreateWnd(this, ui::WindowCreateParam("StarDesk", true));
        w->ShowWindow(ui::kSW_SHOW_NORMAL);
        return true;
    });
    headRow->AddItem(m_settingsBtn);

    // IP + port row
    ui::HBox* row1 = makeRow(cardConn, "34");
    row1->AddItem(makeFieldLabel("client.ip"));
    m_remoteIpEdit = MakeEdit("client.ip", false);
    m_remoteIpEdit->SetText("127.0.0.1");
    row1->AddItem(m_remoteIpEdit);

    row1->AddItem(makeFieldLabel("client.remotePort"));
    m_remotePortEdit = MakeEdit("client.port", false);
    m_remotePortEdit->SetText(ui::StringUtil::Printf("%d", (int)App::Instance().Config().port));
    m_remotePortEdit->SetAttribute("width", "70");
    row1->AddItem(m_remotePortEdit);

    // password row (with the same show/hide toggle as the host password)
    ui::HBox* row2 = makeRow(cardConn, "34");
    row2->AddItem(makeFieldLabel("client.remotePassword"));
    m_remotePwdEdit = MakeEdit("client.password", true);
    row2->AddItem(m_remotePwdEdit);

    m_remotePwdShowBtn = MakeIconButton(VectorArt::Icon::Eye, "client.showRemotePassword", "");
    m_remotePwdShowBtn->SetAttribute("width", "30");
    m_remotePwdShowBtn->SetAttribute("height", "30");
    m_remotePwdShowBtn->AttachClick([this](const ui::EventArgs&) {
        m_remotePwdVisible = !m_remotePwdVisible;
        m_remotePwdEdit->SetAttribute("password", m_remotePwdVisible ? "false" : "true");
        m_remotePwdShowBtn->SetIconToolTip(
            SDK_TR(m_remotePwdVisible ? "client.hideRemotePassword" : "client.showRemotePassword"));
        return true;
    });
    row2->AddItem(m_remotePwdShowBtn);

    // connect row
    ui::HBox* row3 = makeRow(cardConn, "40");
    m_connectBtn = new AccentButton(this);
    m_connectBtn->SetText(SDK_TR("client.connect"));
    m_connectBtn->SetAttribute("width", "124");
    m_connectBtn->SetAttribute("height", "38");
    m_connectBtn->AttachClick([this](const ui::EventArgs&) {
        OnConnectClicked();
        return true;
    });
    row3->AddItem(m_connectBtn);

    // status on its own line: sharing the row squeezed "已连接" down to a single
    // character on Retina macs
    ui::HBox* statusRow = makeRow(cardConn, "20");
    m_connStatusLabel = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Sub));
    m_connStatusLabel->SetText("");
    m_connStatusLabel->SetAttribute("width", "stretch");
    m_connStatusLabel->SetAttribute("height", "stretch");
    m_connStatusLabel->SetAttribute("text_align", "left,vcenter");
    statusRow->AddItem(m_connStatusLabel);

    // ---- file tab ----
    m_panelFile = new ui::VBox(this);
    m_panelFile->SetAttribute("child_margin_y", "12");
    m_panelFile->SetVisible(false);
    mainArea->AddItem(m_panelFile);

    // send card
    CardBox* cardSend = new CardBox(this);
    cardSend->SetAttribute("padding", "16,14,16,14");
    cardSend->SetAttribute("height", "auto");
    m_panelFile->AddItem(cardSend);

    ui::HBox* sendRow = new ui::HBox(this);
    sendRow->SetAttribute("child_margin_x", "12");
    sendRow->SetAttribute("height", "40");
    cardSend->AddItem(sendRow);

    ThemeLabel* hint = static_cast<ThemeLabel*>(MakeLabel("file.dragHint", ThemeLabel::Role::Sub));
    hint->SetAttribute("width", "stretch");
    hint->SetAttribute("height", "stretch");
    hint->SetAttribute("text_align", "left,vcenter");
    sendRow->AddItem(hint);

    m_sendFileBtn = new AccentButton(this);
    m_sendFileBtn->SetText(SDK_TR("file.title"));
    m_sendFileBtn->SetAttribute("width", "140");
    m_sendFileBtn->SetAttribute("height", "38");
    m_sendFileBtn->AttachClick([this](const ui::EventArgs&) {
        OnSendFileClicked();
        return true;
    });
    sendRow->AddItem(m_sendFileBtn);

    // receive card
    CardBox* cardReceive = new CardBox(this);
    cardReceive->SetAttribute("padding", "16,12,16,14");
    cardReceive->SetAttribute("child_margin_y", "8");
    cardReceive->SetAttribute("height", "stretch");
    m_panelFile->AddItem(cardReceive);

    ui::HBox* dirRow = new ui::HBox(this);
    dirRow->SetAttribute("child_margin_x", "6");
    dirRow->SetAttribute("height", "28");
    cardReceive->AddItem(dirRow);

    ThemeLabel* dirTitle = static_cast<ThemeLabel*>(MakeLabel("host.fileSaveDir", ThemeLabel::Role::Title));
    dirTitle->SetAttribute("width", "auto");
    dirRow->AddItem(dirTitle);

    ui::Control* dirSpacer = new ui::Control(this);
    dirSpacer->SetMouseEnabled(false);
    dirRow->AddItem(dirSpacer);

    PillButton* openDirBtn = new PillButton(this);
    m_lang.SetText(openDirBtn, "file.openDir");
    openDirBtn->SetAttribute("width", "88");
    openDirBtn->SetAttribute("height", "28");
    openDirBtn->AttachClick([this](const ui::EventArgs&) {
        OnOpenReceiveDir();
        return true;
    });
    dirRow->AddItem(openDirBtn);

    m_receiveDirLabel = static_cast<ThemeLabel*>(MakeLabel("", ThemeLabel::Role::Sub));
    std::string receiveDir = App::Instance().Config().receiveDir;
    if (receiveDir.empty()) {
        receiveDir = AppConfig::GetDefaultReceiveDir().ToString();
    }
    m_receiveDirLabel->SetText(receiveDir);
    m_receiveDirLabel->SetAttribute("width", "stretch");
    m_receiveDirLabel->SetAttribute("height", "22");
    m_receiveDirLabel->SetAttribute("text_align", "left,vcenter");
    cardReceive->AddItem(m_receiveDirLabel);

    ThemeLabel* transferTitle = static_cast<ThemeLabel*>(MakeLabel("file.transfers", ThemeLabel::Role::Title));
    transferTitle->SetAttribute("height", "22");
    transferTitle->SetAttribute("margin", "0,4,0,0");
    cardReceive->AddItem(transferTitle);

    m_transferList = new ui::VBox(this);
    m_transferList->SetAttribute("child_margin_y", "6");
    m_transferList->SetAttribute("height", "stretch");
    cardReceive->AddItem(m_transferList);

    m_transferEmptyLabel = static_cast<ThemeLabel*>(MakeLabel("file.empty", ThemeLabel::Role::Sub));
    m_transferEmptyLabel->SetText(SDK_TR("file.empty"));
    m_transferEmptyLabel->SetAttribute("width", "stretch");
    m_transferEmptyLabel->SetAttribute("height", "stretch");
    m_transferEmptyLabel->SetAttribute("text_align", "hcenter,vcenter");
    m_transferList->AddItem(m_transferEmptyLabel);

    AttachBox(root);

    App::Instance().AddListener(this);

    // test hook: STARDESK_TEST_LANG_CYCLE=1 cycles through the 8 languages,
    // one every 1.5s, so a log-only run can verify that every window re-applies
    // its strings on a runtime language switch.
    if (std::getenv("STARDESK_TEST_LANG_CYCLE") != nullptr) {
        std::thread([this]() {
            for (int i = 1; i <= (int)Lang::Count; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                const Lang lang = (Lang)(i % (int)Lang::Count);
                PostToUI(ui::UiBind(this, [this, lang]() {
                    App::Instance().SetLanguage(lang);
                }));
            }
        }).detach();
    }
    SelectTab(true);

    // ---- host service (always listening) ----
    m_host = std::make_unique<HostService>();
    HostSession::Callbacks hostCb;
    hostCb.onStatus = [this](const std::string& text, bool ok) {
        PostToUI(ui::UiBind(this, [this, text, ok]() { OnHostStatus(text, ok); }));
    };
    hostCb.onAsk = [this](HostSession* session, const AskInfo& info) {
        PostToUI(ui::UiBind(this, [this, session, info]() {
            if (session == nullptr) {
                return;
            }
            AskWindow* dlg = new AskWindow(info, [session](bool accept) {
                session->RespondAsk(accept);
            });
            dlg->CreateWnd(this, ui::WindowCreateParam("StarDesk", true));
            dlg->ShowWindow(ui::kSW_SHOW_NORMAL);
        }));
    };
    hostCb.onClosed = []() {
        // host status is restored by onStatus in HostSession::Run
    };
    // Bind the configured port; when it is taken (e.g. a second StarDesk
    // instance already runs on this machine) HostService falls back to the
    // next free port so both instances can host at once. The address label,
    // discovery beacon and file port all follow the bound port.
    if (m_host->Start(App::Instance().Config().port, hostCb)) {
        m_actualPort = m_host->Port();
    }
    else {
        SetHostStatus(SDK_TR("client.networkError"), false);
        m_actualPort = App::Instance().Config().port; // UI shows the configured value
    }
    // remote input injection needs the Accessibility permission (macOS);
    // without it mouse/keyboard events are silently dropped by the system
    RefreshPermissionState();

    // poll while the app runs so granting a permission in System Settings is
    // picked up without restarting the app
    std::thread([this]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            PostToUI(ui::UiBind(this, [this]() {
                RefreshPermissionState();
            }));
        }
    }).detach();

    // global file-transfer progress -> transfer list
    App::Instance().FileTx().SetItemCallback([this](const FileTransfer::Item& item) {
        PostToUI(ui::UiBind(this, [this, item]() { OnItemUpdated(item); }));
    });

    // Test hook: STARDESK_AUTOCONNECT=ip:port fills the connect form and
    // connects immediately (automated two-instance testing).
    if (const char* ac = std::getenv("STARDESK_AUTOCONNECT")) {
        const std::string s(ac);
        const size_t colon = s.find(':');
        m_remoteIpEdit->SetText(ui::StringConvert::UTF8ToT(
            colon == std::string::npos ? s : s.substr(0, colon)));
        if (colon != std::string::npos) {
            m_remotePortEdit->SetText(ui::StringConvert::UTF8ToT(s.substr(colon + 1)));
        }
        m_remotePwdEdit->SetText(App::Instance().Config().password);
        OnConnectClicked();
    }

    // LAN discovery: beacon + device list (advertises the bound port)
    const std::string deviceName =
        ui::StringConvert::TToUTF8(App::Instance().Config().deviceName);
    App::Instance().Disc().Start(deviceName, m_actualPort,
        [this](const std::vector<Discovery::Device>& devices) {
            PostToUI(ui::UiBind(this, [this, devices]() {
                std::vector<std::pair<std::string, std::string>> rows;
                for (const Discovery::Device& d : devices) {
                    const std::string ip = ui::StringConvert::UTF8ToT(d.ip);
                    const std::string ipPort = ui::StringUtil::Printf(
                        "%s:%d", ip.c_str(), (int)d.port);
                    rows.emplace_back(ui::StringConvert::UTF8ToT(d.name), ipPort);
                }
                SetLanDevices(rows);
            }));
        });

    // local IPs
    std::vector<std::string> ips;
    for (const std::string& ip : Socket::GetLocalIPv4s()) {
        ips.push_back(ui::StringConvert::UTF8ToT(ip));
    }
    SetLocalAddresses(ips);

    BaseClass::OnInitWindow();

    // test hook: STARDESK_AUTO_HOST[+_PORT+_PASSWORD] (or config auto_host)
    // connects automatically so fps/latency can be measured without GUI clicks
    const AppConfig& cfg = App::Instance().Config();
    const char* autoHost = std::getenv("STARDESK_AUTO_HOST");
    std::string autoHostCfg = autoHost != nullptr ? ui::StringConvert::UTF8ToT(autoHost)
                                              : cfg.autoHost;
    if (!autoHostCfg.empty()) {
        m_remoteIpEdit->SetText(autoHostCfg);
        if (const char* autoPort = std::getenv("STARDESK_AUTO_PORT")) {
            m_remotePortEdit->SetText(ui::StringConvert::UTF8ToT(autoPort));
        }
        else if (cfg.autoPort > 0) {
            m_remotePortEdit->SetText(ui::StringUtil::Printf("%d", cfg.autoPort));
        }
        if (const char* autoPwd = std::getenv("STARDESK_AUTO_PASSWORD")) {
            m_remotePwdEdit->SetText(ui::StringConvert::UTF8ToT(autoPwd));
        }
        else if (!cfg.autoPassword.empty()) {
            m_remotePwdEdit->SetText(cfg.autoPassword);
        }
        OnConnectClicked();
    }
}

// ---------------------------------------------------------------- logic

void MainWindow::SelectTab(bool remote)
{
    m_tabRemote->SetSelected(remote);
    m_tabFile->SetSelected(!remote);
    m_panelRemote->SetVisible(remote);
    m_panelFile->SetVisible(!remote);
}

void MainWindow::CycleLanguage()
{
    const int cur = (int)Language::Get();
    const int next = (cur + 1) % Language::Count();
    App::Instance().SetLanguage((Lang)next);
}

void MainWindow::OnConnectClicked()
{
    // While a remote window owns the session, the main button acts as the
    // disconnect switch: give immediate feedback instead of silently queueing
    // another connection attempt.
    if (m_remoteWin != nullptr) {
        m_connectBtn->SetText(SDK_TR("client.disconnecting"));
        m_connectBtn->SetEnabled(false);
        m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
        m_connStatusLabel->SetText(SDK_TR("client.disconnecting"));
        m_remoteWin->CloseWnd();
        return;
    }
    // a connection attempt is already running: cancel it with feedback
    if (m_client) {
        m_connectBtn->SetText(SDK_TR("client.disconnecting"));
        m_connectBtn->SetEnabled(false);
        m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
        m_connStatusLabel->SetText(SDK_TR("client.disconnecting"));
        m_client->Stop();
        m_client.reset();
        m_connectBtn->SetText(SDK_TR("client.connect"));
        m_connectBtn->SetEnabled(true);
        return;
    }

    ClientSession::Options opt;
    opt.host = ui::StringConvert::TToUTF8(m_remoteIpEdit->GetText());
    opt.port = (uint16_t)std::max(1, std::min(65535,
                std::atoi(ui::StringConvert::TToUTF8(m_remotePortEdit->GetText()).c_str())));
    opt.password = m_remotePwdEdit->GetText();
    // advanced options are configured in the settings window (persisted)
    const AppConfig& cfg = App::Instance().Config();
    opt.mode = cfg.viewOnly ? ConnMode::View : ConnMode::Control;
    opt.resCode = cfg.wantRes;
    opt.fps = cfg.wantFps;
    // test hook: STARDESK_AUTO_RES / STARDESK_AUTO_FPS override for this session
    if (const char* ar = std::getenv("STARDESK_AUTO_RES")) {
        opt.resCode = (uint8_t)std::atoi(ar);
    }
    if (const char* af = std::getenv("STARDESK_AUTO_FPS")) {
        opt.fps = (uint8_t)std::atoi(af);
    }

    if (opt.host.empty() || opt.port == 0) {
        m_connStatusLabel->SetText(SDK_TR("client.networkError"));
        return;
    }

    ClientSession::Callbacks cb;
    cb.onState = [this](ClientSession::State st, const std::string& text) {
        PostToUI(ui::UiBind(this, [this, st, text]() { OnClientState(st, text); }));
    };

    m_client = std::make_unique<ClientSession>(opt, std::move(cb));
    // stays clickable: pressing it again cancels the attempt
    m_connectBtn->SetText(SDK_TR("client.cancelConnect"));
    m_connectBtn->SetEnabled(true);
    m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
    m_connStatusLabel->SetText(SDK_TR("client.connecting"));
    m_client->Start();
}

void MainWindow::OnClientState(ClientSession::State state, const std::string& text)
{
    // diagnostics: every client state transition goes to stderr so a
    // headless run (logs only) can be followed without the GUI
    fprintf(stderr, "[client] state=%d text=%s\n", (int)state,
            ui::StringConvert::TToUTF8(text).c_str());
    fflush(stderr);
    if (state == ClientSession::State::Connected) {
        m_connectBtn->SetText(SDK_TR("client.disconnect"));
        m_connectBtn->SetDangerLook(false);
        m_connectBtn->SetEnabled(true);
        m_connStatusLabel->SetRole(ThemeLabel::Role::Success);
        m_connStatusLabel->SetText(text);

        // hand the session to a standalone remote window (NOT a child of the
        // main window): the main "start" page and the video page are two
        // independent windows and both stay visible/usable
        if (m_client) {
            ClientSession* sessionPtr = m_client.get();
            RemoteWindow* rw = new RemoteWindow(std::move(m_client));
            m_remoteWin = rw;
            rw->SetClosedCallback([this]() {
                m_remoteWin = nullptr; // synchronous: do not leave a stale pointer
                PostToUI(ui::UiBind(this, [this]() {
                    m_connectBtn->SetText(SDK_TR("client.connect"));
                    m_connectBtn->SetDangerLook(false);
                    m_connectBtn->SetEnabled(true);
                    m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
                    m_connStatusLabel->SetText(SDK_TR("client.remoteDisconnected"));
                }));
            });
            rw->CreateWnd(nullptr, ui::WindowCreateParam("StarDesk", true));
            // Offset the remote window from the main window instead of letting
            // the two same-sized windows overlap completely.
            const ui::UiRect rcMainPos = GetWindowPos(false);
            const ui::UiRect rcRemotePos = rw->GetWindowPos(false);
            fprintf(stderr, "[client] main window pos=%d,%d %dx%d remote pos=%d,%d %dx%d\n",
                    rcMainPos.left, rcMainPos.top, rcMainPos.Width(), rcMainPos.Height(),
                    rcRemotePos.left, rcRemotePos.top, rcRemotePos.Width(),
                    rcRemotePos.Height());
            fflush(stderr);
            const int32_t cascadeX = rw->Dpi().GetScaleInt(36);
            const int32_t cascadeY = rw->Dpi().GetScaleInt(26);
            rw->MoveWindow(rcRemotePos.left + cascadeX, rcRemotePos.top + cascadeY,
                           rcRemotePos.Width(), rcRemotePos.Height(), true);
            const ui::UiRect rcRemoteAfter = rw->GetWindowPos(false);
            fprintf(stderr, "[client] remote window moved to=%d,%d\n",
                    rcRemoteAfter.left, rcRemoteAfter.top);
            fflush(stderr);
            rw->ShowWindow(ui::kSW_SHOW_NORMAL);
            // the remote view must be the front window - the main window
            // stays visible behind it but the user expects the video page
            rw->SetWindowForeground();

            // the file browser is opened on demand (the 传输文件 button); it
            // is NOT auto-opened on connect anymore (feature is tested)
        }
    }
    else if (state == ClientSession::State::Failed) {
        m_connStatusLabel->SetRole(ThemeLabel::Role::Danger);
        m_connStatusLabel->SetText(text);
        m_connectBtn->SetText(SDK_TR("client.connect"));
        m_connectBtn->SetEnabled(true);
        if (m_client) {
            m_client->Stop();
            m_client.reset();
        }
    }
    else if (state == ClientSession::State::Closed) {
        m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
        m_connStatusLabel->SetText(text);
        m_connectBtn->SetText(SDK_TR("client.connect"));
        m_connectBtn->SetEnabled(true);
    }
    else {
        m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
        m_connStatusLabel->SetText(text);
    }
}

void MainWindow::OnHostStatus(const std::string& text, bool ok)
{
    SetHostStatus(text, ok);
}

// ---------------------------------------------------------------- file tab

void MainWindow::OnSendFileClicked()
{
    // client side: session to the peer; host side: first connected session
    if (m_client && m_client->IsConnected()) {
        FileSendWindow* w = new FileSendWindow(m_client->GetHost(), m_client->FilePort(),
                                               m_client->Token());
        w->CreateWnd(this, ui::WindowCreateParam("StarDesk", true));
        w->ShowWindow(ui::kSW_SHOW_NORMAL);
        return;
    }
    if (m_host) {
        HostSessionPtr s = m_host->FirstConnectedSession();
        if (s) {
            FileSendWindow* w = new FileSendWindow(s->PeerIp(),
                                                   App::Instance().FileTx().BoundPort(),
                                                   s->Token());
            w->CreateWnd(this, ui::WindowCreateParam("StarDesk", true));
            w->ShowWindow(ui::kSW_SHOW_NORMAL);
            return;
        }
    }
    m_connStatusLabel->SetRole(ThemeLabel::Role::Sub);
    m_connStatusLabel->SetText(SDK_TR("client.remoteDisconnected"));
}

void MainWindow::OnItemUpdated(const FileTransfer::Item& item)
{
    m_transfers[item.id] = item;
    // rebuild the list lazily (few transfers; simple and correct)
    if (m_transferList == nullptr) {
        return;
    }
    ClearTransferRows();
    size_t count = 0;
    for (const auto& kv : m_transfers) {
        if (count >= 8) {
            break;
        }
        const FileTransfer::Item& it = kv.second;
        CardBox* row = new CardBox(this);
        row->SetAttribute("height", "34");
        row->SetAttribute("padding", "10,4,10,4");
        m_transferList->AddItem(row);

        ui::VBox* info = new ui::VBox(this);
        info->SetAttribute("child_margin_y", "1");
        // NOTE: `info` is added to the row exactly once (below); adding it
        // twice made Box::~Box free it twice (double-free -> SEGV in the
        // transfer list rebuild)

        ThemeLabel* name = new ThemeLabel(this);
        name->SetRole(ThemeLabel::Role::Main);
        name->SetText(ui::StringConvert::UTF8ToT(it.name));
        name->SetAttribute("width", "stretch");
        name->SetAttribute("font", "system_12");
        info->AddItem(name);

        ThemeLabel* status = new ThemeLabel(this);
        ThemeLabel::Role role = ThemeLabel::Role::Sub;
        std::string text;
        switch (it.state) {
        case FileTransfer::ItemState::Transferring:
            text = ui::StringUtil::Printf("%llu / %llu KB",
                                          (unsigned long long)(it.transferred / 1024),
                                          (unsigned long long)(it.size / 1024));
            break;
        case FileTransfer::ItemState::Done:
            role = ThemeLabel::Role::Success;
            text = SDK_TR("file.done");
            break;
        case FileTransfer::ItemState::Failed:
            role = ThemeLabel::Role::Danger;
            text = SDK_TR("file.failed");
            break;
        case FileTransfer::ItemState::Rejected:
            role = ThemeLabel::Role::Danger;
            text = SDK_TR("file.rejected");
            break;
        case FileTransfer::ItemState::Cancelled:
            text = SDK_TR("file.cancelled");
            break;
        default:
            text = SDK_TR("file.waiting");
            break;
        }
        status->SetRole(role);
        status->SetText(text);
        status->SetAttribute("width", "stretch");
        info->AddItem(status);

        row->AddItem(info);
        ++count;
    }
    if (count == 0) {
        m_transferEmptyLabel = static_cast<ThemeLabel*>(MakeLabel("file.empty", ThemeLabel::Role::Sub));
        m_transferEmptyLabel->SetText(SDK_TR("file.empty"));
        m_transferEmptyLabel->SetAttribute("height", "24");
        m_transferList->AddItem(m_transferEmptyLabel);
    }
    InvalidateAll();
}

void MainWindow::ClearTransferRows()
{
    if (m_transferList == nullptr) {
        return;
    }
    m_lang.Unbind(m_transferEmptyLabel);
    m_transferList->RemoveAllItems();
    m_transferEmptyLabel = nullptr;
}

void MainWindow::RebuildTransferList()
{
    if (m_transferList == nullptr) {
        return;
    }
    ClearTransferRows();
    // re-render from the stored map (language switch etc.)
    size_t count = 0;
    for (const auto& kv : m_transfers) {
        if (count >= 8) {
            break;
        }
        CardBox* row = new CardBox(this);
        row->SetAttribute("height", "34");
        row->SetAttribute("padding", "10,4,10,4");
        m_transferList->AddItem(row);
        ThemeLabel* name = new ThemeLabel(this);
        name->SetRole(ThemeLabel::Role::Main);
        name->SetText(ui::StringConvert::UTF8ToT(kv.second.name));
        name->SetAttribute("width", "stretch");
        name->SetAttribute("font", "system_12");
        row->AddItem(name);
        ++count;
    }
    if (count == 0) {
        m_transferEmptyLabel = static_cast<ThemeLabel*>(MakeLabel("file.empty", ThemeLabel::Role::Sub));
        m_transferEmptyLabel->SetText(SDK_TR("file.empty"));
        m_transferEmptyLabel->SetAttribute("height", "24");
        m_transferList->AddItem(m_transferEmptyLabel);
    }
}

void MainWindow::OnOpenReceiveDir()
{
    std::string dir = App::Instance().Config().receiveDir;
    if (dir.empty()) {
        dir = AppConfig::GetDefaultReceiveDir().ToString();
    }
    ui::FilePathUtil::CreateDirectories(dir);
#ifdef DUI_BUILD_FOR_WIN
    const std::string cmd = "explorer "" + ui::StringConvert::TToUTF8(dir) + """;
#else
    const std::string cmd = "open "" + ui::StringConvert::TToUTF8(dir) + """;
#endif
    std::system(cmd.c_str());
}

void MainWindow::SetHostStatus(const std::string& text, bool ok)
{
    m_hostStatusLabel->SetRole(ok ? ThemeLabel::Role::Success : ThemeLabel::Role::Danger);
    m_hostStatusLabel->SetText(text);
}

void MainWindow::SetLocalAddresses(const std::vector<std::string>& ips)
{
    std::string text;
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) {
            text += "\n";
        }
        text += ips[i];
        text += ui::StringUtil::Printf(":%d", (int)m_actualPort);
    }
    if (text.empty()) {
        text = ui::StringUtil::Printf("127.0.0.1:%d", (int)m_actualPort);
    }
    m_localAddresses = ips;
    m_addrLabel->SetText(text);
}

void MainWindow::SetLanDevices(const std::vector<std::pair<std::string, std::string>>& nameIpPorts)
{
    m_lastLanDevices = nameIpPorts;
    // rebuild the device row list (the empty hint is destroyed and re-created,
    // so drop its language binding first or Apply() would touch freed memory)
    m_lang.Unbind(m_deviceEmptyLabel);
    m_deviceList->RemoveAllItems();
    m_deviceEmptyLabel = nullptr;

    // hidden peers stay out of the list until restored; our own beacons (this
    // machine on our own port) are filtered so only other machines are listed
    auto isSelf = [this](const std::string& ipPort) {
        const size_t colon = ipPort.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const std::string ip = ipPort.substr(0, colon);
        int port = 0;
        try {
            port = std::stoi(ipPort.substr(colon + 1));
        }
        catch (...) {
            return false;
        }
        if (port != (int)m_actualPort) {
            return false;
        }
        if (ip == "127.0.0.1" || ip == "::1") {
            return true;
        }
        for (const std::string& localIp : m_localAddresses) {
            if (localIp == ip) {
                return true;
            }
        }
        return false;
    };

    std::vector<std::pair<std::string, std::string>> visible;
    visible.reserve(nameIpPorts.size());
    for (const auto& nameIp : nameIpPorts) {
        if (m_hiddenDevices.count(nameIp.second) != 0 || isSelf(nameIp.second)) {
            continue;
        }
        visible.push_back(nameIp);
    }

    for (const auto& nameIp : visible) {
        const bool removable = m_manageDevices;
        CardHBox* row = new CardHBox(this);
        row->SetAttribute("height", "34");
        row->SetAttribute("padding", "12,0,12,0");
        row->SetAttribute("child_margin_x", "8");

        ThemeLabel* nameLabel = new ThemeLabel(this);
        nameLabel->SetRole(ThemeLabel::Role::Main);
        nameLabel->SetText(nameIp.first);
        nameLabel->SetAttribute("width", "stretch");
        nameLabel->SetAttribute("height", "stretch");
        row->AddItem(nameLabel);

        ThemeLabel* addrLabel = new ThemeLabel(this);
        addrLabel->SetRole(ThemeLabel::Role::Sub);
        addrLabel->SetText(nameIp.second);
        addrLabel->SetAttribute("width", "auto");
        addrLabel->SetAttribute("height", "stretch");
        row->AddItem(addrLabel);

        if (removable) {
            IconButton* removeBtn = new IconButton(this);
            removeBtn->SetIcon(VectorArt::Icon::Close);
            removeBtn->SetIconToolTip(SDK_TR("client.removeDevice"));
            removeBtn->SetAttribute("width", "22");
            removeBtn->SetAttribute("height", "22");
            const std::string ipPort = nameIp.second;
            removeBtn->AttachClick([this, ipPort](const ui::EventArgs&) {
                HideDevice(ipPort);
                return true;
            });
            row->AddItem(removeBtn);
        }

        const std::string ipPort = nameIp.second;
        row->AttachClick([this, ipPort](const ui::EventArgs&) {
            if (m_manageDevices) {
                return true; // managing: row clicks do not start a connection
            }
            // "ip:port" -> fill both fields
            const size_t colon = ipPort.find((':'));
            m_remoteIpEdit->SetText(ipPort.substr(0, colon));
            if (colon != std::string::npos) {
                m_remotePortEdit->SetText(ipPort.substr(colon + 1));
            }
            return true;
        });
        m_deviceList->AddItem(row);
    }

    if (visible.empty()) {
        const char* emptyKey = m_hiddenDevices.empty() ? "client.searching" : "client.allHidden";
        m_deviceEmptyLabel = static_cast<ThemeLabel*>(MakeLabel(emptyKey, ThemeLabel::Role::Sub));
        m_deviceEmptyLabel->SetText(SDK_TR(emptyKey));
        m_deviceEmptyLabel->SetAttribute("width", "stretch");
        m_deviceEmptyLabel->SetAttribute("height", "stretch");
        m_deviceEmptyLabel->SetAttribute("text_align", "hcenter,vcenter");
        m_deviceList->AddItem(m_deviceEmptyLabel);
    }

    if (m_manageDevices && !m_hiddenDevices.empty()) {
        PillButton* restoreBtn = new PillButton(this);
        restoreBtn->SetText(SDK_TR("client.restoreDevices"));
        restoreBtn->SetAttribute("width", "auto");
        restoreBtn->SetAttribute("padding", "12,0,12,0");
        restoreBtn->SetAttribute("height", "28");
        restoreBtn->AttachClick([this](const ui::EventArgs&) {
            RestoreHiddenDevices();
            return true;
        });
        m_deviceList->AddItem(restoreBtn);
    }
}

void MainWindow::UpdateManageButton()
{
    if (m_manageBtn == nullptr) {
        return;
    }
    m_manageBtn->SetText(SDK_TR(m_manageDevices ? "client.manageDone" : "client.manage"));
    m_manageBtn->SetSelected(m_manageDevices);
}

void MainWindow::ToggleManageDevices()
{
    m_manageDevices = !m_manageDevices;
    UpdateManageButton();
    SetLanDevices(m_lastLanDevices);
}

void MainWindow::HideDevice(const std::string& ipPort)
{
    m_hiddenDevices.insert(ipPort);
    AppConfig& cfg = App::Instance().Config();
    cfg.hiddenDevices.clear();
    for (const std::string& hidden : m_hiddenDevices) {
        cfg.hiddenDevices.push_back(hidden);
    }
    cfg.Save();
    SetLanDevices(m_lastLanDevices);
}

void MainWindow::RestoreHiddenDevices()
{
    m_hiddenDevices.clear();
    AppConfig& cfg = App::Instance().Config();
    cfg.hiddenDevices.clear();
    cfg.Save();
    SetLanDevices(m_lastLanDevices);
}

// ---------------------------------------------------------------- theme / language

void MainWindow::ApplyThemeEdits()
{
    for (ui::RichEdit* edit : m_edits) {
        SetEditBk(edit);
        SetTextPaddingForEdit(edit);
    }
}

void MainWindow::RefreshPermissionState()
{
    if (m_permissionHint == nullptr || m_permissionBtn == nullptr) {
        return;
    }
    std::string permissionHint;
    std::string permissionTooltip;
#ifdef __APPLE__
    if (!InputInjector::PermissionGranted()) {
        permissionHint = SDK_TR("client.permAccessibilityShort");
        permissionTooltip = SDK_TR("remote.accessibilityHint");
    }
    if (!CGPreflightScreenCaptureAccess()) {
        if (!permissionHint.empty()) {
            permissionHint += "\n";
            permissionTooltip += "\n\n";
        }
        permissionHint += SDK_TR("client.permScreenShort");
        permissionTooltip += SDK_TR("remote.permissionHint");
    }
#endif
#ifdef __APPLE__
    if (!permissionTooltip.empty()) {
        permissionTooltip += "\n\nApp: /Applications/StarDesk.app";
    }
#endif
    const bool needed = !permissionHint.empty();
    if (m_permissionHint->IsVisible() != needed) {
        m_permissionHint->SetVisible(needed);
        m_permissionBtn->SetVisible(needed);
    }
    if (needed && m_permissionHint->GetText() != permissionHint) {
        m_permissionHint->SetText(permissionHint);
        m_permissionHint->SetToolTipText(permissionTooltip);
    }
}

void MainWindow::ApplyTheme()
{
    if (m_caption != nullptr) {
        m_caption->SetBkColor(Theme::Get().captionBg);
    }
    const Palette& p = Theme::Get();
    ui::VBox* root = dynamic_cast<ui::VBox*>(GetRoot());
    if (root != nullptr) {
        root->SetBkColor(p.windowBg);
    }
    if (m_sidebar != nullptr) {
        m_sidebar->SetBkColor(p.panelBg);
    }
    ApplyThemeEdits();
    m_themeBtn->SetIcon(Theme::IsDark() ? VectorArt::Icon::Moon : VectorArt::Icon::Sun);
    InvalidateAll();
}

void MainWindow::ApplyLanguage()
{
    fprintf(stderr, "[lang] MainWindow::ApplyLanguage\n");
    m_lang.Apply();
    m_langBtn->SetText(Language::NativeName(Language::Get()));

    m_tabRemote->SetText(SDK_TR("client.title"));
    m_tabFile->SetText(SDK_TR("file.title"));
    m_autoAcceptPill->SetText(SDK_TR("host.autoAcceptShort"));
    m_manualAcceptPill->SetText(SDK_TR("host.manualAcceptShort"));
    m_mirrorPill->SetText(SDK_TR("host.shareMirrorShort"));
    m_extendPill->SetText(SDK_TR("host.shareExtendShort"));
    m_connTitleLabel->SetText(SDK_TR("client.title"));
    m_settingsBtn->SetIconToolTip(SDK_TR("settings.openSettings"));
    m_connectBtn->SetText(SDK_TR("client.connect"));

    // status lines (re-render with the current language)
    SetHostStatus(SDK_TR("host.ready"), true);
    RefreshPermissionState();

    // prompt texts per field
    m_remoteIpEdit->SetAttribute("prompt_text", SDK_TR("client.ip"));
    m_remotePortEdit->SetAttribute("prompt_text", SDK_TR("client.port"));
    m_remotePwdEdit->SetAttribute("prompt_text", SDK_TR("client.password"));
    m_hostPwdEdit->SetAttribute("prompt_text", SDK_TR("host.password"));
    if (m_pwdShowBtn != nullptr) {
        m_pwdShowBtn->SetToolTipText(
            SDK_TR(m_pwdVisible ? "host.hidePassword" : "host.showPassword"));
    }
    // rows/hints that embed translated text are rebuilt in the new language
    UpdateManageButton();
    SetLanDevices(m_lastLanDevices);
    RebuildTransferList();

    InvalidateAll();
}

void MainWindow::OnThemeChanged()
{
    ApplyTheme();
}

void MainWindow::OnLanguageChanged()
{
    ApplyLanguage();
}

LRESULT MainWindow::OnPaintMsg(const ui::UiRect& rcPaint, const ui::NativeMsg& nativeMsg,
                               bool& bHandled)
{
    LRESULT result = BaseClass::OnPaintMsg(rcPaint, nativeMsg, bHandled);

    // Developer hook: dump the pixels + control tree of every open window into
    // STARDESK_UI_SHOT_DIR (used to verify the macOS UI without a screen
    // recording permission). The capture happens on a paint triggered after a
    // short delay so dialogs/second windows exist; optional test hooks:
    //   STARDESK_UI_SHOT_TAB=file   select the file tab first
    //   STARDESK_UI_SHOT_TYPE=1     type a password into the remote field first
    // developer test: close the remote view and connect again (crash repro)
    static bool sReconnectTestScheduled = false;
    if (!sReconnectTestScheduled && std::getenv("STARDESK_TEST_RECONNECT") != nullptr) {
        sReconnectTestScheduled = true;
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            PostToUI(ui::UiBind(this, [this]() {
                fprintf(stderr, "[test] step1: close remote window (ptr=%p)\n", (void*)m_remoteWin);
                if (m_remoteWin != nullptr) {
                    m_remoteWin->CloseWnd();
                }
            }));
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            PostToUI(ui::UiBind(this, [this]() {
                fprintf(stderr, "[test] step2: click connect, ptr=%p\n", (void*)m_remoteWin);
                OnConnectClicked();
            }));
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            PostToUI(ui::UiBind(this, [this]() {
                fprintf(stderr, "[test] step3: close remote again, ptr=%p\n", (void*)m_remoteWin);
                if (m_remoteWin != nullptr) {
                    m_remoteWin->CloseWnd();
                }
            }));
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            fprintf(stderr, "[test] reconnect test finished without crash\n");
            fflush(stderr);
            std::_Exit(0);
        }).detach();
    }

    static bool sCaptured = false;
    static bool sKickScheduled = false;
    static bool sKickDone = false;
    const char* shotDir = std::getenv("STARDESK_UI_SHOT_DIR");
    if (shotDir == nullptr) {
        return result;
    }
    if (!sKickScheduled) {
        sKickScheduled = true;
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(3200));
            PostToUI(ui::UiBind(this, [this]() {
                if (std::getenv("STARDESK_UI_SHOT_DARK") != nullptr) {
                    App::Instance().SetTheme(true);
                }
                if (const char* shotLang = std::getenv("STARDESK_UI_SHOT_LANG")) {
                    App::Instance().SetLanguage((Lang)std::atoi(shotLang));
                }
                if (std::getenv("STARDESK_UI_SHOT_TAB") != nullptr) {
                    const std::string tab = std::getenv("STARDESK_UI_SHOT_TAB");
                    if (tab == "file") {
                        SelectTab(false);
                    }
                }
                const char* shotType = std::getenv("STARDESK_UI_SHOT_TYPE");
                if (shotType != nullptr && m_remotePwdEdit != nullptr) {
                    m_remotePwdEdit->SetText("");
                    m_remotePwdEdit->SetFocus();
                    if (std::string(shotType) == "char") {
                        // exercise the real per-keystroke path instead of SetText
                        const std::string typed = "87654321";
                        for (const char ch : typed) {
                            ui::EventArgs charEvent;
                            charEvent.eventType = ui::kEventChar;
                            charEvent.wParam = (WPARAM)ch;
                            m_remotePwdEdit->HandleEvent(charEvent);
                        }
                    }
                    else {
                        m_remotePwdEdit->InsertText(0, "87654321");
                        m_remotePwdEdit->SetSel(8, 8); // collapse the caret at the end
                    }
                    int32_t selStart = 0;
                    int32_t selEnd = 0;
                    m_remotePwdEdit->GetSel(selStart, selEnd);
                    fprintf(stderr, "[shot] typed password len=%zu sel=%d,%d\n",
                            ui::StringConvert::TToUTF8(m_remotePwdEdit->GetText()).size(),
                            selStart, selEnd);
                    fflush(stderr);
                }
                sKickDone = true;
                InvalidateAll();
            }));
        }).detach();
        return result;
    }
    if (sCaptured || !sKickDone) {
        return result;
    }
    sCaptured = true;
    std::vector<ui::WindowPtr> windows =
        ui::GlobalManager::Instance().Windows().GetAllWindowList();
    int index = 0;
    for (const ui::WindowPtr& pWindow : windows) {
        if (pWindow.get() == nullptr) {
            continue;
        }
        const std::string base = std::string(shotDir) + "/win" + std::to_string(index);
        DumpWindowTreeToFile(pWindow.get(), base + ".txt");
        const bool captured = CaptureWindowPixels(pWindow.get(), base + ".raw");
        fprintf(stderr, "[shot] window %d (%s) pixels=%s\n", index,
                pWindow->GetWindowId().c_str(), captured ? "ok" : "failed");
        ++index;
    }
    fflush(stderr);
    std::_Exit(0);
    return result;
}

} // namespace sdk
