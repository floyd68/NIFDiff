#include "NifCompareControlPanel.h"
#include <Button.h>
#include <Slider.h>
#include <CheckBox.h>
#include <ComboBox.h>
#include <Text.h>

namespace nsk
{

NifCompareControlPanel::NifCompareControlPanel(const std::wstring& name)
    : FD2D::StackPanel(name, FD2D::Orientation::Vertical)
{
    SetSpacing(4.0f);

    // Row 1: whole-view actions + view/display toggles.
    auto row1 = std::make_shared<FD2D::StackPanel>(name + L"_Row1", FD2D::Orientation::Horizontal);
    row1->SetSpacing(10.0f);

    m_addPaneBtn = std::make_shared<FD2D::Button>(L"AddPane");
    m_addPaneBtn->SetLabel(L"+ Add Pane");
    row1->AddChild(m_addPaneBtn);

    m_resetBtn = std::make_shared<FD2D::Button>(L"ResetCameras");
    m_resetBtn->SetLabel(L"Center / Reset View");
    row1->AddChild(m_resetBtn);

    m_syncViewsChk = std::make_shared<FD2D::CheckBox>(L"SyncViews");
    m_syncViewsChk->SetLabel(L"Sync Views (Camera)");
    m_syncViewsChk->SetChecked(true);
    row1->AddChild(m_syncViewsChk);

    m_syncLightingChk = std::make_shared<FD2D::CheckBox>(L"SyncLighting");
    m_syncLightingChk->SetLabel(L"Sync Lighting");
    m_syncLightingChk->SetChecked(true);
    row1->AddChild(m_syncLightingChk);

    m_orientationCombo = std::make_shared<FD2D::ComboBox>(L"Orientation");
    m_orientationCombo->SetItems({ L"Front", L"Back", L"Left", L"Right", L"Top", L"Bottom" });
    m_orientationCombo->SetSelectedIndex(0);
    row1->AddChild(m_orientationCombo);

    m_frontalLightChk = std::make_shared<FD2D::CheckBox>(L"FrontalLight");
    m_frontalLightChk->SetLabel(L"Frontal Light");
    row1->AddChild(m_frontalLightChk);

    m_showGridChk = std::make_shared<FD2D::CheckBox>(L"ShowGrid");
    m_showGridChk->SetLabel(L"Show Grid");
    m_showGridChk->SetChecked(true);
    row1->AddChild(m_showGridChk);

    m_showAxesChk = std::make_shared<FD2D::CheckBox>(L"ShowAxes");
    m_showAxesChk->SetLabel(L"Show Axes");
    m_showAxesChk->SetChecked(true);
    row1->AddChild(m_showAxesChk);

    m_wireframeChk = std::make_shared<FD2D::CheckBox>(L"Wireframe");
    m_wireframeChk->SetLabel(L"Wireframe");
    row1->AddChild(m_wireframeChk);

    AddChild(row1);

    // Row 2: lighting sliders.
    auto row2 = std::make_shared<FD2D::StackPanel>(name + L"_Row2", FD2D::Orientation::Horizontal);
    row2->SetSpacing(14.0f);

    m_brightnessSlider = std::make_shared<FD2D::Slider>(L"Brightness");
    m_brightnessSlider->SetLabel(L"Brightness");
    m_brightnessSlider->SetRange(0.0f, 3.0f);
    m_brightnessSlider->SetValue(1.0f);
    row2->AddChild(m_brightnessSlider);

    m_ambientSlider = std::make_shared<FD2D::Slider>(L"Ambient");
    m_ambientSlider->SetLabel(L"Ambient");
    m_ambientSlider->SetRange(0.0f, 1.0f);
    m_ambientSlider->SetValue(0.35f);
    row2->AddChild(m_ambientSlider);

    m_declinationSlider = std::make_shared<FD2D::Slider>(L"Declination");
    m_declinationSlider->SetLabel(L"Declination");
    m_declinationSlider->SetRange(-90.0f, 90.0f);
    m_declinationSlider->SetValue(45.0f);
    row2->AddChild(m_declinationSlider);

    m_planarAngleSlider = std::make_shared<FD2D::Slider>(L"PlanarAngle");
    m_planarAngleSlider->SetLabel(L"Planar Angle");
    m_planarAngleSlider->SetRange(-180.0f, 180.0f);
    m_planarAngleSlider->SetValue(45.0f);
    row2->AddChild(m_planarAngleSlider);

    // POM height budget: HeightScale in CS terms (0.1*scale UV depth).
    // Starts disabled - NifCompareView enables it when a loaded NIF has
    // parallax-active materials.
    m_parallaxSlider = std::make_shared<FD2D::Slider>(L"ParallaxHeight");
    m_parallaxSlider->SetLabel(L"Parallax Height");
    m_parallaxSlider->SetRange(0.0f, 5.0f);
    m_parallaxSlider->SetValue(2.0f);
    m_parallaxSlider->SetEnabled(false);
    row2->AddChild(m_parallaxSlider);

    // Extended-material toggles: off = the closest legacy interpretation
    // (no POM, complex materials as plain env masks, PBR through the
    // vanilla lit path) - handy for before/after comparisons. Each starts
    // disabled like the parallax slider; NifCompareView enables one while
    // some loaded NIF carries material that toggle would affect.
    m_parallaxChk = std::make_shared<FD2D::CheckBox>(L"EnableParallax");
    m_parallaxChk->SetLabel(L"Parallax");
    m_parallaxChk->SetChecked(true);
    m_parallaxChk->SetEnabled(false);
    row2->AddChild(m_parallaxChk);

    m_complexMaterialChk = std::make_shared<FD2D::CheckBox>(L"EnableComplexMaterial");
    m_complexMaterialChk->SetLabel(L"Complex Mat");
    m_complexMaterialChk->SetChecked(true);
    m_complexMaterialChk->SetEnabled(false);
    row2->AddChild(m_complexMaterialChk);

    m_pbrChk = std::make_shared<FD2D::CheckBox>(L"EnablePBR");
    m_pbrChk->SetLabel(L"True PBR");
    m_pbrChk->SetChecked(true);
    m_pbrChk->SetEnabled(false);
    row2->AddChild(m_pbrChk);

    AddChild(row2);

    // Row 3: resources (Game Data / Overrides).
    auto row3 = std::make_shared<FD2D::StackPanel>(name + L"_Row3", FD2D::Orientation::Horizontal);
    row3->SetSpacing(10.0f);

    m_gameDataLabel = std::make_shared<FD2D::Text>(L"GameDataLabel");
    m_gameDataLabel->SetText(L"Game Data: (not set)");
    m_gameDataLabel->SetFont(L"Segoe UI", 13.0f);
    m_gameDataLabel->SetFixedWidth(260.0f);
    m_gameDataLabel->SetEllipsisTrimmingEnabled(true);
    row3->AddChild(m_gameDataLabel);

    m_browseGameDataBtn = std::make_shared<FD2D::Button>(L"BrowseGameData");
    m_browseGameDataBtn->SetLabel(L"Browse Game Data...");
    row3->AddChild(m_browseGameDataBtn);

    m_detectGameDataBtn = std::make_shared<FD2D::Button>(L"DetectGameData");
    m_detectGameDataBtn->SetLabel(L"Detect Game Data");
    row3->AddChild(m_detectGameDataBtn);

    m_overrideLabel = std::make_shared<FD2D::Text>(L"OverrideLabel");
    m_overrideLabel->SetText(L"Overrides: 0 folder(s)");
    m_overrideLabel->SetFont(L"Segoe UI", 13.0f);
    row3->AddChild(m_overrideLabel);

    m_addOverrideBtn = std::make_shared<FD2D::Button>(L"AddOverride");
    m_addOverrideBtn->SetLabel(L"Add Override Folder...");
    row3->AddChild(m_addOverrideBtn);

    m_clearOverridesBtn = std::make_shared<FD2D::Button>(L"ClearOverrides");
    m_clearOverridesBtn->SetLabel(L"Clear Overrides");
    row3->AddChild(m_clearOverridesBtn);

    AddChild(row3);
}

void NifCompareControlPanel::SetOnAddPane(std::function<void()> handler) { m_addPaneBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnResetCameras(std::function<void()> handler) { m_resetBtn->OnClick(std::move(handler)); }

void NifCompareControlPanel::SetOnSyncViewsChanged(std::function<void(bool)> handler) { m_syncViewsChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnSyncLightingChanged(std::function<void(bool)> handler) { m_syncLightingChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnOrientationChanged(std::function<void(int)> handler) { m_orientationCombo->OnSelectionChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnFrontalLightChanged(std::function<void(bool)> handler) { m_frontalLightChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowGridChanged(std::function<void(bool)> handler) { m_showGridChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowAxesChanged(std::function<void(bool)> handler) { m_showAxesChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnWireframeChanged(std::function<void(bool)> handler) { m_wireframeChk->OnCheckedChanged(std::move(handler)); }

void NifCompareControlPanel::SetOnBrightnessChanged(std::function<void(float)> handler) { m_brightnessSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnAmbientChanged(std::function<void(float)> handler) { m_ambientSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnDeclinationChanged(std::function<void(float)> handler) { m_declinationSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnPlanarAngleChanged(std::function<void(float)> handler) { m_planarAngleSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnParallaxHeightChanged(std::function<void(float)> handler) { m_parallaxSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetParallaxHeightEnabled(bool enabled) { m_parallaxSlider->SetEnabled(enabled); }
float NifCompareControlPanel::ParallaxHeightValue() const { return m_parallaxSlider->Value(); }
void NifCompareControlPanel::SetOnParallaxEnabledChanged(std::function<void(bool)> handler) { m_parallaxChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnComplexMaterialEnabledChanged(std::function<void(bool)> handler) { m_complexMaterialChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnPBREnabledChanged(std::function<void(bool)> handler) { m_pbrChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetParallaxToggleEnabled(bool enabled) { m_parallaxChk->SetEnabled(enabled); }
void NifCompareControlPanel::SetComplexMaterialToggleEnabled(bool enabled) { m_complexMaterialChk->SetEnabled(enabled); }
void NifCompareControlPanel::SetPBRToggleEnabled(bool enabled) { m_pbrChk->SetEnabled(enabled); }

void NifCompareControlPanel::SetOnBrowseGameData(std::function<void()> handler) { m_browseGameDataBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnDetectGameData(std::function<void()> handler) { m_detectGameDataBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnAddOverrideFolder(std::function<void()> handler) { m_addOverrideBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnClearOverrides(std::function<void()> handler) { m_clearOverridesBtn->OnClick(std::move(handler)); }

void NifCompareControlPanel::SetGameDataLabel(const std::wstring& text)
{
    m_gameDataLabel->SetText(text.empty() ? L"Game Data: (not set)" : (L"Game Data: " + text));
}

void NifCompareControlPanel::SetOverrideCountLabel(std::size_t count)
{
    m_overrideLabel->SetText(L"Overrides: " + std::to_wstring(count) + L" folder(s)");
}

} // namespace nsk
