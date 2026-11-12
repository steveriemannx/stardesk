#ifndef STARDESK_UI_ASK_WINDOW_H_
#define STARDESK_UI_ASK_WINDOW_H_

#include "dui/dui.h"

#include "../app/AppConfig.h"
#include "../session/HostSession.h"
#include "LangBinder.h"
#include "VectorArt.h"

namespace sdk {

/** Manual-accept dialog: shows who is asking and whether they want control or
 *  view; the result is delivered through the callback (UI thread).
 */
class AskWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;
public:
    typedef std::function<void(bool accept)> ResultCb;

    explicit AskWindow(const AskInfo& info, ResultCb cb);
    virtual ~AskWindow() override;

    virtual std::string GetSkinFolder() override;
    virtual std::string GetSkinFile() override;
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    virtual void OnInitWindow() override;

    virtual void OnThemeChanged() override { ApplyTheme(); }
    virtual void OnLanguageChanged() override { ApplyLanguage(); }

    void ApplyTheme();
    void ApplyLanguage();

private:
    LangBinder m_lang;
    AskInfo m_info;
    ResultCb m_cb;
    bool m_done = false;

    ThemeLabel* m_textLabel = nullptr;
};

} // namespace sdk

#endif // STARDESK_UI_ASK_WINDOW_H_
