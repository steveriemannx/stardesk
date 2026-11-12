#include "TextProbe.h"

#include "app/AppConfig.h"
#include "app/Language.h"

#include "dui/dui.h"
#include "dui/RenderSkia/HorizontalDrawText.h"
#include "dui/RenderSkia/Font_Skia.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkFont.h"

#include <cstdio>
#include <string>
#include <cstdint>
#include <vector>

namespace sdk {

void RunTextProbe(const std::string& path)
{
    using namespace ui;

    // report which configured UI font families are actually installed, and
    // which typeface dui ends up using as the default
    {
        ui::IFontMgr* pFontMgr = GlobalManager::Instance().GetRenderFactory() != nullptr
            ? GlobalManager::Instance().GetRenderFactory()->GetFontMgr()
            : nullptr;
        if (pFontMgr != nullptr) {
            const char* candidates[] = {"PingFang SC", "Microsoft YaHei", "Noto Sans CJK SC",
                                        "Helvetica Neue", "Arial", "Arial Unicode MS",
                                        "Hiragino Sans GB", "Heiti SC", "Apple SD Gothic Neo",
                                        "sans-serif"};
            for (const char* name : candidates) {
                fprintf(stderr, "[probe] font \"%s\": %s\n", name,
                        pFontMgr->HasFontName(name) ? "installed" : "MISSING");
            }
        }
        IFont* pDefaultFont = GlobalManager::Instance().Font().GetIFont("system_12", 100);
        auto* pSkiaFont = dynamic_cast<ui::Font_Skia*>(pDefaultFont);
        if (pSkiaFont != nullptr) {
            const SkFont* pSkFont = pSkiaFont->GetFontHandle();
            if (pSkFont != nullptr) {
                sk_sp<SkTypeface> typeface =
                    pSkFont->getTypeface() ? sk_ref_sp(pSkFont->getTypeface()) : nullptr;
                if (typeface) {
                    SkString familyName;
                    typeface->getFamilyName(&familyName);
                    fprintf(stderr, "[probe] default typeface family: %s\n", familyName.c_str());
                }
            }
        }
    }

    IFont* pFont = GlobalManager::Instance().Font().GetIFont("system_16", 100);
    if (pFont == nullptr) {
        fprintf(stderr, "[probe] no system_16 font\n");
        return;
    }

    constexpr int kWidth = 980;
    constexpr int kHeight = 560;
    SkImageInfo info = SkImageInfo::MakeN32Premul(kWidth, kHeight);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (surface == nullptr) {
        fprintf(stderr, "[probe] SkSurface::MakeRaster failed\n");
        return;
    }
    SkCanvas* pCanvas = surface->getCanvas();
    pCanvas->clear(SK_ColorWHITE);

    SkPaint paint;
    paint.setColor(SK_ColorBLACK);
    paint.setAntiAlias(true);
    SkPoint ptOrg = SkPoint::Make(0.0f, 0.0f);
    HorizontalDrawText drawText(pCanvas, &paint, &ptOrg);

    const std::vector<std::string> lines = {
        "StarDesk UI font fallback probe",
        "简体中文  繁體中文  English  日本語",
        "한국어  Deutsch  Français  Русский",
        "远程控制 / 遠端控制 / 원격 제어 / Удаленный рабочий стол",
        "Кириллица: Съешь же ещё этих мягких французских булок",
        "Hangul: 다람쥐 헌 쳇바퀴에 타고파",
        "Kana: いろはにほへと ちりぬるを",
    };

    int y = 16;
    for (const std::string& line : lines) {
        DrawStringParam param;
        param.textRect = UiRect(20, y, kWidth - 20, y + 40);
        param.dwTextColor = UiColor(0xFF111827);
        param.pFont = pFont;
        param.uFormat = TEXT_SINGLELINE | TEXT_LEFT | TEXT_VCENTER;
        drawText.DrawString(line, param);

        MeasureStringParam measureParam;
        measureParam.rectSize = kWidth - 40;
        measureParam.pFont = pFont;
        measureParam.uFormat = TEXT_SINGLELINE;
        const UiRect measured = drawText.MeasureString(line, measureParam);
        fprintf(stderr, "[probe] width=%d \"%s\"\n", measured.Width(), line.c_str());
        y += 70;
    }

    // self-consistency check: measure a string, then draw it into exactly that
    // width - if the renderer still ellipsizes, measure and draw disagree
    {
        const char* samples[] = {"连接密码", "远程控制", "状态", "密码", "本机设备", "连接方式"};
        int sampleY = 16 + 70 * (int)lines.size();
        for (const char* sample : samples) {
            MeasureStringParam mp;
            mp.rectSize = 4000;
            mp.pFont = pFont;
            mp.uFormat = TEXT_SINGLELINE | TEXT_END_ELLIPSIS;
            const UiRect measured = drawText.MeasureString(sample, mp);
            fprintf(stderr, "[probe] measure \"%s\" = %d x %d\n", sample,
                    measured.Width(), measured.Height());
            DrawStringParam dp;
            dp.textRect = UiRect(20, sampleY, 20 + measured.Width(), sampleY + 40);
            dp.dwTextColor = UiColor(0xFF111827);
            dp.pFont = pFont;
            dp.uFormat = TEXT_SINGLELINE | TEXT_LEFT | TEXT_VCENTER | TEXT_END_ELLIPSIS;
            drawText.DrawString(sample, dp);
            sampleY += 50;
        }
    }

    std::vector<uint8_t> pixels((size_t)kWidth * kHeight * 4);
    if (!surface->readPixels(info, pixels.data(), kWidth * 4, 0, 0)) {
        fprintf(stderr, "[probe] readPixels failed\n");
        return;
    }
    FILE* pFile = std::fopen(path.c_str(), "wb");
    if (pFile == nullptr) {
        fprintf(stderr, "[probe] cannot write %s\n", path.c_str());
        return;
    }
    const uint32_t header[2] = {(uint32_t)kWidth, (uint32_t)kHeight};
    std::fwrite(header, sizeof(uint32_t), 2, pFile);
    std::fwrite(pixels.data(), 1, pixels.size(), pFile);
    std::fclose(pFile);
    fprintf(stderr, "[probe] saved %s (%dx%d)\n", path.c_str(), kWidth, kHeight);
}

} // namespace sdk

namespace sdk {

namespace {

void DumpControl(ui::Control* pControl, int depth, FILE* pFile)
{
    if (pControl == nullptr || pFile == nullptr) {
        return;
    }
    const ui::UiRect rc = pControl->GetRect();
    std::string text;
    if (auto* pLabel = dynamic_cast<ui::Label*>(pControl)) {
        text = pLabel->GetText();
    }
    const std::string name = pControl->GetName();
    const bool visible = pControl->IsVisible();
    fprintf(pFile, "%*s%s%s%s rect=%d,%d %dx%d%s%s%s\n", depth * 2, "",
            pControl->GetType().c_str(), name.empty() ? "" : " name=", name.c_str(),
            rc.left, rc.top, rc.Width(), rc.Height(),
            visible ? "" : " HIDDEN", text.empty() ? "" : " text=\"",
            text.empty() ? "" : text.c_str());
    if (!text.empty()) {
        fprintf(pFile, "%*s\"\n", 0, "");
    }
    auto* pBox = dynamic_cast<ui::Box*>(pControl);
    if (pBox != nullptr) {
        for (size_t i = 0; i < pBox->GetItemCount(); ++i) {
            DumpControl(pBox->GetItemAt(i), depth + 1, pFile);
        }
    }
}

} // namespace

void DumpWindowTreeToFile(ui::Window* pWindow, const std::string& path)
{
    if (pWindow == nullptr) {
        return;
    }
    FILE* pFile = std::fopen(path.c_str(), "w");
    if (pFile == nullptr) {
        return;
    }
    ui::UiRect rcClient;
    pWindow->GetClientRect(rcClient);
    fprintf(pFile, "window %s client=%dx%d\n", pWindow->GetWindowId().c_str(),
            rcClient.Width(), rcClient.Height());
    ui::Box* pRoot = pWindow->GetRoot();
    if (pRoot != nullptr) {
        DumpControl(pRoot, 0, pFile);
    }
    std::fclose(pFile);
}

bool CaptureWindowPixels(ui::Window* pWindow, const std::string& path)
{
    if (pWindow == nullptr) {
        return false;
    }
    ui::IRender* pRender = pWindow->GetRender();
    if (pRender == nullptr) {
        return false;
    }
    ui::UiRect rcClient;
    pWindow->GetClientRect(rcClient);
    const int32_t width = rcClient.Width();
    const int32_t height = rcClient.Height();
    if (width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels((size_t)width * height * 4);
    if (!pRender->ReadPixels(ui::UiRect(0, 0, width, height), pixels.data(), pixels.size())) {
        return false;
    }
    FILE* pFile = std::fopen(path.c_str(), "wb");
    if (pFile == nullptr) {
        return false;
    }
    const uint32_t header[2] = {(uint32_t)width, (uint32_t)height};
    std::fwrite(header, sizeof(uint32_t), 2, pFile);
    std::fwrite(pixels.data(), 1, pixels.size(), pFile);
    std::fclose(pFile);
    return true;
}

} // namespace sdk
