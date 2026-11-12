#ifndef STARDESK_UI_VECTOR_ART_H_
#define STARDESK_UI_VECTOR_ART_H_

#include "dui/dui.h"

namespace sdk {

/** Code-drawn vector assets: the StarDesk logo and the toolbar icons.
 *  Everything is painted with IRender primitives (no image files).
 */
class VectorArt {
public:
    /** Draw the StarDesk logo (a 5-pointed star in a rounded square/glow
     *  disc) centered in rc. Pass accent/star colors, or 0 to use theme.
     */
    static void DrawLogo(ui::IRender* pRender, const ui::UiRect& rc,
                         bool darkTheme);

    /** Icons drawn with IRender primitives; theme-aware colors. */
    enum class Icon {
        Power,      // disconnect / power
        Folder,     // send files
        Fit,        // fit window
        Original,   // 1:1
        Fullscreen, // enter fullscreen
        Restore,    // exit fullscreen
        Eye,        // view-only badge
        Close,
        Min,
        Max,
        Random,     // dice-ish regenerate password
        Refresh,    // circular arrow (refresh)
        Gear,       // settings
        Moon,       // dark theme
        Sun,        // light theme
        Check,
        Cross,
        Plus,
    };

    static void DrawIcon(ui::IRender* pRender, Icon icon, const ui::UiRect& rc,
                         const ui::UiColor& color);
};

/** A small self-drawn icon button: paints a VectorArt icon in its Paint(),
 *  tracks hover/press state, fires AttachClick like a normal button.
 *  Derives from ui::Button (not Control) so dui's caption-button binding
 *  (min/max/close) and the standard click behavior work.
 */
class IconButton : public ui::Button
{
    typedef ui::Button BaseClass;
public:
    explicit IconButton(ui::Window* pWindow);

    void SetIcon(VectorArt::Icon icon) { m_icon = icon; }
    VectorArt::Icon GetIcon() const { return m_icon; }

    /** Tooltip text (shown on hover by dui). */
    void SetIconToolTip(const std::string& text);

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;

private:
    VectorArt::Icon m_icon = VectorArt::Icon::Close;
};

/** Theme-aware accent button (rounded rect + centered text). */
class AccentButton : public ui::Button
{
    typedef ui::Button BaseClass;
public:
    explicit AccentButton(ui::Window* pWindow);

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;
    void PaintText(ui::IRender* pRender, const ui::UiRect& rcPaint);

    void SetAccentTextColor(const ui::UiColor& c) { m_textColor = c; }
    /** Destructive styling: use the theme's danger colour (e.g. 断开). */
    void SetDangerLook(bool danger) { m_danger = danger; }

private:
    ui::UiColor m_textColor;
    bool m_danger = false;
};

/** A pill-shaped toggle button, fully code-drawn: normal = panel bg + border,
 *  selected = accent background. Used for option groups and tabs.
 */
class PillButton : public ui::Button
{
    typedef ui::Button BaseClass;
public:
    explicit PillButton(ui::Window* pWindow);

    void SetSelected(bool selected) { m_selected = selected; }

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;
    void PaintText(ui::IRender* pRender, const ui::UiRect& rcPaint);

private:
    bool m_selected = false;
};

/** Simple code-drawn horizontal progress bar (theme-aware). */
class ProgressBar : public ui::Control
{
    typedef ui::Control BaseClass;
public:
    explicit ProgressBar(ui::Window* pWindow);

    void SetProgress(double ratio) { m_ratio = ratio; Invalidate(); }
    double GetProgress() const { return m_ratio; }

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;

private:
    double m_ratio = 0.0;
};

/** A rounded panel container (info cards); background/border are painted
 *  with the current theme at paint time.
 */
class CardBox : public ui::VBox
{
    typedef ui::VBox BaseClass;
public:
    explicit CardBox(ui::Window* pWindow);
    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;
};

/** A rounded horizontal panel container (one-line info rows); background and
 *  border are painted with the current theme at paint time.
 */
class CardHBox : public ui::HBox
{
    typedef ui::HBox BaseClass;
public:
    explicit CardHBox(ui::Window* pWindow);
    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;

    /** Selected row: highlighted background + accent border. */
    void SetSelected(bool selected) { m_selected = selected; }
    bool IsSelected() const { return m_selected; }

private:
    bool m_selected = false;
};

/** A label whose text color is picked from the theme at paint time by role,
 *  so theme switches apply without re-setting control properties.
 */
class ThemeLabel : public ui::Label
{
    typedef ui::Label BaseClass;
public:
    enum class Role {
        Main,   // primary text
        Sub,    // secondary text
        Title,  // card title (sub, small)
        Accent, // accent color
        Success,
        Danger,
    };

    explicit ThemeLabel(ui::Window* pWindow);

    void SetRole(Role role) { m_role = role; m_lastColor = 0; }
    Role GetRole() const { return m_role; }

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;
    void PaintText(ui::IRender* pRender, const ui::UiRect& rcPaint);

private:
    /** Push the role color into dui's label painter (it keeps alignment,
     *  multi-line handling and ellipsis, which the hand-rolled text drawing
     *  below does not). */
    void ApplyRoleColor();
    Role m_role = Role::Main;
    uint32_t m_lastColor = 0;
};

} // namespace sdk

#endif // STARDESK_UI_VECTOR_ART_H_
