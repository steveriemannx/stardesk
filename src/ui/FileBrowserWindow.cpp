#include "FileBrowserWindow.h"

#include "../app/Language.h"
#include "../app/Theme.h"

#include "dui/Utils/FileDialog.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sdk {

using ui::UiRect;
using ui::UiRectF;

// Finder-like folder/file glyph, drawn with theme colors (no image files).
class FileIconControl : public ui::Control
{
    typedef ui::Control BaseClass;
public:
    explicit FileIconControl(ui::Window* w) : ui::Control(w) {}
    void SetFolder(bool folder) { m_folder = folder; Invalidate(); }
    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override
    {
        ui::IRenderFactory* factory = ui::GlobalManager::Instance().GetRenderFactory();
        const Palette& p = Theme::Get();
        const UiRect rc = GetRect();
        const float x0 = (float)rc.left + 2, y0 = (float)rc.top + 3;
        const float w = (float)rc.Width() - 4, h = (float)rc.Height() - 6;
        if (m_folder) {
            // body + tab in the StarDesk brand color (accent)
            const float tabH = h * 0.28f;
            std::unique_ptr<ui::IPath> body(factory->CreatePath());
            body->AddRect(UiRectF(x0, y0 + tabH * 0.5f, x0 + w, y0 + h));
            body->AddRect(UiRectF(x0 + w * 0.08f, y0, x0 + w * 0.45f, y0 + tabH * 0.5f));
            std::unique_ptr<ui::IBrush> brush(factory->CreateBrush(p.accent));
            pRender->FillPath(body.get(), brush.get());
            // spine line for depth (window bg over the accent)
            std::unique_ptr<ui::IPen> pen(factory->CreatePen(p.windowBg, 1.0f));
            pRender->DrawLine(ui::UiPointF(x0 + w * 0.55f, y0 + tabH * 0.5f),
                              ui::UiPointF(x0 + w * 0.55f, y0 + h), pen.get());
        }
        else {
            // page with a folded corner, neutral
            const float fold = h * 0.42f;
            std::unique_ptr<ui::IPath> page(factory->CreatePath());
            page->AddRect(UiRectF(x0, y0, x0 + w - fold, y0 + h));
            std::unique_ptr<ui::IBrush> brush(factory->CreateBrush(p.panelBg));
            pRender->FillPath(page.get(), brush.get());
            std::unique_ptr<ui::IPen> pen(factory->CreatePen(p.textSub, 1.0f));
            pRender->DrawRect(UiRectF(x0, y0, x0 + w - fold, y0 + h), p.textSub, 1.0f);
            pRender->DrawLine(ui::UiPointF(x0 + w - fold, y0), ui::UiPointF(x0 + w - fold, y0 + fold), pen.get());
            pRender->DrawLine(ui::UiPointF(x0 + w - fold, y0 + fold), ui::UiPointF(x0 + w, y0 + fold), pen.get());
            // two text lines
            pRender->DrawLine(ui::UiPointF(x0 + w * 0.14f, y0 + h * 0.38f),
                              ui::UiPointF(x0 + w * 0.86f, y0 + h * 0.38f), pen.get());
            pRender->DrawLine(ui::UiPointF(x0 + w * 0.14f, y0 + h * 0.62f),
                              ui::UiPointF(x0 + w * 0.60f, y0 + h * 0.62f), pen.get());
        }
        BaseClass::Paint(pRender, rcPaint);
    }
private:
    bool m_folder = true;
};

FileBrowserWindow::FileBrowserWindow(ClientSession* session)
    : m_sessionRaw(session)
{
}

FileBrowserWindow::~FileBrowserWindow()
{
    App::Instance().RemoveListener(this);
}

std::string FileBrowserWindow::GetSkinFolder() { return ""; }
std::string FileBrowserWindow::GetSkinFile() { return ""; }

void FileBrowserWindow::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 960;
    attrs.m_szInitSize.cy = 640;
    attrs.m_bShadowAttached = true;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = false;
    attrs.m_bIsLayeredWindowDefined = true;
    attrs.m_rcSizeBox = UiRect(4, 4, 4, 4);
    attrs.m_bSizeBoxDefined = true;
    attrs.m_rcCaption = UiRect(0, 0, 0, 32);
    attrs.m_bCaptionDefined = true;
    ui::Shadow::ShadowType nShadowType = ui::Shadow::ShadowType::kShadowSystemRound;
    ui::UiSize szBorderRound;
    ui::UiPadding rcShadowCorner;
    std::string shadowImage;
    ui::Shadow::GetShadowParam(nShadowType, szBorderRound, rcShadowCorner, shadowImage);
    attrs.m_rcShadowCorner = rcShadowCorner;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void FileBrowserWindow::OnInitWindow()
{
    SetSizeBox(ui::UiRect(4, 4, 4, 4), true);
    SetCaptionRect(ui::UiRect(0, 0, 0, 32), true);
    SetShadowType(ui::Shadow::ShadowType::kShadowSystemRound);

    const Palette& p = Theme::Get();

    m_root = new ui::VBox(this);
    m_root->SetBkColor(p.windowBg);
    m_root->SetAttribute("width", "stretch");
    m_root->SetAttribute("height", "stretch");

    // caption
    ui::HBox* caption = new ui::HBox(this);
    caption->SetAttribute("height", "32");
    caption->SetBkColor(p.captionBg);
    m_root->AddItem(caption);

    ThemeLabel* title = new ThemeLabel(this);
    title->SetRole(ThemeLabel::Role::Main);
    title->SetText(SDK_TR("filebrowser.title"));
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

    // drag & drop anywhere on the window uploads to the remote folder
    m_root->AttachDropData([this](const ui::EventArgs& args) {
        OnFileDropped(args);
        return true;
    });

    // body
    m_body = new ui::VBox(this);
    ui::VBox* body = m_body;
    body->SetAttribute("width", "stretch");
    body->SetAttribute("height", "stretch");
    body->SetAttribute("padding", "14,12,14,12");
    body->SetAttribute("child_margin_y", "8");
    m_root->AddItem(body);

    // status line
    m_statusLabel = new ThemeLabel(this);
    m_statusLabel->SetRole(ThemeLabel::Role::Sub);
    m_statusLabel->SetText("");
    m_statusLabel->SetAttribute("height", "18");
    body->AddItem(m_statusLabel);

    // two panes
    m_panes = new ui::HBox(this);
    ui::HBox* panes = m_panes;
    panes->SetAttribute("child_margin_x", "8");
    panes->SetAttribute("width", "stretch");
    panes->SetAttribute("height", "stretch");
    body->AddItem(panes);

    // ---- remote pane ----
    m_remoteCol = new ui::VBox(this);
    ui::VBox* remoteCol = m_remoteCol;
    remoteCol->SetAttribute("width", "50%");
    remoteCol->SetAttribute("height", "stretch");
    panes->AddItem(remoteCol);

    m_remoteHead = new ui::HBox(this);
    ui::HBox* remoteHead = m_remoteHead;
    remoteHead->SetAttribute("child_margin_x", "6");
    remoteHead->SetAttribute("height", "24");
    remoteCol->AddItem(remoteHead);

    ThemeLabel* remoteTitle = new ThemeLabel(this);
    remoteTitle->SetRole(ThemeLabel::Role::Main);
    remoteTitle->SetText(SDK_TR("filebrowser.remote"));
    remoteTitle->SetAttribute("font", "system_bold_12");
    remoteTitle->SetAttribute("width", "auto");
    remoteHead->AddItem(remoteTitle);

    IconButton* remoteUp = new IconButton(this);
    remoteUp->SetIcon(VectorArt::Icon::Folder);
    remoteUp->SetIconToolTip(SDK_TR("filebrowser.up"));
    remoteUp->SetAttribute("width", "24");
    remoteUp->SetAttribute("height", "24");
    remoteUp->AttachClick([this](const ui::EventArgs&) {
        GoRemoteUp();
        return true;
    });
    remoteHead->AddItem(remoteUp);

    IconButton* remoteRefresh = new IconButton(this);
    remoteRefresh->SetIcon(VectorArt::Icon::Refresh);
    remoteRefresh->SetIconToolTip(SDK_TR("filebrowser.refresh"));
    remoteRefresh->SetAttribute("width", "24");
    remoteRefresh->SetAttribute("height", "24");
    remoteRefresh->AttachClick([this](const ui::EventArgs&) {
        RefreshRemote();
        return true;
    });
    remoteHead->AddItem(remoteRefresh);

    m_remotePathLabel = new ThemeLabel(this);
    m_remotePathLabel->SetRole(ThemeLabel::Role::Sub);
    m_remotePathLabel->SetAttribute("width", "stretch");
    m_remotePathLabel->SetAttribute("height", "24");
    m_remotePathLabel->SetAttribute("valign", "center");
    m_remotePathLabel->SetAttribute("text_align", "right,vcenter");
    m_remotePathLabel->SetText("...");
    remoteHead->AddItem(m_remotePathLabel);

    m_remotePane = new CardBox(this);
    m_remotePane->SetAttribute("width", "stretch");
    m_remotePane->SetAttribute("height", "stretch");
    m_remotePane->SetAttribute("padding", "8,8,8,8");
    remoteCol->AddItem(m_remotePane);

    m_remoteList = new ui::VScrollBox(this);
    m_remoteList->SetAttribute("vscrollbar", "true");
    m_remoteList->SetAttribute("width", "stretch");
    m_remoteList->SetAttribute("height", "stretch");
    m_remotePane->AddItem(m_remoteList);

    // column header (name | size), Finder-style
    ui::HBox* remoteHeader = new ui::HBox(this);
    remoteHeader->SetAttribute("height", "20");
    remoteHeader->SetAttribute("padding", "26,2,6,2");
    remoteHeader->SetBkColor(Theme::Get().panelBg);
    m_remotePane->AddItem(remoteHeader);

    ThemeLabel* remoteNameCol = new ThemeLabel(this);
    remoteNameCol->SetRole(ThemeLabel::Role::Sub);
    remoteNameCol->SetText(SDK_TR("file.name"));
    remoteNameCol->SetAttribute("width", "stretch");
    remoteNameCol->SetAttribute("text_align", "left,vcenter");
    remoteNameCol->SetAttribute("font", "system_10");
    remoteHeader->AddItem(remoteNameCol);

    ThemeLabel* remoteSizeCol = new ThemeLabel(this);
    remoteSizeCol->SetRole(ThemeLabel::Role::Sub);
    remoteSizeCol->SetText(SDK_TR("file.size"));
    remoteSizeCol->SetAttribute("width", "auto");
    remoteSizeCol->SetAttribute("text_align", "right,vcenter");
    remoteSizeCol->SetAttribute("font", "system_10");
    remoteSizeCol->SetAttribute("margin", "0,0,6,0");
    remoteHeader->AddItem(remoteSizeCol);

    // ---- local pane ----
    m_localCol = new ui::VBox(this);
    ui::VBox* localCol = m_localCol;
    localCol->SetAttribute("width", "50%");
    localCol->SetAttribute("height", "stretch");
    panes->AddItem(localCol);

    m_localHead = new ui::HBox(this);
    ui::HBox* localHead = m_localHead;
    localHead->SetAttribute("child_margin_x", "6");
    localHead->SetAttribute("height", "24");
    localCol->AddItem(localHead);

    ThemeLabel* localTitle = new ThemeLabel(this);
    localTitle->SetRole(ThemeLabel::Role::Main);
    localTitle->SetText(SDK_TR("filebrowser.local"));
    localTitle->SetAttribute("font", "system_bold_12");
    localTitle->SetAttribute("width", "auto");
    localHead->AddItem(localTitle);

    IconButton* localUp = new IconButton(this);
    localUp->SetIcon(VectorArt::Icon::Folder);
    localUp->SetIconToolTip(SDK_TR("filebrowser.up"));
    localUp->SetAttribute("width", "24");
    localUp->SetAttribute("height", "24");
    localUp->AttachClick([this](const ui::EventArgs&) {
        GoLocalUp();
        return true;
    });
    localHead->AddItem(localUp);

    IconButton* localRefresh = new IconButton(this);
    localRefresh->SetIcon(VectorArt::Icon::Refresh);
    localRefresh->SetIconToolTip(SDK_TR("filebrowser.refresh"));
    localRefresh->SetAttribute("width", "24");
    localRefresh->SetAttribute("height", "24");
    localRefresh->AttachClick([this](const ui::EventArgs&) {
        RefreshLocal();
        return true;
    });
    localHead->AddItem(localRefresh);

    m_localPathLabel = new ThemeLabel(this);
    m_localPathLabel->SetRole(ThemeLabel::Role::Sub);
    m_localPathLabel->SetAttribute("width", "stretch");
    m_localPathLabel->SetAttribute("height", "24");
    m_localPathLabel->SetAttribute("valign", "center");
    m_localPathLabel->SetAttribute("text_align", "right,vcenter");
    m_localPathLabel->SetText("...");
    localHead->AddItem(m_localPathLabel);

    m_localPane = new CardBox(this);
    m_localPane->SetAttribute("width", "stretch");
    m_localPane->SetAttribute("height", "stretch");
    m_localPane->SetAttribute("padding", "8,8,8,8");
    localCol->AddItem(m_localPane);

    m_localList = new ui::VScrollBox(this);
    m_localList->SetAttribute("vscrollbar", "true");
    m_localList->SetAttribute("width", "stretch");
    m_localList->SetAttribute("height", "stretch");
    m_localPane->AddItem(m_localList);

    // column header (name | size), Finder-style
    ui::HBox* localHeader = new ui::HBox(this);
    localHeader->SetAttribute("height", "20");
    localHeader->SetAttribute("padding", "26,2,6,2");
    localHeader->SetBkColor(Theme::Get().panelBg);
    m_localPane->AddItem(localHeader);

    ThemeLabel* localNameCol = new ThemeLabel(this);
    localNameCol->SetRole(ThemeLabel::Role::Sub);
    localNameCol->SetText(SDK_TR("file.name"));
    localNameCol->SetAttribute("width", "stretch");
    localNameCol->SetAttribute("text_align", "left,vcenter");
    localNameCol->SetAttribute("font", "system_10");
    localHeader->AddItem(localNameCol);

    ThemeLabel* localSizeCol = new ThemeLabel(this);
    localSizeCol->SetRole(ThemeLabel::Role::Sub);
    localSizeCol->SetText(SDK_TR("file.size"));
    localSizeCol->SetAttribute("width", "auto");
    localSizeCol->SetAttribute("text_align", "right,vcenter");
    localSizeCol->SetAttribute("font", "system_10");
    localSizeCol->SetAttribute("margin", "0,0,6,0");
    localHeader->AddItem(localSizeCol);

    // directory listing replies come in on the session thread -> post to UI
    if (m_sessionRaw) {
        m_sessionRaw->SetDirListCallback([this](bool ok, const std::string& path,
                                             const std::vector<FileEntry>& entries) {
            PostToUI(ui::UiBind(this, [this, ok, path, entries]() {
                m_remoteLoading = false;
                OnDirListResult(ok, path, entries);
            }));
        });
    }

    AttachBox(m_root);
    App::Instance().AddListener(this);

    // initial browse: local = home, remote = host home
    {
        std::error_code ec;
        const std::filesystem::path home =
            std::filesystem::path(std::getenv("HOME") != nullptr ? std::getenv("HOME") : "/");
        m_localDir = std::filesystem::exists(home, ec) ? home : std::filesystem::current_path(ec);
    }
    RefreshLocal();
    RefreshRemote();

    // test hook: STARDESK_AUTO_UPLOAD=<dir> auto-uploads a folder (folder
    // upload crash repro)
    if (const char* up = std::getenv("STARDESK_AUTO_UPLOAD")) {
        std::vector<ui::FilePath> files;
        files.emplace_back(ui::FilePath(ui::StringConvert::UTF8ToT(up)));
        PostToUI([this, files]() { UploadFiles(files); });
    }

    BaseClass::OnInitWindow();
}

void FileBrowserWindow::ApplyTheme()
{
    if (m_root != nullptr) {
        m_root->SetBkColor(Theme::Get().windowBg);
    }
}

void FileBrowserWindow::ApplyLanguage()
{
    if (m_statusLabel != nullptr) {
        m_statusLabel->SetText("");
    }
}

// ---------------------------------------------------------------- local

void FileBrowserWindow::RefreshLocal()
{
    std::error_code ec;
    std::vector<Entry> entries;
    std::filesystem::directory_iterator it(m_localDir, ec);
    if (!ec) {
        for (const auto& de : it) {
            std::error_code e2;
            const std::string name = de.path().filename().string();
            if (name.empty() || name[0] == '.') {
                continue;
            }
            Entry e;
            e.name = ui::StringConvert::UTF8ToT(name);
            e.isDir = de.is_directory(e2);
            if (!e.isDir) {
                e.size = (uint64_t)de.file_size(e2);
            }
            entries.push_back(std::move(e));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) {
            return a.isDir;
        }
        return a.name < b.name;
    });
    m_localEntries = std::move(entries);
    RenderLocal();
}

void FileBrowserWindow::EnterLocalDir(const std::string& name)
{
    std::error_code ec;
    const std::filesystem::path next = m_localDir / ui::StringConvert::TToUTF8(name);
    if (std::filesystem::is_directory(next, ec)) {
        m_localDir = next;
        RefreshLocal();
    }
}

void FileBrowserWindow::GoLocalUp()
{
    const std::filesystem::path parent = m_localDir.parent_path();
    if (!parent.empty() && parent != m_localDir) {
        m_localDir = parent;
        RefreshLocal();
    }
}

void FileBrowserWindow::RenderLocal()
{
    m_selectedRow = nullptr;
    ClearPane(m_localList);
    if (m_localPathLabel != nullptr) {
        m_localPathLabel->SetText(ui::StringConvert::UTF8ToT(m_localDir.string()));
    }
    if (m_localEntries.empty()) {
        ThemeLabel* empty = new ThemeLabel(this);
        empty->SetRole(ThemeLabel::Role::Sub);
        empty->SetText(SDK_TR("filebrowser.empty"));
        empty->SetAttribute("height", "24");
        m_localList->AddItem(empty);
        return;
    }
    for (const Entry& e : m_localEntries) {
        AddRow(m_localList, e, true);
    }
}

// ---------------------------------------------------------------- remote

void FileBrowserWindow::RefreshRemote()
{
    if (!m_sessionRaw || m_remoteLoading) {
        return;
    }
    m_remoteLoading = true;
    // a directory-list refresh must NOT show the "正在下载..." status (that
    // text is for real file downloads) - it would flash briefly on every
    // refresh (pane refresh button / right-click menu refresh)
    m_sessionRaw->RequestDirList(m_remotePath);
}

void FileBrowserWindow::EnterRemoteDir(const std::string& name)
{
    if (m_remotePath.empty()) {
        m_remotePath = name;
    }
    else {
        m_remotePath += "/" + name;
    }
    RefreshRemote();
}

void FileBrowserWindow::GoRemoteUp()
{
    const size_t slash = m_remotePath.rfind('/');
    m_remotePath = (slash == std::string::npos) ? std::string() : m_remotePath.substr(0, slash);
    RefreshRemote();
}

void FileBrowserWindow::OnDirListResult(bool ok, const std::string& path,
                                        const std::vector<FileEntry>& entries)
{
    if (m_statusLabel != nullptr) {
        m_statusLabel->SetText("");
    }
    if (!ok) {
        SetStatus(SDK_TR("client.networkError"), true);
        return;
    }
    m_remotePath = path;
    if (m_remotePathLabel != nullptr) {
        m_remotePathLabel->SetText(ui::StringConvert::UTF8ToT(
            m_remotePath.empty() ? "~" : m_remotePath));
    }
    m_remoteEntries.clear();
    for (const FileEntry& fe : entries) {
        Entry e;
        e.name = ui::StringConvert::UTF8ToT(fe.name);
        e.isDir = fe.isDir;
        e.size = fe.size;
        m_remoteEntries.push_back(std::move(e));
    }
    RenderRemote();
}

void FileBrowserWindow::RenderRemote()
{
    m_selectedRow = nullptr;
    ClearPane(m_remoteList);
    if (m_remoteEntries.empty()) {
        ThemeLabel* empty = new ThemeLabel(this);
        empty->SetRole(ThemeLabel::Role::Sub);
        empty->SetText(SDK_TR("filebrowser.empty"));
        empty->SetAttribute("height", "24");
        m_remoteList->AddItem(empty);
        return;
    }
    for (const Entry& e : m_remoteEntries) {
        AddRow(m_remoteList, e, false);
    }
}

// ---------------------------------------------------------------- rows

void FileBrowserWindow::ClearPane(ui::Box* pane)
{
    if (pane != nullptr) {
        pane->RemoveAllItems();
    }
    // drop only the rows of THIS pane from the hit-test bookkeeping - the
    // other pane's rows must stay, otherwise right-click works only on the
    // pane that refreshed last (e.g. the async remote reply wiped the local
    // rows right after startup, so the local pane never opened a menu).
    // NOTE: RemoveAllItems() above already DESTROYED this pane's row objects,
    // so the rows are matched by their stored pane pointer - never call a
    // method on r.row here (use-after-free).
    m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(),
                                [pane](const RowInfo& r) {
                                    return r.pane == pane;
                                }),
                 m_rows.end());
}

void FileBrowserWindow::AddRow(ui::Box* pane, const Entry& e, bool local)
{
    const Palette& p = Theme::Get();
    const ui::UiColor transparent(0, 0, 0, 0);

    ui::HBox* row = new ui::HBox(this);
    row->SetAttribute("height", "26");
    row->SetAttribute("padding", "4,0,4,0");
    row->SetBkColor(transparent);
    pane->AddItem(row);

    FileIconControl* icon = new FileIconControl(this);
    icon->SetFolder(e.isDir);
    icon->SetAttribute("width", "16");
    icon->SetAttribute("height", "20");
    icon->SetAttribute("valign", "center");
    icon->SetAttribute("mouse_enabled", "false");
    row->AddItem(icon);

    ThemeLabel* nameLabel = new ThemeLabel(this);
    nameLabel->SetRole(ThemeLabel::Role::Main);
    nameLabel->SetText(e.name);
    nameLabel->SetAttribute("width", "stretch");
    nameLabel->SetAttribute("height", "26");
    nameLabel->SetAttribute("valign", "center");
    nameLabel->SetAttribute("text_align", "left,vcenter");
    nameLabel->SetAttribute("mouse_enabled", "false");
    row->AddItem(nameLabel);

    ThemeLabel* sizeLabel = new ThemeLabel(this);
    sizeLabel->SetRole(ThemeLabel::Role::Sub);
    sizeLabel->SetText(e.isDir ? "" : FormatSize(e.size));
    sizeLabel->SetAttribute("width", "auto");
    sizeLabel->SetAttribute("height", "26");
    sizeLabel->SetAttribute("valign", "center");
    sizeLabel->SetAttribute("text_align", "right,vcenter");
    sizeLabel->SetAttribute("margin", "0,0,6,0");
    sizeLabel->SetAttribute("mouse_enabled", "false");
    row->AddItem(sizeLabel);

    // Finder-like rows: left-click = select, double-click = enter/transfer,
    // right-button-down = context menu (reliable - the dui's RClick only
    // fires on the button-up inside the row). NOTE: the rows are plain
    // controls inside a scroll box, so the dui never delivers its click /
    // double-click events to them - selection and enter/transfer are handled
    // in OnMouseLButtonUpMsg / OnMouseLButtonDbClickMsg instead.
    const std::string name = e.name;
    const bool isDir = e.isDir;
    const uint64_t size = e.size;
    m_rows.push_back({row, pane, name, isDir, local, size});
    row->AttachMouseEnter([this, row, p](const ui::EventArgs&) {
        if (row != m_selectedRow) {
            row->SetBkColor(p.listHover);
        }
        return true;
    });
    row->AttachMouseLeave([this, row, transparent](const ui::EventArgs&) {
        if (row != m_selectedRow) {
            row->SetBkColor(transparent);
        }
        return true;
    });
}

void FileBrowserWindow::SelectRow(ui::Control* row)
{
    if (row == nullptr) {
        return;
    }
    const Palette& p = Theme::Get();
    if (m_selectedRow != nullptr && m_selectedRow != row) {
        m_selectedRow->SetBkColor(ui::UiColor(0, 0, 0, 0));
    }
    m_selectedRow = row;
    row->SetBkColor(p.listSelected);
}

void FileBrowserWindow::ShowRowMenu(const ui::UiPoint& pt, ui::Control* related,
                                    bool local, const std::string& name, bool isDir,
                                    uint64_t size)
{
    const auto t0 = std::chrono::steady_clock::now();
    ui::Menu* menu = new ui::Menu(this, related);
    m_contextMenu = menu;
    m_contextMenuFlag = menu->GetWeakFlag();
    // pure-code menu: no XML shadow config - the default shadow corner (60px
    // scaled) shifted the window 29px up-left of the cursor; disable it so
    // the menu pops exactly at the cursor
    menu->SetShadowType(ui::Shadow::ShadowType::kShadowDrawNone);
    // noFocus: the menu must NOT steal the keyboard focus - taking focus made
    // the parent window's OS shadow oscillate (focused vs unfocused state)
    // and caused a focus race where the menu closed instantly (sometimes it
    // appeared, sometimes it didn't)
    // pt is in CLIENT coordinates; ShowMenu positions the popup in SCREEN
    // coordinates - passing client coords put the menu at the window's
    // top-left instead of under the cursor (the "menu at the left" bug)
    ui::UiPoint screenPt = pt;
    ClientToScreen(screenPt);
    menu->ShowMenu("", screenPt, ui::MenuPopupPosType::LEFT_TOP, true);
    if (isDir) {
        ui::MenuItem* enter = new ui::MenuItem(menu);
        enter->SetClass("menu_element");
        enter->SetName("enter");
        enter->SetFixedWidth(ui::UiFixedInt(160), true, true);
        ui::Label* lbl = new ui::Label(menu);
        lbl->SetClass("menu_text");
        lbl->SetText(SDK_TR("filebrowser.enter"));
        lbl->SetAttribute("mouse_enabled", "false");
        enter->AddItem(lbl);
        menu->AddMenuItem(enter);

        if (local) {
            // upload the whole folder (recursively) to the remote folder
            ui::MenuItem* up = new ui::MenuItem(menu);
            up->SetClass("menu_element");
            up->SetName("upload");
            up->SetFixedWidth(ui::UiFixedInt(160), true, true);
            ui::Label* upLbl = new ui::Label(menu);
            upLbl->SetClass("menu_text");
            upLbl->SetText(SDK_TR("filebrowser.upload"));
            upLbl->SetAttribute("mouse_enabled", "false");
            up->AddItem(upLbl);
            menu->AddMenuItem(up);
        }
        else {
            // download the whole folder (recursively) to the local folder
            ui::MenuItem* dl = new ui::MenuItem(menu);
            dl->SetClass("menu_element");
            dl->SetName("download");
            dl->SetFixedWidth(ui::UiFixedInt(160), true, true);
            ui::Label* dlLbl = new ui::Label(menu);
            dlLbl->SetClass("menu_text");
            dlLbl->SetText(SDK_TR("filebrowser.download"));
            dlLbl->SetAttribute("mouse_enabled", "false");
            dl->AddItem(dlLbl);
            menu->AddMenuItem(dl);
        }
    }
    else {
        ui::MenuItem* act = new ui::MenuItem(menu);
        act->SetClass("menu_element");
        act->SetName(local ? "upload" : "download");
        act->SetFixedWidth(ui::UiFixedInt(160), true, true);
        ui::Label* lbl = new ui::Label(menu);
        lbl->SetClass("menu_text");
        lbl->SetText(local ? SDK_TR("filebrowser.upload")
                           : SDK_TR("filebrowser.download"));
        lbl->SetAttribute("mouse_enabled", "false");
        act->AddItem(lbl);
        menu->AddMenuItem(act);
    }
    ui::MenuItem* refresh = new ui::MenuItem(menu);
    refresh->SetClass("menu_element");
    refresh->SetName("refresh");
    refresh->SetFixedWidth(ui::UiFixedInt(160), true, true);
    ui::Label* rl = new ui::Label(menu);
    rl->SetClass("menu_text");
    rl->SetText(SDK_TR("filebrowser.refresh"));
    rl->SetAttribute("mouse_enabled", "false");
    refresh->AddItem(rl);
    menu->AddMenuItem(refresh);

    const bool isDirC = isDir;
    const uint64_t sizeC = size;
    const std::string nameC = name;
    const bool localC = local;
    menu->AttachMenuItemActivated([this, localC, nameC, isDirC, sizeC](
                                      const std::string&, int32_t,
                                      const std::string& itemName, size_t) {
        // the menu closes itself after the callback - drop the pointer so the
        // click-to-dismiss handlers never touch a dead window
        m_contextMenu = nullptr;
        m_contextMenuFlag.reset();
        if (itemName == "enter" && isDirC) {
            if (localC) {
                EnterLocalDir(nameC);
            }
            else {
                EnterRemoteDir(ui::StringConvert::TToUTF8(nameC));
            }
        }
        else if (itemName == "upload") {
            // file or folder upload (folders recurse on the host)
            try {
                std::vector<ui::FilePath> files;
                std::error_code ec;
                const std::filesystem::path full =
                    m_localDir / ui::StringConvert::TToUTF8(nameC);
                if (std::filesystem::exists(full, ec)) {
                    files.emplace_back(ui::FilePath(ui::StringConvert::UTF8ToT(full.string())));
                    UploadFiles(files);
                }
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[browser] menu upload exception: %s\n", e.what());
            }
            catch (...) {
                fprintf(stderr, "[browser] menu upload unknown exception\n");
            }
        }
        else if (itemName == "download") {
            OnRemoteRowClick(ui::StringConvert::TToUTF8(nameC), false, sizeC);
        }
        else if (itemName == "refresh") {
            if (localC) {
                RefreshLocal();
            }
            else {
                RefreshRemote();
            }
        }
        return true;
    });

    // the window exists now: actually disable the shadow (removes the
    // shadow-image rendering cost + the margin; the pre-ShowMenu call was
    // a no-op because the shadow object is created with the window)
    menu->SetShadowType(ui::Shadow::ShadowType::kShadowDrawNone);
    // Size: the menu window was sized from the EMPTY listbox before the items
    // existed (the "right-click shows no content" bug). The library resizes a
    // pure-code menu itself on every AddMenuItem/AddMenuControl, so no explicit
    // update is needed here.
    const auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "[browser] menu created in %lld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

void FileBrowserWindow::CloseContextMenu()
{
    if (m_contextMenu != nullptr) {
        ui::Menu* menu = m_contextMenu;
        m_contextMenu = nullptr;
        if (!m_contextMenuFlag.expired()) {
            // still alive: close it explicitly
            menu->CloseMenu();
        }
        // else: the menu already closed itself (e.g. focus loss) and the dui
        // destroyed the window - never touch the pointer
        m_contextMenuFlag.reset();
    }
}

LRESULT FileBrowserWindow::OnMouseLButtonDownMsg(const ui::UiPoint& pt,
                                                 uint32_t modifierKey,
                                                 const ui::NativeMsg& nativeMsg,
                                                 bool& bHandled)
{
    CloseContextMenu();
    return BaseClass::OnMouseLButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);
}

LRESULT FileBrowserWindow::OnMouseLButtonUpMsg(const ui::UiPoint& pt,
                                               uint32_t modifierKey,
                                               const ui::NativeMsg& nativeMsg,
                                               bool& bHandled)
{
    LRESULT lResult = BaseClass::OnMouseLButtonUpMsg(pt, modifierKey, nativeMsg, bHandled);
    // left-click selects the row under the cursor (the dui click event never
    // reaches these plain rows)
    for (const RowInfo& r : m_rows) {
        if (r.row != nullptr && r.row->IsPointInWithScrollOffset(pt)) {
            SelectRow(r.row);
            break;
        }
    }
    return lResult;
}

LRESULT FileBrowserWindow::OnMouseLButtonDbClickMsg(const ui::UiPoint& pt,
                                                    uint32_t modifierKey,
                                                    const ui::NativeMsg& nativeMsg,
                                                    bool& bHandled)
{
    LRESULT lResult = BaseClass::OnMouseLButtonDbClickMsg(pt, modifierKey, nativeMsg, bHandled);
    // double-click enters the folder / starts the transfer (the dui
    // double-click event never reaches these plain rows either)
    for (const RowInfo& r : m_rows) {
        if (r.row != nullptr && r.row->IsPointInWithScrollOffset(pt)) {
            if (r.local) {
                OnLocalRowClick(r.name, r.isDir, r.size);
            }
            else {
                OnRemoteRowClick(ui::StringConvert::TToUTF8(r.name), r.isDir, r.size);
            }
            break;
        }
    }
    return lResult;
}

LRESULT FileBrowserWindow::OnMouseRButtonDownMsg(const ui::UiPoint& pt,
                                                 uint32_t modifierKey,
                                                 const ui::NativeMsg& nativeMsg,
                                                 bool& bHandled)
{
    // SDL/X11 delivers the button-down once MORE (same point, immediately)
    // when the menu window pops up under the pointer while the button is
    // held. The duplicate must be IGNORED ENTIRELY: closing the menu or even
    // letting the base class run would re-focus a row, the menu loses focus
    // and closes itself (the "menu randomly vanishes" bug). The window is
    // wide because creating the menu window can take a few hundred ms (it
    // falls back to an opaque window when the transparent create fails).
    const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (pt == m_lastRbdPt && (nowMs - m_lastRbdMs) < 1000) {
        return 0;
    }
    m_lastRbdPt = pt;
    m_lastRbdMs = nowMs;

    // Run the base class FIRST: it captures the mouse and focuses the control
    // under the cursor. Running it after ShowRowMenu would re-focus the row,
    // stealing focus from the menu window that was just created - the menu
    // sees the focus loss and closes itself (the "menu randomly vanishes" bug).
    LRESULT lResult = BaseClass::OnMouseRButtonDownMsg(pt, modifierKey, nativeMsg, bHandled);

    // dismiss any open menu, then select the row under the cursor and open
    // its menu on the BUTTON-DOWN (the dui's RClick event only fires on the
    // button-up inside the row - a slight mouse move made the menu randomly
    // not appear). Explorer-like: the right-click also selects the row.
    CloseContextMenu();
    for (const RowInfo& r : m_rows) {
        if (r.row != nullptr && r.row->IsPointInWithScrollOffset(pt)) {
            SelectRow(r.row);
            ShowRowMenu(pt, r.row, r.local, r.name, r.isDir, r.size);
            break;
        }
    }
    return lResult;
}

LRESULT FileBrowserWindow::OnKeyDownMsg(ui::VirtualKeyCode vkCode, uint32_t modifierKey,
                                        const ui::NativeMsg& nativeMsg, bool& bHandled)
{
    if (vkCode == ui::kVK_ESCAPE) {
        bHandled = true;
        CloseContextMenu();
        return 0;
    }
    return BaseClass::OnKeyDownMsg(vkCode, modifierKey, nativeMsg, bHandled);
}

void FileBrowserWindow::OnLocalRowClick(const std::string& name, bool isDir, uint64_t size)
{
    if (isDir) {
        EnterLocalDir(name);
        return;
    }
    // upload to the remote's current folder
    std::vector<ui::FilePath> files;
    std::error_code ec;
    const std::filesystem::path full = m_localDir / ui::StringConvert::TToUTF8(name);
    if (!std::filesystem::is_regular_file(full, ec)) {
        return;
    }
    files.emplace_back(ui::FilePath(ui::StringConvert::UTF8ToT(full.string())));
    UploadFiles(files);
}

void FileBrowserWindow::OnRemoteRowClick(const std::string& name, bool isDir, uint64_t size)
{
    (void)size;
    if (isDir) {
        EnterRemoteDir(name);
        return;
    }
    if (!m_sessionRaw) {
        return;
    }
    const std::string remoteFile = m_remotePath.empty()
                                       ? name
                                       : m_remotePath + "/" + name;
    SetStatus(SDK_TR("filebrowser.downloading"));
    m_sessionRaw->RequestDownload(remoteFile, m_localDir.string());
}

// ---------------------------------------------------------------- transfers

void FileBrowserWindow::UploadFiles(const std::vector<ui::FilePath>& files)
{
    std::string err;
    try {
    if (!m_sessionRaw || files.empty()) {
        return;
    }
    SetStatus(SDK_TR("filebrowser.uploading"));
    bool ok = false;
    ok = App::Instance().FileTx().SendFiles(
        m_sessionRaw->GetHost(), m_sessionRaw->FilePort(), m_sessionRaw->Token(), files,
        m_remotePath,
        [this](const FileTransfer::Item& item) {
            // transfer finished (worker thread): reflect it in the status line
            bool done = false;
            bool failed = false;
            switch (item.state) {
            case FileTransfer::ItemState::Done:
                done = true;
                break;
            case FileTransfer::ItemState::Failed:
            case FileTransfer::ItemState::Rejected:
            case FileTransfer::ItemState::Cancelled:
                failed = true;
                break;
            default:
                break;
            }
            if (done || failed) {
                PostToUI([this, done, name = item.name]() {
                    if (done) {
                        SetStatus(ui::StringUtil::Printf(
                            "%s ✓", ui::StringConvert::UTF8ToT(name).c_str()));
                        // the uploaded files are on the remote now - refresh
                        // both panes so the new content shows immediately
                        RefreshRemote();
                        RefreshLocal();
                    }
                    else {
                        SetStatus(ui::StringUtil::Printf(
                                      "%s ✗",
                                      ui::StringConvert::UTF8ToT(name).c_str()),
                                  true);
                    }
                });
            }
        },
        err);
    }
    catch (const std::exception& e) {
        fprintf(stderr, "[browser] UploadFiles exception: %s\n", e.what());
        SetStatus(ui::StringConvert::UTF8ToT(e.what()), true);
    }
    catch (...) {
        fprintf(stderr, "[browser] UploadFiles unknown exception\n");
        SetStatus("exception", true);
    }
}

void FileBrowserWindow::OnFileDropped(const ui::EventArgs& args)
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
    if (!files.empty()) {
        UploadFiles(files);
    }
}

void FileBrowserWindow::SetStatus(const std::string& text, bool danger)
{
    if (m_statusLabel != nullptr) {
        m_statusLabel->SetRole(danger ? ThemeLabel::Role::Danger : ThemeLabel::Role::Sub);
        m_statusLabel->SetText(text);
    }
}

std::string FileBrowserWindow::FormatSize(uint64_t bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0) {
        return ui::StringUtil::Printf("%llu B", (unsigned long long)bytes);
    }
    return ui::StringUtil::Printf("%.1f %hs", v, units[u]);
}

} // namespace sdk
