#ifndef STARDesk_MAIN_THREAD_H_
#define STARDesk_MAIN_THREAD_H_

#include "dui/dui.h"

namespace sdk {

/** Application thread: initializes the framework, starts the StarDesk
 *  services (host listener, discovery beacon, config) and shows the main window.
 */
class MainThread : public ui::FrameworkThread
{
    typedef ui::FrameworkThread BaseClass;
public:
    MainThread();
    virtual ~MainThread() override;

    /** Run the UI message loop (entry of DUI_APP_ENTRY).
     */
    void Run() { RunMessageLoop(); }

private:
    virtual void OnInit() override;
    virtual void OnCleanup() override;
};

} // namespace sdk

#endif // STARDesk_MAIN_THREAD_H_
