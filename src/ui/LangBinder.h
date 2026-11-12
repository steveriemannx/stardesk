#ifndef STARDESK_UI_LANG_BINDER_H_
#define STARDESK_UI_LANG_BINDER_H_

#include "dui/dui.h"

#include <algorithm>

#include "../app/Language.h"

namespace sdk {

/** Binds pure-code controls to translation keys so a runtime language switch
 *  re-applies every one of them. The windows are built in code (no XML layout),
 *  so dui's XML language reload never runs - each window keeps a binder and
 *  calls Apply() from OnLanguageChanged.
 */
class LangBinder {
public:
    /** Set the control's text to SDK_TR(key) and remember it. */
    void SetText(ui::Control* control, const char* key)
    {
        if (control == nullptr) {
            return;
        }
        SetControlText(control, SDK_TR(key));
        Bind(control, key);
    }

    /** Forget a control (it is about to be destroyed / re-created). */
    void Unbind(ui::Control* control)
    {
        if (control == nullptr) {
            return;
        }
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                     [control](const Item& item) {
                                         return item.control == control;
                                     }),
                      m_items.end());
    }

    /** Track a control whose text was already set to SDK_TR(key). */
    void Bind(ui::Control* control, const char* key)
    {
        if (control != nullptr) {
            Unbind(control);
            m_items.push_back({control, key, Kind::Text});
        }
    }

    /** Bind an edit's prompt text (placeholder) to a key. */
    void BindPrompt(ui::RichEdit* edit, const char* key)
    {
        if (edit != nullptr) {
            edit->SetAttribute("prompt_text", SDK_TR(key));
            m_items.push_back({edit, key, Kind::Prompt});
        }
    }

    /** Bind a tooltip to a key. */
    void BindToolTip(ui::Control* control, const char* key)
    {
        if (control != nullptr) {
            control->SetToolTipText(SDK_TR(key));
            m_items.push_back({control, key, Kind::ToolTip});
        }
    }

    /** Re-apply every bound string in the current language. */
    void Apply()
    {
        for (const Item& item : m_items) {
            switch (item.kind) {
            case Kind::Text:
                SetControlText(item.control, SDK_TR(item.key.c_str()));
                break;
            case Kind::Prompt:
                item.control->SetAttribute("prompt_text", SDK_TR(item.key.c_str()));
                break;
            case Kind::ToolTip:
                item.control->SetToolTipText(SDK_TR(item.key.c_str()));
                break;
            }
        }
    }

private:
    enum class Kind { Text, Prompt, ToolTip };
    struct Item {
        ui::Control* control;
        std::string key;
        Kind kind;
    };

    /** SetText lives on the concrete control classes, not on ui::Control. */
    static void SetControlText(ui::Control* control, const std::string& text)
    {
        if (auto* label = dynamic_cast<ui::Label*>(control)) {
            label->SetText(text);
            return;
        }
        if (auto* button = dynamic_cast<ui::Button*>(control)) {
            button->SetText(text);
            return;
        }
        if (auto* edit = dynamic_cast<ui::RichEdit*>(control)) {
            edit->SetText(text);
            return;
        }
        if (auto* option = dynamic_cast<ui::Option*>(control)) {
            option->SetText(text);
            return;
        }
        if (auto* checkbox = dynamic_cast<ui::CheckBox*>(control)) {
            checkbox->SetText(text);
            return;
        }
    }

    std::vector<Item> m_items;
};

} // namespace sdk

#endif // STARDESK_UI_LANG_BINDER_H_
