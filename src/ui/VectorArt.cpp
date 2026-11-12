#include "VectorArt.h"

#include "../app/Theme.h"

namespace sdk {

using ui::IRender;
using ui::UiColor;
using ui::UiPointF;
using ui::UiRect;
using ui::UiRectF;

namespace {

UiColor IconNormalColor() { return Theme::Get().textSub; }
UiColor IconHoverColor() { return Theme::Get().textMain; }
UiColor IconPressedColor() { return Theme::Get().accent; }

void DrawStar(IRender* pRender, const UiRect& rc, const UiColor& color)
{
    // 5-pointed star: 10 vertices alternating outer/inner radius
    const float cx = (float)rc.CenterX();
    const float cy = (float)rc.CenterY();
    const float outer = (float)rc.Width() * 0.5f;
    const float inner = outer * 0.45f;
    ui::UiPointF pts[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -90.0f + i * 36.0f; // start at top
        const float r = (i % 2 == 0) ? outer : inner;
        pts[i] = ui::UiPointF(cx + r * cosf(angle * 3.14159265f / 180.0f),
                              cy + r * sinf(angle * 3.14159265f / 180.0f));
    }
    ui::IRenderFactory* factory = ui::GlobalManager::Instance().GetRenderFactory();
    std::unique_ptr<ui::IPath> path(factory->CreatePath());
    path->AddPolygon(pts, 10);
    path->Close();
    std::unique_ptr<ui::IBrush> brush(factory->CreateBrush(color));
    pRender->FillPath(path.get(), brush.get());
}

} // namespace

// ---------------------------------------------------------------- VectorArt

void VectorArt::DrawLogo(IRender* pRender, const UiRect& rc, bool darkTheme)
{
    const Palette& p = Theme::Get();
    // glowing rounded disc behind the star
    const float r = (float)rc.Width() * 0.5f;
    const UiPointF center((float)rc.CenterX(), (float)rc.CenterY());
    UiColor disc = darkTheme ? UiColor(0xFF253049) : UiColor(0xFFE8EEFB);
    UiColor ring = darkTheme ? UiColor(0xFF33405F) : UiColor(0xFFC9D6EF);
    UiColor star = p.accent;

    pRender->FillCircle(center, r, disc, 255);
    pRender->DrawCircle(center, r, ring, 1.5f);

    UiRect starRect(rc.left + (int)(rc.Width() * 0.22f),
                    rc.top + (int)(rc.Height() * 0.22f),
                    rc.right - (int)(rc.Width() * 0.22f),
                    rc.bottom - (int)(rc.Height() * 0.22f));
    DrawStar(pRender, starRect, star);
}

void VectorArt::DrawIcon(IRender* pRender, Icon icon, const UiRect& rc,
                         const UiColor& color)
{
    ui::IRenderFactory* factory = ui::GlobalManager::Instance().GetRenderFactory();
    std::unique_ptr<ui::IPen> pen(factory->CreatePen(color, 1.6f));
    const float cx = (float)rc.CenterX();
    const float cy = (float)rc.CenterY();
    const float s = (float)rc.Width(); // icon box size
    const float m = s * 0.22f;         // margin
    const float half = s * 0.28f;      // half-length of strokes
    const float r = s * 0.30f;         // circle radius

    const UiPointF c(cx, cy);

    switch (icon) {
    case Icon::Close: {
        const float d = half * 0.8f;
        pRender->DrawLine(UiPointF(cx - d, cy - d), UiPointF(cx + d, cy + d), pen.get());
        pRender->DrawLine(UiPointF(cx + d, cy - d), UiPointF(cx - d, cy + d), pen.get());
        break;
    }
    case Icon::Min: {
        const float d = half * 0.8f;
        pRender->DrawLine(UiPointF(cx - d, cy), UiPointF(cx + d, cy), pen.get());
        break;
    }
    case Icon::Max: {
        const float d = half * 0.75f;
        pRender->DrawRect(UiRectF(cx - d, cy - d, cx + d, cy + d), color, 1.4f);
        break;
    }
    case Icon::Power: {
        // arc from 200° to -20° (opening at top) + vertical line
        pRender->DrawArc(UiRect((int)(cx - r), (int)(cy - r), (int)(cx + r), (int)(cy + r)),
                         200.0f, 140.0f, false, pen.get());
        pRender->DrawLine(UiPointF(cx, cy - half), UiPointF(cx, cy + half * 0.5f), pen.get());
        break;
    }
    case Icon::Folder: {
        std::unique_ptr<ui::IPath> path(factory->CreatePath());
        const float x0 = cx - half, y0 = cy - half, x1 = cx + half, y1 = cy + half;
        path->AddRect(UiRectF(x0, y0, x1, y1)); // folder body
        std::unique_ptr<ui::IBrush> brush(factory->CreateBrush(color));
        pRender->FillPath(path.get(), brush.get());
        // tab
        pRender->FillRect(UiRectF(x0 + s * 0.08f, y0, x1 - s * 0.08f, y0 + s * 0.18f),
                          color, 255);
        break;
    }
    case Icon::Fit:
    case Icon::Fullscreen: {
        // four corner brackets
        const float d = half * 0.7f;
        const float t = half;
        struct Corner { float x0, y0, x1, y1; int mode; };
        const Corner corners[4] = {
            { cx - t, cy - t, cx - t + d, cy - t, 0 }, // top-left
            { cx + t - d, cy - t, cx + t, cy - t, 0 }, // top-right
            { cx - t, cy + t - d, cx - t, cy + t, 1 }, // bottom-left
            { cx + t - d, cy + t, cx + t, cy + t, 1 }, // bottom-right
        };
        for (const Corner& k : corners) {
            if (k.mode == 0) {
                pRender->DrawLine(UiPointF(k.x0, k.y0), UiPointF(k.x1, k.y0), pen.get());
                pRender->DrawLine(UiPointF(k.x0, k.y0), UiPointF(k.x0, k.y1), pen.get());
            }
            else {
                pRender->DrawLine(UiPointF(k.x0, k.y1), UiPointF(k.x1, k.y1), pen.get());
                pRender->DrawLine(UiPointF(k.x1, k.y0), UiPointF(k.x1, k.y1), pen.get());
            }
        }
        break;
    }
    case Icon::Restore: {
        // two overlapping rounded rects
        pRender->DrawRect(UiRectF(cx - half * 0.8f, cy - half * 0.55f,
                                  cx + half * 0.25f, cy + half * 0.45f),
                          color, 1.4f);
        pRender->DrawRect(UiRectF(cx - half * 0.25f, cy - half * 0.45f,
                                  cx + half * 0.8f, cy + half * 0.55f),
                          color, 1.4f);
        break;
    }
    case Icon::Original: {
        const float d = half * 0.75f;
        pRender->DrawRect(UiRectF(cx - d, cy - d, cx + d, cy + d), color, 1.4f);
        pRender->FillCircle(UiPointF(cx, cy), 1.6f, color, 255);
        break;
    }
    case Icon::Eye: {
        // lens: ellipse + pupil
        std::unique_ptr<ui::IPath> path(factory->CreatePath());
        path->AddEllipse(UiRectF(cx - half, cy - half * 0.7f, cx + half, cy + half * 0.7f));
        pRender->DrawPath(path.get(), pen.get());
        pRender->FillCircle(c, half * 0.28f, color, 255);
        break;
    }
    case Icon::Random: {
        // two crossed arrows (dice/refresh feel)
        pRender->DrawLine(UiPointF(cx - half * 0.9f, cy + half * 0.5f),
                          UiPointF(cx + half * 0.5f, cy - half * 0.9f), pen.get());
        pRender->DrawLine(UiPointF(cx + half * 0.9f, cy - half * 0.9f),
                          UiPointF(cx + half * 0.5f, cy - half * 0.9f), pen.get());
        pRender->DrawLine(UiPointF(cx + half * 0.5f, cy - half * 0.9f),
                          UiPointF(cx + half * 0.5f, cy - half * 0.5f), pen.get());
        pRender->DrawLine(UiPointF(cx + half * 0.9f, cy - half * 0.5f),
                          UiPointF(cx - half * 0.5f, cy + half * 0.9f), pen.get());
        pRender->DrawLine(UiPointF(cx - half * 0.9f, cy + half * 0.9f),
                          UiPointF(cx - half * 0.5f, cy + half * 0.9f), pen.get());
        pRender->DrawLine(UiPointF(cx - half * 0.5f, cy + half * 0.9f),
                          UiPointF(cx - half * 0.5f, cy + half * 0.5f), pen.get());
        break;
    }
    case Icon::Refresh: {
        // circular arrow: arc with an arrowhead (refresh)
        const float r2 = half * 0.85f;
        pRender->DrawArc(UiRect((int)(cx - r2), (int)(cy - r2), (int)(cx + r2), (int)(cy + r2)),
                         70.0f, 290.0f, false, pen.get());
        // arrowhead at the arc end
        const float ax = cx + r2 * 0.94f, ay = cy - r2 * 0.34f;
        pRender->DrawLine(UiPointF(ax, ay), UiPointF(ax + half * 0.35f, ay - half * 0.25f), pen.get());
        pRender->DrawLine(UiPointF(ax, ay), UiPointF(ax + half * 0.42f, ay + half * 0.05f), pen.get());
        break;
    }
    case Icon::Gear: {
        pRender->DrawCircle(c, r * 0.55f, color, 1.8f);
        pRender->FillCircle(c, r * 0.30f, color, 255);
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i * 45.0f * 3.14159265f / 180.0f;
            const float cosA = cosf(a), sinA = sinf(a);
            pRender->DrawLine(UiPointF(cx + r * 0.75f * cosA, cy + r * 0.75f * sinA),
                              UiPointF(cx + r * 1.0f * cosA, cy + r * 1.0f * sinA), pen.get());
        }
        break;
    }
    case Icon::Moon: {
        pRender->FillCircle(c, r * 0.9f, color, 255);
        pRender->FillCircle(UiPointF(cx + r * 0.45f, cy - r * 0.30f), r * 0.78f,
                            Theme::Get().windowBg, 255);
        break;
    }
    case Icon::Sun: {
        pRender->FillCircle(c, r * 0.55f, color, 255);
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i * 45.0f * 3.14159265f / 180.0f;
            const float cosA = cosf(a), sinA = sinf(a);
            pRender->DrawLine(UiPointF(cx + r * 0.8f * cosA, cy + r * 0.8f * sinA),
                              UiPointF(cx + r * 1.15f * cosA, cy + r * 1.15f * sinA), pen.get());
        }
        break;
    }
    case Icon::Check: {
        const float d = half * 0.75f;
        pRender->DrawLine(UiPointF(cx - d, cy), UiPointF(cx - d * 0.2f, cy + d * 0.6f), pen.get());
        pRender->DrawLine(UiPointF(cx - d * 0.2f, cy + d * 0.6f), UiPointF(cx + d, cy - d * 0.6f), pen.get());
        break;
    }
    case Icon::Cross: {
        const float d = half * 0.75f;
        pRender->DrawLine(UiPointF(cx - d, cy - d), UiPointF(cx + d, cy + d), pen.get());
        pRender->DrawLine(UiPointF(cx + d, cy - d), UiPointF(cx - d, cy + d), pen.get());
        break;
    }
    case Icon::Plus: {
        const float d = half * 0.7f;
        pRender->DrawLine(UiPointF(cx - d, cy), UiPointF(cx + d, cy), pen.get());
        pRender->DrawLine(UiPointF(cx, cy - d), UiPointF(cx, cy + d), pen.get());
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- IconButton

IconButton::IconButton(ui::Window* pWindow) : BaseClass(pWindow)
{
    SetMouseEnabled(true);
}

void IconButton::SetIconToolTip(const std::string& text)
{
    SetToolTipText(text);
}

void IconButton::Paint(IRender* pRender, const UiRect& rcPaint)
{
    BaseClass::Paint(pRender, rcPaint);

    const Palette& p = Theme::Get();
    const UiRect rc = GetRect();
    if (rc.Width() <= 0 || rc.Height() <= 0) {
        return;
    }

    // hover / press background
    const ui::ControlStateType state = GetState();
    const float bgRadius = std::min(rc.Width(), rc.Height()) / 2.0f;
    if (state == ui::kControlStateHot) {
        pRender->FillRoundRect(UiRectF::MakeFromRect(rc), bgRadius, bgRadius, p.hoverBg, 255);
    }
    else if (state == ui::kControlStatePushed) {
        pRender->FillRoundRect(UiRectF::MakeFromRect(rc), bgRadius, bgRadius, p.pressedBg, 255);
    }

    UiColor color = IconNormalColor();
    if (state == ui::kControlStateHot) {
        color = IconHoverColor();
    }
    else if (state == ui::kControlStatePushed) {
        color = IconPressedColor();
    }
    VectorArt::DrawIcon(pRender, m_icon, rc, color);
}

// ---------------------------------------------------------------- AccentButton

AccentButton::AccentButton(ui::Window* pWindow)
    : BaseClass(pWindow), m_textColor(0xFFFFFFFF)
{
    SetMouseEnabled(true);
    SetTextPadding(ui::UiPadding(16, 0, 16, 0), true);
}

void AccentButton::Paint(IRender* pRender, const UiRect& rcPaint)
{
    // state colors come from the theme instead of a class
    const Palette& p = Theme::Get();
    const UiRect rc = GetRect();
    const ui::ControlStateType state = GetState();
    UiColor bg = m_danger ? p.danger : p.accent;
    if (state == ui::kControlStateHot) {
        bg = m_danger ? p.danger : p.accentHover;
    }
    else if (state == ui::kControlStatePushed) {
        bg = m_danger ? p.danger : p.accentPressed;
    }
    else if (!IsEnabled()) {
        bg = p.border;
    }
    pRender->FillRoundRect(UiRectF::MakeFromRect(rc), 8.0f, 8.0f, bg, 255);
    PaintText(pRender, rcPaint);
}

void AccentButton::PaintText(IRender* pRender, const UiRect& rcPaint)
{
    // draw the label centered with the accent-appropriate text color
    const std::string text = GetText();
    if (text.empty()) {
        return;
    }
    ui::DrawStringParam param;
    param.textRect = GetRect();
    param.dwTextColor = m_textColor;
    param.pFont = GetIFontById("system_bold_12");
    param.uFormat = ui::TEXT_HCENTER | ui::TEXT_VCENTER;
    pRender->DrawString(text, param);
}

// ---------------------------------------------------------------- PillButton

PillButton::PillButton(ui::Window* pWindow) : BaseClass(pWindow)
{
    SetMouseEnabled(true);
    SetAttribute("text_align", "hcenter,vcenter");
    SetAttribute("font", "system_12");
    // keep the label clear of the rounded ends (the chip is auto-width)
    SetTextPadding(ui::UiPadding(14, 0, 14, 0), true);
}

void PillButton::Paint(IRender* pRender, const UiRect& rcPaint)
{
    const Palette& p = Theme::Get();
    const UiRect rc = GetRect();
    if (rc.Width() <= 0 || rc.Height() <= 0) {
        return;
    }
    const ui::ControlStateType state = GetState();
    UiColor bg = p.panelBg;
    UiColor border = p.border;
    if (m_selected) {
        bg = (state == ui::kControlStateHot)   ? p.accentHover
             : (state == ui::kControlStatePushed) ? p.accentPressed
                                                  : p.accent;
        border = bg;
    }
    else if (state == ui::kControlStateHot) {
        bg = p.hoverBg;
        border = p.border;
    }
    else if (state == ui::kControlStatePushed) {
        bg = p.pressedBg;
    }
    // rounded-rect chip: a full pill radius made the ends swallow the label
    const float radius = std::min(rc.Height() / 2.0f, 8.0f);
    pRender->FillRoundRect(UiRectF::MakeFromRect(rc), radius, radius, bg, 255);
    pRender->DrawRoundRect(UiRectF::MakeFromRect(rc), radius, radius, border, 1.0f);
    PaintText(pRender, rcPaint);
}

void PillButton::PaintText(IRender* pRender, const UiRect& rcPaint)
{
    const std::string text = GetText();
    if (text.empty()) {
        return;
    }
    const Palette& p = Theme::Get();
    ui::DrawStringParam param;
    param.textRect = GetRect();
    param.dwTextColor = m_selected ? p.textInvert : p.textMain;
    param.pFont = GetIFontById("system_12");
    param.uFormat = ui::TEXT_HCENTER | ui::TEXT_VCENTER;
    pRender->DrawString(text, param);
}

// ---------------------------------------------------------------- ProgressBar

ProgressBar::ProgressBar(ui::Window* pWindow) : BaseClass(pWindow)
{
    SetMouseEnabled(false);
}

void ProgressBar::Paint(ui::IRender* pRender, const UiRect& rcPaint)
{
    const Palette& p = Theme::Get();
    const UiRect rc = GetRect();
    const float barRadius = rc.Height() / 2.0f;
    pRender->FillRoundRect(UiRectF::MakeFromRect(rc), barRadius, barRadius, p.progressBg, 255);
    if (m_ratio > 0.0) {
        const int w = std::max(2, (int)(rc.Width() * m_ratio));
        UiRect fill(rc.left, rc.top, rc.left + w, rc.bottom);
        pRender->FillRoundRect(UiRectF::MakeFromRect(fill), barRadius, barRadius,
                               p.progressFg, 255);
    }
}

// ---------------------------------------------------------------- CardBox

CardBox::CardBox(ui::Window* pWindow) : BaseClass(pWindow)
{
}

void CardBox::Paint(IRender* pRender, const UiRect& rcPaint)
{
    const Palette& p = Theme::Get();
    const UiRect rc = GetRect();
    pRender->FillRoundRect(UiRectF::MakeFromRect(rc), 12.0f, 12.0f, p.panelBg, 255);
    pRender->DrawRoundRect(UiRectF::MakeFromRect(rc), 12.0f, 12.0f, p.border, 1.0f);
    BaseClass::Paint(pRender, rcPaint);
}

// ---------------------------------------------------------------- CardHBox

CardHBox::CardHBox(ui::Window* pWindow) : BaseClass(pWindow)
{
}

void CardHBox::Paint(ui::IRender* pRender, const ui::UiRect& rcPaint)
{
    const Palette& p = Theme::Get();
    const ui::UiRect rc = GetRect();
    const bool hot = GetState() == ui::kControlStateHot;
    pRender->FillRoundRect(UiRectF::MakeFromRect(rc), 10.0f, 10.0f,
                           hot ? p.listHover : p.panelBg, 255);
    pRender->DrawRoundRect(UiRectF::MakeFromRect(rc), 10.0f, 10.0f, p.border, 1.0f);
    BaseClass::Paint(pRender, rcPaint);
}

// ---------------------------------------------------------------- ThemeLabel

ThemeLabel::ThemeLabel(ui::Window* pWindow) : BaseClass(pWindow)
{
    SetAttribute("font", "system_12");
    SetMouseEnabled(false);
}

void ThemeLabel::ApplyRoleColor()
{
    const char* colorName = "sd_text_main";
    switch (m_role) {
    case Role::Sub:    colorName = "sd_text_sub"; break;
    case Role::Title:  colorName = "sd_text_sub"; break;
    case Role::Accent: colorName = "sd_accent"; break;
    case Role::Success: colorName = "sd_success"; break;
    case Role::Danger: colorName = "sd_danger"; break;
    default: break;
    }
    const uint32_t key = (uint32_t)m_role + 1;
    if (key == m_lastColor) {
        return;
    }
    m_lastColor = key;
    // "textcolor" is the attribute dui's LabelImpl uses for the paint colour
    const_cast<ThemeLabel*>(this)->SetAttribute("normal_text_color", colorName);
}

void ThemeLabel::Paint(ui::IRender* pRender, const UiRect& rcPaint)
{
    ApplyRoleColor();
    BaseClass::Paint(pRender, rcPaint);
}

void ThemeLabel::PaintText(IRender* pRender, const UiRect& rcPaint)
{
    const std::string text = GetText();
    if (text.empty()) {
        return;
    }
    const Palette& p = Theme::Get();
    ui::UiColor color = p.textMain;
    switch (m_role) {
    case Role::Sub:    color = p.textSub; break;
    case Role::Title:  color = p.textSub; break;
    case Role::Accent: color = p.accent; break;
    case Role::Success: color = p.success; break;
    case Role::Danger: color = p.danger; break;
    default: break;
    }

    ui::DrawStringParam param;
    param.textRect = GetRect();
    param.dwTextColor = color;
    param.pFont = GetIFontById(GetFontId().empty() ? "system_12" : GetFontId());
    param.uFormat = ui::TEXT_LEFT | ui::TEXT_VCENTER;
    pRender->DrawString(text, param);
}

} // namespace sdk
