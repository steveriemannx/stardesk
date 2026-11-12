#ifndef STARDESK_UI_FILE_BROWSER_WINDOW_H_
#define STARDESK_UI_FILE_BROWSER_WINDOW_H_

#include "dui/dui.h"

#include <filesystem>

#include "../app/AppConfig.h"
#include "LangBinder.h"
#include "../session/ClientSession.h"
#include "VectorArt.h"

namespace sdk {

/** Two-pane file browser opened from the remote window's "send file" button.
 *
 *  Left pane = local files, right pane = remote (host) files. Clicking a
 *  file uploads (local -> remote) or downloads (remote -> local) it; clicking
 *  a folder enters it; the "up" button goes up; local files can be dragged
 *  onto the window to upload them to the remote's current folder.
 */
class FileBrowserWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;public:
    FileBrowserWindow(ClientSession* session);
    virtual ~FileBrowserWindow() override;

    virtual std::string GetSkinFolder() override;
    virtual std::string GetSkinFile() override;
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    virtual void OnInitWindow() override;

    virtual void OnThemeChanged() override { ApplyTheme(); }
    virtual void OnLanguageChanged() override { ApplyLanguage(); }

    // close any open context menu when the user clicks anywhere on the
    // browser (the menu is noFocus, so it does not close on focus change)
    virtual LRESULT OnMouseLButtonDownMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                          const ui::NativeMsg& nativeMsg,
                                          bool& bHandled) override;
    // plain (non-ListBox) rows never receive the dui click event, so the
    // selection (left button-up) and the enter/transfer action (double-click)
    // are handled at the window level
    virtual LRESULT OnMouseLButtonUpMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                        const ui::NativeMsg& nativeMsg,
                                        bool& bHandled) override;
    virtual LRESULT OnMouseLButtonDbClickMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                             const ui::NativeMsg& nativeMsg,
                                             bool& bHandled) override;
    virtual LRESULT OnMouseRButtonDownMsg(const ui::UiPoint& pt, uint32_t modifierKey,
                                          const ui::NativeMsg& nativeMsg,
                                          bool& bHandled) override;
    virtual LRESULT OnKeyDownMsg(ui::VirtualKeyCode vkCode, uint32_t modifierKey,
                                 const ui::NativeMsg& nativeMsg, bool& bHandled) override;

    void ApplyTheme();
    void ApplyLanguage();
    void CloseContextMenu();
    void SelectRow(ui::Control* row);
    ui::Menu* m_contextMenu = nullptr; // open row context menu (owned by the dui)
    // the dui deletes the menu window when it closes itself (e.g. on focus
    // loss); the weak flag tells whether m_contextMenu is still alive
    std::weak_ptr<ui::WeakFlag> m_contextMenuFlag;

    // row bookkeeping for hit-testing on right-button-down (the dui's RClick
    // only fires on the button-up inside the row, which misses when the mouse
    // moves slightly - that was the "menu sometimes does not appear" bug).
    // pane = the list the row belongs to; it outlives the row objects (which
    // are destroyed on refresh), so bookkeeping is keyed on the pane, never
    // on a destroyed row pointer.
    struct RowInfo {
        ui::Control* row = nullptr;
        ui::Box* pane = nullptr;
        std::string name;
        bool isDir = false;
        bool local = false;
        uint64_t size = 0;
    };
    std::vector<RowInfo> m_rows;

private:
    ui::HBox* m_caption = nullptr;
    LangBinder m_lang;
    struct Entry {
        std::string name;
        bool isDir = false;
        uint64_t size = 0;
    };

    // ---- local pane ----
    void RefreshLocal();
    void EnterLocalDir(const std::string& name);
    void GoLocalUp();
    void RenderLocal();

    // ---- remote pane ----
    void RefreshRemote();
    void EnterRemoteDir(const std::string& name);
    void GoRemoteUp();
    void RenderRemote();
    void OnDirListResult(bool ok, const std::string& path,
                         const std::vector<FileEntry>& entries);

    // ---- transfers ----
    void UploadFiles(const std::vector<ui::FilePath>& files);
    void OnLocalRowClick(const std::string& name, bool isDir, uint64_t size);
    void OnRemoteRowClick(const std::string& name, bool isDir, uint64_t size);
    void OnFileDropped(const ui::EventArgs& args);
    void SetStatus(const std::string& text, bool danger = false);

    static std::string FormatSize(uint64_t bytes);

    void ClearPane(ui::Box* pane);
    void AddRow(ui::Box* pane, const Entry& e, bool local);
    void ShowRowMenu(const ui::UiPoint& pt, ui::Control* related, bool local,
                     const std::string& name, bool isDir, uint64_t size);

    std::shared_ptr<ClientSession> m_session;
    ClientSession* m_sessionRaw = nullptr;

    // local navigation (UI thread)
    std::filesystem::path m_localDir;
    std::vector<Entry> m_localEntries;

    // remote navigation (mirrored on the session thread)
    std::string m_remotePath; // "" = host home
    std::vector<Entry> m_remoteEntries;
    bool m_remoteLoading = false;

    ui::VBox* m_root = nullptr;
    ui::VBox* m_body = nullptr;
    ui::HBox* m_panes = nullptr;
    ui::VBox* m_remoteCol = nullptr;
    ui::HBox* m_remoteHead = nullptr;
    ui::VBox* m_localCol = nullptr;
    ui::HBox* m_localHead = nullptr;
    CardBox* m_localPane = nullptr;
    CardBox* m_remotePane = nullptr;
    ui::VScrollBox* m_localList = nullptr;
    ui::VScrollBox* m_remoteList = nullptr;
    ThemeLabel* m_localPathLabel = nullptr;
    ThemeLabel* m_remotePathLabel = nullptr;
    ThemeLabel* m_statusLabel = nullptr;
    ui::Control* m_selectedRow = nullptr; // last clicked row (selection highlight)

    // duplicate-delivery guard for the right-button-down: SDL/X11 re-delivers
    // the button-down once (same point, immediately) when the menu window pops
    // under the pointer while the button is held; the duplicate used to close
    // the menu that was just opened (the "menu randomly vanishes" bug)
    ui::UiPoint m_lastRbdPt;
    uint64_t m_lastRbdMs = 0;
};

} // namespace sdk

#endif // STARDESK_UI_FILE_BROWSER_WINDOW_H_
