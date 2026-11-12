#include "FileSendWindow.h"

#include "../app/Language.h"
#include "../app/Theme.h"

#include "dui/Utils/FileDialog.h"

namespace sdk {

using ui::UiRect;
using ui::UiRectF;

FileSendWindow::FileSendWindow(const std::string& peerHost, uint16_t peerFilePort,
                               const uint8_t token[32])
    : m_peerHost(peerHost), m_peerFilePort(peerFilePort)
{
    std::memcpy(m_token, token, 32);
    // keep the classic "save to the receive folder" behavior now that an
    // empty target dir means the user's home (file browser)
    m_targetDir = ui::StringConvert::TToUTF8(
        App::Instance().Config().receiveDir.empty()
            ? AppConfig::GetDefaultReceiveDir().ToString()
            : App::Instance().Config().receiveDir);
}

FileSendWindow::~FileSendWindow()
{
    App::Instance().RemoveListener(this);
}

std::string FileSendWindow::GetSkinFolder() { return ""; }
std::string FileSendWindow::GetSkinFile() { return ""; }

void FileSendWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 560;
    attrs.m_szInitSize.cy = 460;
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
    attrs.m_rcCaption = UiRect(0, 0, 0, 32);
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

void FileSendWindow::OnInitWindow()
{
    // pure-code windows must apply the caption/size-box rects themselves
    // (XML mode does this in WindowBuilder; attrs.m_rcCaption is not applied)
    SetSizeBox(ui::UiRect(4, 4, 4, 4), true);
    SetCaptionRect(ui::UiRect(0, 0, 0, 32), true);
    // system shadow (like shadow_type="system_round" in XML)
    SetShadowType(ui::Shadow::ShadowType::kShadowSystemRound);

    const Palette& p = Theme::Get();

    ui::VBox* root = new ui::VBox(this);
    root->SetBkColor(p.windowBg);
    root->SetAttribute("width", "stretch");
    root->SetAttribute("height", "stretch");

    // caption
    ui::HBox* caption = new ui::HBox(this);
    // macOS self-drawn traffic lights (see RemoteWindow)
    caption->SetAttribute("name", "window_caption_bar");
    caption->SetAttribute("height", "32");
    caption->SetBkColor(p.captionBg);
    root->AddItem(caption);

    ThemeLabel* title = new ThemeLabel(this);
    title->SetRole(ThemeLabel::Role::Main);
    title->SetText(SDK_TR("file.title"));
    title->SetAttribute("font", "system_bold_12");
    title->SetAttribute("width", "auto");
    title->SetAttribute("margin", "12,0,0,0");
    caption->AddItem(title);

    ui::Control* spacer = new ui::Control(this);
    spacer->SetMouseEnabled(false);
    caption->AddItem(spacer);

    IconButton* closeBtn = new IconButton(this);
    closeBtn->SetIcon(VectorArt::Icon::Close);
    closeBtn->SetIconToolTip(SDK_TR("window.close"));
    closeBtn->SetName("closebtn");
    closeBtn->SetAttribute("width", "28");
    closeBtn->SetAttribute("height", "26");
    closeBtn->SetAttribute("margin", "0,3,4,3");
    caption->AddItem(closeBtn);

    // body
    ui::VBox* body = new ui::VBox(this);
    body->SetAttribute("padding", "14,12,14,12");
    body->SetAttribute("child_margin_y", "8");
    root->AddItem(body);

    // drop zone
    m_dropCard = new CardBox(this);
    m_dropCard->SetAttribute("height", "72");
    m_dropCard->SetAttribute("padding", "10,10,10,10");
    m_dropCard->AttachDropData([this](const ui::EventArgs& args) {
        OnFileDropped(args);
        return true;
    });
    body->AddItem(m_dropCard);

    m_dropHint = new ThemeLabel(this);
    m_dropHint->SetRole(ThemeLabel::Role::Sub);
    m_dropHint->SetText(SDK_TR("file.dragHint"));
    m_dropHint->SetAttribute("width", "stretch");
    m_dropHint->SetAttribute("text_align", "hcenter,vcenter");
    m_dropCard->AddItem(m_dropHint);

    // buttons row
    ui::HBox* btnRow = new ui::HBox(this);
    btnRow->SetAttribute("child_margin_x", "8");
    body->AddItem(btnRow);

    PillButton* browseBtn = new PillButton(this);
    browseBtn->SetText(SDK_TR("file.addFiles"));
    browseBtn->SetAttribute("width", "96");
    browseBtn->SetAttribute("height", "28");
    browseBtn->AttachClick([this](const ui::EventArgs&) {
        OnBrowse();
        return true;
    });
    btnRow->AddItem(browseBtn);

    m_sendBtn = new AccentButton(this);
    m_sendBtn->SetText(SDK_TR("file.send"));
    m_sendBtn->SetAttribute("width", "96");
    m_sendBtn->SetAttribute("height", "28");
    m_sendBtn->AttachClick([this](const ui::EventArgs&) {
        OnSend();
        return true;
    });
    btnRow->AddItem(m_sendBtn);

    ui::Control* spacer2 = new ui::Control(this);
    spacer2->SetMouseEnabled(false);
    btnRow->AddItem(spacer2);

    // list
    m_list = new ui::VBox(this);
    m_list->SetAttribute("child_margin_y", "6");
    body->AddItem(m_list);

    m_emptyLabel = new ThemeLabel(this);
    m_emptyLabel->SetRole(ThemeLabel::Role::Sub);
    m_emptyLabel->SetText(SDK_TR("file.empty"));
    m_emptyLabel->SetAttribute("height", "24");
    m_list->AddItem(m_emptyLabel);

    AttachBox(root);
    App::Instance().AddListener(this);

    BaseClass::OnInitWindow();
}

// ---------------------------------------------------------------- file list

void FileSendWindow::AddFiles(const std::vector<ui::FilePath>& files)
{
    bool added = false;
    for (const ui::FilePath& f : files) {
        if (f.IsExistsFile()) {
            m_files.push_back(f);
            added = true;
        }
    }
    if (added) {
        RebuildRows();
    }
}

void FileSendWindow::OnFileDropped(const ui::EventArgs& args)
{
    std::vector<ui::FilePath> files;
#ifdef DUI_BUILD_FOR_WIN
    if (args.wParam == ui::kControlDropTypeWindows) {
        const ui::ControlDropData_Windows* data =
            (const ui::ControlDropData_Windows*)args.lParam;
        if (data != nullptr) {
            for (const std::string& path : data->m_fileList) {
                files.emplace_back(ui::StringConvert::TToUTF8(path));
            }
        }
    }
#else
    if (args.wParam == ui::kControlDropTypeWayland) {
        // The native backends (macOS/Linux) deliver the same drop-data structure
        // the Wayland backend defines; SDL's drop data is gone with the SDL backend.
        const ui::ControlDropData_Wayland* data = (const ui::ControlDropData_Wayland*)args.lParam;
        if (data != nullptr && !data->m_bTextData) {
            for (const std::string& path : data->m_fileList) {
                files.emplace_back(ui::StringConvert::TToUTF8(path));
            }
        }
    }
#endif
    AddFiles(files);
}

void FileSendWindow::OnBrowse()
{
    std::vector<ui::FilePath> files;
    ui::FileDialog dlg;
    if (dlg.BrowseForFiles(this, files)) {
        AddFiles(files);
    }
}

void FileSendWindow::ClearRows()
{
    m_list->RemoveAllItems();
    m_rows.clear();
}

void FileSendWindow::RebuildRows()
{
    ClearRows();
    if (m_files.empty()) {
        m_emptyLabel = new ThemeLabel(this);
        m_emptyLabel->SetRole(ThemeLabel::Role::Sub);
        m_emptyLabel->SetText(SDK_TR("file.empty"));
        m_emptyLabel->SetAttribute("height", "24");
        m_list->AddItem(m_emptyLabel);
        return;
    }
    for (const ui::FilePath& f : m_files) {
        Row row;
        row.name = ui::StringConvert::UTF8ToT(f.GetFileName());
        row.size = f.GetFileSize();

        row.card = new CardBox(this);
        row.card->SetAttribute("height", "46");
        row.card->SetAttribute("padding", "10,6,10,6");
        m_list->AddItem(row.card);

        ui::VBox* info = new ui::VBox(this);
        info->SetAttribute("child_margin_y", "2");
        row.card->AddItem(info);

        ThemeLabel* nameLabel = new ThemeLabel(this);
        nameLabel->SetRole(ThemeLabel::Role::Main);
        nameLabel->SetText(row.name);
        nameLabel->SetAttribute("width", "stretch");
        nameLabel->SetAttribute("font", "system_12");
        info->AddItem(nameLabel);

        row.statusLabel = new ThemeLabel(this);
        row.statusLabel->SetRole(ThemeLabel::Role::Sub);
        row.statusLabel->SetText(SDK_TR("file.waiting"));
        row.statusLabel->SetAttribute("width", "stretch");
        info->AddItem(row.statusLabel);

        row.progress = new ProgressBar(this);
        row.progress->SetAttribute("width", "120");
        row.progress->SetAttribute("height", "8");
        row.card->AddItem(row.progress);

        row.cancelBtn = new IconButton(this);
        row.cancelBtn->SetIcon(VectorArt::Icon::Cross);
        row.cancelBtn->SetIconToolTip(SDK_TR("file.cancel"));
        row.cancelBtn->SetAttribute("width", "24");
        row.cancelBtn->SetAttribute("height", "24");
        row.card->AddItem(row.cancelBtn);

        m_rows.push_back(row);
    }
}

void FileSendWindow::OnSend()
{
    if (m_files.empty()) {
        return;
    }
    std::vector<ui::FilePath> files = m_files;
    std::string err;
    const bool ok = App::Instance().FileTx().SendFiles(
        m_peerHost, m_peerFilePort, m_token, files, m_targetDir,
        [this](const FileTransfer::Item& item) {
            PostToUI(ui::UiBind(this, [this, item]() { OnItemUpdated(item.id, item); }));
        },
        err);
    if (!ok) {
        // busy or failed: mark rows
        for (auto& row : m_rows) {
            if (row.statusLabel) {
                row.statusLabel->SetRole(ThemeLabel::Role::Danger);
                row.statusLabel->SetText(SDK_TR("file.busy"));
            }
        }
    }
}

void FileSendWindow::OnItemUpdated(uint32_t id, const FileTransfer::Item& item)
{
    m_items[id] = item;
    // find the row by file name (send rows are created from m_files)
    for (auto& row : m_rows) {
        if (row.name != ui::StringConvert::UTF8ToT(item.name)) {
            continue;
        }
        row.id = id;
        if (row.progress) {
            row.progress->SetProgress(item.size > 0 ? (double)item.transferred / item.size : 0.0);
        }
        if (row.statusLabel) {
            ThemeLabel::Role role = ThemeLabel::Role::Sub;
            std::string text;
            switch (item.state) {
            case FileTransfer::ItemState::Transferring:
                text = ui::StringUtil::Printf("%s %llu / %llu KB",
                                              SDK_TR("file.transferring").c_str(),
                                              (unsigned long long)(item.transferred / 1024),
                                              (unsigned long long)(item.size / 1024));
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
                role = ThemeLabel::Role::Sub;
                text = SDK_TR("file.cancelled");
                break;
            default:
                text = SDK_TR("file.waiting");
                break;
            }
            row.statusLabel->SetRole(role);
            row.statusLabel->SetText(text);
        }
        break;
    }
}

// ---------------------------------------------------------------- theme / language

void FileSendWindow::ApplyTheme()
{
    const Palette& p = Theme::Get();
    ui::VBox* root = dynamic_cast<ui::VBox*>(GetRoot());
    if (root != nullptr) {
        root->SetBkColor(p.windowBg);
    }
    InvalidateAll();
}

void FileSendWindow::ApplyLanguage()
{
    m_dropHint->SetText(SDK_TR("file.dragHint"));
    m_sendBtn->SetText(SDK_TR("file.send"));
    for (auto& row : m_rows) {
        if (row.statusLabel) {
            const auto it = m_items.find(row.id);
            if (it == m_items.end() || it->second.state == FileTransfer::ItemState::Waiting) {
                row.statusLabel->SetText(SDK_TR("file.waiting"));
            }
        }
    }
    InvalidateAll();
}

} // namespace sdk
