#include "InviteWindow.h"

#include "../app/AppConfig.h"
#include "../app/Language.h"
#include "../app/Theme.h"

namespace sdk {

InviteWindow::InviteWindow(const std::string& deviceName, const std::string& ip, bool view,
                           ResultCb cb)
    : m_deviceName(deviceName), m_ip(ip), m_view(view), m_cb(std::move(cb))
{
}

InviteWindow::~InviteWindow()
{
    App::Instance().RemoveListener(this);
    if (!m_done && m_cb) {
        m_cb(false);
    }
}

std::string InviteWindow::GetSkinFolder()
{
    return "";
}

std::string InviteWindow::GetSkinFile()
{
    return "";
}

void InviteWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 420;
    attrs.m_szInitSize.cy = 210;
    attrs.m_bShadowAttached = true;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = false;
    attrs.m_bIsLayeredWindowDefined = true;
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 32);
    attrs.m_bCaptionDefined = true;
    ui::UiSize szBorderRound;
    ui::UiPadding rcShadowCorner;
    std::string shadowImage;
    ui::Shadow::GetShadowParam(ui::Shadow::ShadowType::kShadowSystemRound, szBorderRound,
                               rcShadowCorner, shadowImage);
    attrs.m_rcShadowCorner = rcShadowCorner;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void InviteWindow::OnInitWindow()
{
    SetSizeBox(ui::UiRect(4, 4, 4, 4), true);
    SetCaptionRect(ui::UiRect(0, 0, 0, 32), true);
    SetShadowType(ui::Shadow::ShadowType::kShadowSystemRound);

    const Palette& p = Theme::Get();

    ui::VBox* root = new ui::VBox(this);
    root->SetBkColor(p.windowBg);
    root->SetAttribute("width", "stretch");
    root->SetAttribute("height", "stretch");

    ui::HBox* caption = new ui::HBox(this);
    m_caption = caption;
    caption->SetAttribute("name", "window_caption_bar");
    caption->SetAttribute("height", "32");
    caption->SetBkColor(p.captionBg);
    root->AddItem(caption);

    ThemeLabel* title = new ThemeLabel(this);
    title->SetRole(ThemeLabel::Role::Main);
    m_lang.SetText(title, "invite.title");
    title->SetAttribute("font", "system_bold_12");
    title->SetAttribute("width", "auto");
    title->SetAttribute("height", "stretch");
    title->SetAttribute("margin", "12,0,0,0");
    caption->AddItem(title);

    ui::Control* spacer = new ui::Control(this);
    spacer->SetMouseEnabled(false);
    caption->AddItem(spacer);

    IconButton* closeBtn = new IconButton(this);
    closeBtn->SetIcon(VectorArt::Icon::Close);
    m_lang.BindToolTip(closeBtn, "window.close");
    closeBtn->SetName("closebtn");
    closeBtn->SetAttribute("width", "28");
    closeBtn->SetAttribute("height", "28");
    closeBtn->SetAttribute("margin", "0,3,4,3");
    caption->AddItem(closeBtn);

    ui::VBox* body = new ui::VBox(this);
    body->SetAttribute("padding", "16,14,16,14");
    body->SetAttribute("child_margin_y", "12");
    root->AddItem(body);

    m_textLabel = new ThemeLabel(this);
    m_textLabel->SetRole(ThemeLabel::Role::Main);
    m_textLabel->SetAttribute("text_align", "left,top");
    m_textLabel->SetAttribute("multi_line", "true");
    m_textLabel->SetAttribute("width", "stretch");
    m_textLabel->SetAttribute("height", "auto");
    body->AddItem(m_textLabel);

    ui::HBox* btnRow = new ui::HBox(this);
    btnRow->SetAttribute("height", "40");
    btnRow->SetAttribute("child_halign", "right");
    btnRow->SetAttribute("child_margin_x", "8");
    body->AddItem(btnRow);

    PillButton* rejectBtn = new PillButton(this);
    m_lang.SetText(rejectBtn, "invite.reject");
    rejectBtn->SetAttribute("width", "90");
    rejectBtn->SetAttribute("height", "34");
    rejectBtn->AttachClick([this](const ui::EventArgs&) {
        if (!m_done && m_cb) m_cb(false);
        m_done = true;
        CloseWnd();
        return true;
    });
    btnRow->AddItem(rejectBtn);

    AccentButton* acceptBtn = new AccentButton(this);
    m_lang.SetText(acceptBtn, "invite.accept");
    acceptBtn->SetAttribute("width", "110");
    acceptBtn->SetAttribute("height", "34");
    acceptBtn->AttachClick([this](const ui::EventArgs&) {
        if (!m_done && m_cb) m_cb(true);
        m_done = true;
        CloseWnd();
        return true;
    });
    btnRow->AddItem(acceptBtn);

    AttachBox(root);
    App::Instance().AddListener(this);
    ApplyLanguage();
    BaseClass::OnInitWindow();
}

void InviteWindow::ApplyTheme()
{
    const Palette& p = Theme::Get();
    if (m_caption != nullptr) {
        m_caption->SetBkColor(p.captionBg);
    }
    ui::VBox* root = dynamic_cast<ui::VBox*>(GetRoot());
    if (root != nullptr) {
        root->SetBkColor(p.windowBg);
    }
    InvalidateAll();
}

void InviteWindow::ApplyLanguage()
{
    m_lang.Apply();
    if (m_textLabel == nullptr) {
        return;
    }
    const std::string fmt = ui::StringConvert::TToUTF8(SDK_TR("invite.text"));
    const std::string name = ui::StringConvert::TToUTF8(m_deviceName);
    const std::string ip = ui::StringConvert::TToUTF8(m_ip);
    const std::string text = ui::StringUtil::Printf(fmt.c_str(), name.c_str(), ip.c_str());
    m_textLabel->SetText(ui::StringConvert::UTF8ToT(text));
    InvalidateAll();
}

} // namespace sdk
