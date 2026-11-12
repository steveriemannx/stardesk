#ifndef STARDESK_APP_LANGUAGE_H_
#define STARDESK_APP_LANGUAGE_H_

#include "dui/dui.h"

namespace sdk {

/** Built-in languages. Order matters: it is the index stored in the config
 *  and the order of the language picker.
 */
enum class Lang : int {
    ZhCN = 0, // 简体中文
    ZhTW = 1, // 繁體中文
    En = 2,   // English
    Ja = 3,   // 日本語
    Ko = 4,   // 한국어
    De = 5,   // Deutsch
    Fr = 6,   // Français
    Ru = 7,   // Русский
    Count = 8,
};

/** Tiny built-in i18n: UI strings are looked up by an English key through
 *  Tr(key). No resource files involved - the 8 translations are compiled in.
 *  Language switch is applied immediately by every window's ApplyLanguage().
 */
class Language {
public:
    static void Set(Lang lang);
    static Lang Get() { return s_lang; }

    /** Native name of a language (shown in the picker). */
    static std::string NativeName(Lang lang);
    /** Short code, e.g. "zh_CN", "en_US". */
    static const char* Code(Lang lang);
    /** Number of languages. */
    static int Count() { return (int)Lang::Count; }

    /** Look up a UI string in the current language; falls back to English,
     *  then to the key itself. Returns a wide (std::string) string.
     */
    static std::string Tr(const char* key);

    /** Tr() as a UTF-8 std::string (protocol/beacon payloads). */
    static std::string TrUtf8(const char* key);

private:
    static Lang s_lang;
};

/** Convenience macro for UI code: Tr("key"). */
#define SDK_TR(key) (::sdk::Language::Tr(key))

} // namespace sdk

#endif // STARDESK_APP_LANGUAGE_H_
