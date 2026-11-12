#ifndef STARDESK_UI_SETTINGS_WINDOW_H_
#define STARDESK_UI_SETTINGS_WINDOW_H_

#include "dui/dui.h"

#include "../app/AppConfig.h"
#include "LangBinder.h"
#include "VectorArt.h"

namespace sdk {

/** Connection settings window (ToDesk-style): keeps the advanced client
 *  options (control/view mode, resolution, frame rate) out of the main
 *  window's connect panel. Every change is applied to AppConfig and
 *  persisted immediately; the next connect uses it.
 */
class SettingsWindow : public ui::WindowImplBase, public IAppListener
{
    typedef ui::WindowImplBase BaseClass;
public:
    SettingsWindow();
    virtual ~SettingsWindow() override;

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
    PillButton* MakePill(const char* key);

    // option rows (values are persisted to AppConfig)
    PillButton* m_viewOnlyPill = nullptr;  // view only
    PillButton* m_controlPill = nullptr;   // control
    PillButton* m_resOriginalPill = nullptr;
    PillButton* m_res720Pill = nullptr;
    PillButton* m_res1080Pill = nullptr;
    PillButton* m_fps24Pill = nullptr;
    PillButton* m_fps30Pill = nullptr;
    PillButton* m_fps60Pill = nullptr;

    ThemeLabel* m_modeLabel = nullptr;
    ThemeLabel* m_resLabel = nullptr;
    ThemeLabel* m_fpsLabel = nullptr;
};

} // namespace sdk

#endif // STARDESK_UI_SETTINGS_WINDOW_H_
