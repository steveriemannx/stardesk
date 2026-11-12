#ifndef STARDESK_UI_INVITE_WINDOW_H_
#define STARDESK_UI_INVITE_WINDOW_H_

#include "dui/dui.h"

#include "../app/AppConfig.h"

#include "LangBinder.h"
#include "VectorArt.h"

namespace sdk {

/** Reverse remote-assistance dialog: "device X asks YOU to control IT".
 *  Accepting makes this machine connect back to the requester (one-time token,
 *  no password).
 */
class InviteWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;
public:
    typedef std::function<void(bool accept)> ResultCb;

    InviteWindow(const std::string& deviceName, const std::string& ip, bool view,
                 ResultCb cb);
    virtual ~InviteWindow() override;

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
    std::string m_deviceName;
    std::string m_ip;
    bool m_view = false;
    ResultCb m_cb;
    bool m_done = false;
    ThemeLabel* m_textLabel = nullptr;
};

} // namespace sdk

#endif // STARDESK_UI_INVITE_WINDOW_H_
