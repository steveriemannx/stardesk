#include "AddDeviceWindow.h"

#include "../app/Language.h"
#include "../app/Theme.h"

namespace sdk {

namespace {

ui::RichEdit* MakeEdit(ui::Window* pWindow, LangBinder& lang, const char* promptKey)
{
    ui::RichEdit* edit = new ui::RichEdit(pWindow);
    lang.BindPrompt(edit, promptKey);
    edit->SetAttribute("single_line", "true");
    edit->SetAttribute("font", "system_12");
    edit->SetAttribute("text_align", "left,vcenter");
    edit->SetAttribute("text_color", "sd_text_main");
    edit->SetAttribute("normal_text_color", "sd_text_main");
    edit->SetAttribute("prompt_color", "sd_text_sub");
    edit->SetAttribute("border_size", "1");
    edit->SetAttribute("border_round", "4,4");
    edit->SetAttribute("padding", "10,0,10,0");
    edit->SetBkColor(Theme::Get().inputBg);
    edit->SetAttribute("height", "36");
    edit->SetAttribute("width", "stretch");
    return edit;
}

} // namespace

AddDeviceWindow::AddDeviceWindow(SaveCb cb)
    : m_cb(std::move(cb))
{
}

std::string AddDeviceWindow::GetSkinFolder()
{
    return "";
}

std::string AddDeviceWindow::GetSkinFile()
{
    return "";
}

void AddDeviceWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 420;
    attrs.m_szInitSize.cy = 470;
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

void AddDeviceWindow::OnInitWindow()
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
    m_lang.SetText(title, "client.addDeviceTitle");
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
    body->SetAttribute("child_margin_y", "6");
    root->AddItem(body);

    auto addField = [this, body](const char* labelKey, ui::RichEdit*& edit, const char* value) {
        ThemeLabel* label = new ThemeLabel(this);
        label->SetRole(ThemeLabel::Role::Sub);
        label->SetAttribute("height", "18");
        label->SetAttribute("width", "stretch");
        m_lang.SetText(label, labelKey);
        body->AddItem(label);
        edit = MakeEdit(this, m_lang, labelKey);
        edit->SetAttribute("prompt_text", "");
        if (value != nullptr) {
            edit->SetText(value);
        }
        body->AddItem(edit);
    };
    addField("client.deviceName", m_nameEdit, nullptr);
    addField("client.deviceAddress", m_ipEdit, nullptr);
    addField("client.port", m_portEdit, "7456");

    m_errorLabel = new ThemeLabel(this);
    m_errorLabel->SetRole(ThemeLabel::Role::Danger);
    m_errorLabel->SetAttribute("width", "stretch");
    m_errorLabel->SetAttribute("height", "24");
    body->AddItem(m_errorLabel);

    ui::HBox* btnRow = new ui::HBox(this);
    btnRow->SetAttribute("height", "44");
    btnRow->SetAttribute("child_halign", "right");
    btnRow->SetAttribute("child_margin_x", "8");
    body->AddItem(btnRow);

    PillButton* cancelBtn = new PillButton(this);
    m_lang.SetText(cancelBtn, "file.cancel");
    cancelBtn->SetAttribute("width", "80");
    cancelBtn->SetAttribute("height", "34");
    cancelBtn->AttachClick([this](const ui::EventArgs&) {
        CloseWnd();
        return true;
    });
    btnRow->AddItem(cancelBtn);

    AccentButton* saveBtn = new AccentButton(this);
    m_lang.SetText(saveBtn, "common.ok");
    saveBtn->SetAttribute("width", "80");
    saveBtn->SetAttribute("height", "34");
    saveBtn->AttachClick([this](const ui::EventArgs&) {
        DoSave();
        return true;
    });
    btnRow->AddItem(saveBtn);

    AttachBox(root);
    ApplyTheme();
    BaseClass::OnInitWindow();
}

void AddDeviceWindow::DoSave()
{
    const std::string ip = ui::StringConvert::TToUTF8(m_ipEdit->GetText());
    const int port = std::atoi(ui::StringConvert::TToUTF8(m_portEdit->GetText()).c_str());
    if (ip.empty()) {
        m_errorLabel->SetText(SDK_TR("client.deviceAddressRequired"));
        return;
    }
    if (port <= 0 || port > 65535) {
        m_errorLabel->SetText(SDK_TR("client.devicePortInvalid"));
        return;
    }
    const std::string name = ui::StringConvert::TToUTF8(m_nameEdit->GetText());
    if (m_cb) {
        m_cb(name, ip, port);
    }
    CloseWnd();
}

void AddDeviceWindow::ApplyTheme()
{
    const Palette& p = Theme::Get();
    if (m_caption != nullptr) {
        m_caption->SetBkColor(p.captionBg);
    }
    ui::VBox* root = dynamic_cast<ui::VBox*>(GetRoot());
    if (root != nullptr) {
        root->SetBkColor(p.windowBg);
    }
    for (ui::Control* edit : { (ui::Control*)m_nameEdit, (ui::Control*)m_ipEdit,
                               (ui::Control*)m_portEdit }) {
        if (edit != nullptr) {
            edit->SetBkColor(p.inputBg);
        }
    }
    InvalidateAll();
}

} // namespace sdk
