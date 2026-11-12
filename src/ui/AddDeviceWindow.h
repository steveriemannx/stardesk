#ifndef STARDESK_UI_ADD_DEVICE_WINDOW_H_
#define STARDESK_UI_ADD_DEVICE_WINDOW_H_

#include "dui/dui.h"

#include "LangBinder.h"
#include "VectorArt.h"

namespace sdk {

/** Small dialog for adding a device that is not discovered automatically
 *  (fixed IP / another subnet): name (optional), address and port.
 */
class AddDeviceWindow : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    typedef std::function<void(const std::string& name, const std::string& ip, int port)> SaveCb;

    explicit AddDeviceWindow(SaveCb cb);

    virtual std::string GetSkinFolder() override;
    virtual std::string GetSkinFile() override;
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    virtual void OnInitWindow() override;

    void ApplyTheme();

private:
    void DoSave();

    ui::HBox* m_caption = nullptr;
    LangBinder m_lang;
    SaveCb m_cb;
    ui::RichEdit* m_nameEdit = nullptr;
    ui::RichEdit* m_ipEdit = nullptr;
    ui::RichEdit* m_portEdit = nullptr;
    ThemeLabel* m_errorLabel = nullptr;
};

} // namespace sdk

#endif // STARDESK_UI_ADD_DEVICE_WINDOW_H_
