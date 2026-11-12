#ifndef STARDESK_UI_MAIN_WINDOW_H_
#define STARDESK_UI_MAIN_WINDOW_H_

#include "dui/dui.h"

#include "../app/AppConfig.h"
#include "../session/ClientSession.h"
#include "../session/HostSession.h"
#include "LangBinder.h"
#include "VectorArt.h"

#include <set>

namespace sdk {

class RemoteWindow;

/** StarDesk main window: host panel (left) + remote-control / file tabs (right).
 *  Pure code UI; every control is built in OnInitWindow().
 */
class MainWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;
public:
    MainWindow();
    virtual ~MainWindow() override;

    virtual std::string GetSkinFolder() override;
    virtual std::string GetSkinFile() override;
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    virtual void OnInitWindow() override;

    // IAppListener (UI thread)
    virtual void OnThemeChanged() override;
    virtual void OnLanguageChanged() override;
    virtual LRESULT OnPaintMsg(const ui::UiRect& rcPaint, const ui::NativeMsg& nativeMsg,
                               bool& bHandled) override;

    /** Re-apply theme colors to the few controls that cache them. */
    void ApplyTheme();
    /** Re-apply all UI strings in the current language. */
    void ApplyLanguage();

    /** Set the host status line (e.g. "ready", "device X connected"). */
    void SetHostStatus(const std::string& text, bool ok);

    /** Local IPs + port shown in the device card (filled by the network layer). */
    void SetLocalAddresses(const std::vector<std::string>& ips);

    /** Refresh the LAN device discovery list. */
    void SetLanDevices(const std::vector<std::pair<std::string, std::string>>& nameIpPorts);

private:
    // ---- construction helpers ----
    ui::Label* MakeLabel(const char* key, ThemeLabel::Role role);
    ui::RichEdit* MakeEdit(const char* promptKey, bool password);
    PillButton* MakePill(const char* key);
    IconButton* MakeIconButton(VectorArt::Icon icon, const char* tooltipKey,
                               const std::string& name);

    // ---- UI logic ----
    void SelectTab(bool remote);
    void OnConnectClicked();
    void OnClientState(ClientSession::State state, const std::string& text);
    void OnHostStatus(const std::string& text, bool ok);

    // ---- file tab ----
    void OnSendFileClicked();
    void OnItemUpdated(const FileTransfer::Item& item);
    void OnOpenReceiveDir();
    void RebuildTransferList();
    void ClearTransferRows();

    void ApplyThemeEdits();
    void SetEditBk(ui::RichEdit* edit);
    /** Symmetric inner text padding so the caret can be placed after the last
     *  character when clicking the empty area of a field. */
    void SetTextPaddingForEdit(ui::RichEdit* edit);

    // caption
    IconButton* m_themeBtn = nullptr;
    PillButton* m_langBtn = nullptr;

    // sidebar
    ui::VBox* m_sidebar = nullptr;
    ThemeLabel* m_deviceNameLabel = nullptr;
    ThemeLabel* m_addrLabel = nullptr;
    ui::RichEdit* m_hostPwdEdit = nullptr;
    IconButton* m_pwdShowBtn = nullptr;
    bool m_pwdVisible = false;
    PillButton* m_autoAcceptPill = nullptr;
    PillButton* m_manualAcceptPill = nullptr;
    PillButton* m_mirrorPill = nullptr;
    PillButton* m_extendPill = nullptr;
    ThemeLabel* m_hostStatusLabel = nullptr;
    /** Permission warning shown under the host status (macOS accessibility). */
    ThemeLabel* m_permissionHint = nullptr;
    PillButton* m_permissionBtn = nullptr;
    /** Local interface addresses, used to filter our own discovery beacons. */
    std::vector<std::string> m_localAddresses;

    // right side
    PillButton* m_tabRemote = nullptr;
    PillButton* m_tabFile = nullptr;
    ui::VBox* m_panelRemote = nullptr;
    ui::VBox* m_panelFile = nullptr;

    // connect panel (advanced options live in SettingsWindow)
    ui::VBox* m_deviceList = nullptr;
    ThemeLabel* m_deviceEmptyLabel = nullptr;
    PillButton* m_manageBtn = nullptr;
    bool m_manageDevices = false;
    /** LAN peers removed by the user ("ip:port"), filtered out of discovery. */
    std::set<std::string> m_hiddenDevices;
    void ToggleManageDevices();
    void HideDevice(const std::string& ipPort);
    void RestoreHiddenDevices();
    void UpdateManageButton();
    ui::RichEdit* m_remoteIpEdit = nullptr;
    ui::RichEdit* m_remotePortEdit = nullptr;
    ui::RichEdit* m_remotePwdEdit = nullptr;
    IconButton* m_remotePwdShowBtn = nullptr;
    bool m_remotePwdVisible = false;
    ThemeLabel* m_connTitleLabel = nullptr;
    IconButton* m_settingsBtn = nullptr;
    AccentButton* m_connectBtn = nullptr;
    ThemeLabel* m_connStatusLabel = nullptr;

    // file tab
    ThemeLabel* m_receiveDirLabel = nullptr;
    AccentButton* m_sendFileBtn = nullptr;
    ui::VBox* m_transferList = nullptr;
    ThemeLabel* m_transferEmptyLabel = nullptr;
    std::map<uint32_t, FileTransfer::Item> m_transfers;

    // language cycling support
    void CycleLanguage();
    /** Re-apply every bound string after a runtime language switch. */
    LangBinder m_lang;
    /** Last LAN device list, re-rendered on a language switch (the row labels
     *  and the empty hint are built from translated strings). */
    std::vector<std::pair<std::string, std::string>> m_lastLanDevices;

    // services
    std::unique_ptr<HostService> m_host;
    std::unique_ptr<ClientSession> m_client;
    /** The standalone remote-view window while a session is active. */
    RemoteWindow* m_remoteWin = nullptr;
    /** Port the host actually bound (may differ from the configured port
     *  when that one is taken, e.g. by a second instance on this machine). */
    uint16_t m_actualPort = 0;

    // list of edits needing theme background refresh
    std::vector<ui::RichEdit*> m_edits;
};

} // namespace sdk

#endif // STARDESK_UI_MAIN_WINDOW_H_
