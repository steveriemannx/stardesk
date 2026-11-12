#ifndef STARDESK_UI_FILE_SEND_WINDOW_H_
#define STARDESK_UI_FILE_SEND_WINDOW_H_

#include "dui/dui.h"

#include "../app/AppConfig.h"
#include "LangBinder.h"
#include "VectorArt.h"

namespace sdk {

/** File-send window opened by the "send files" buttons. Collects files via
 *  drag & drop or a native file dialog, then pushes them through the global
 *  FileTransfer service to the given peer (host + file port + session token).
 */
class FileSendWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;
public:
    FileSendWindow(const std::string& peerHost, uint16_t peerFilePort,
                   const uint8_t token[32]);
    virtual ~FileSendWindow() override;

    virtual std::string GetSkinFolder() override;
    virtual std::string GetSkinFile() override;
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    virtual void OnInitWindow() override;

    virtual void OnThemeChanged() override { ApplyTheme(); }
    virtual void OnLanguageChanged() override { ApplyLanguage(); }

    void ApplyTheme();
    void ApplyLanguage();

private:
    ui::HBox* m_caption = nullptr;
    LangBinder m_lang;
    struct Row {
        uint32_t id = 0;
        std::string name;
        uint64_t size = 0;
        CardBox* card = nullptr;
        ThemeLabel* statusLabel = nullptr;
        ProgressBar* progress = nullptr;
        IconButton* cancelBtn = nullptr;
    };

    void AddFiles(const std::vector<ui::FilePath>& files);
    void OnFileDropped(const ui::EventArgs& args);
    void OnBrowse();
    void OnSend();
    void OnItemUpdated(uint32_t id, const FileTransfer::Item& item);
    void RebuildRows();
    void ClearRows();

    std::string m_peerHost;
    std::string m_targetDir; // "" = peer's default receive dir
    uint16_t m_peerFilePort = 0;
    uint8_t m_token[32] = {0};
    std::vector<ui::FilePath> m_files;
    std::vector<Row> m_rows;
    std::map<uint32_t, FileTransfer::Item> m_items;

    CardBox* m_dropCard = nullptr;
    ThemeLabel* m_dropHint = nullptr;
    AccentButton* m_sendBtn = nullptr;
    ui::VBox* m_list = nullptr;
    ThemeLabel* m_emptyLabel = nullptr;
};

} // namespace sdk

#endif // STARDESK_UI_FILE_SEND_WINDOW_H_
