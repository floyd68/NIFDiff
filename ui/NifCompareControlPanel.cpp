#include "NifCompareControlPanel.h"
#include <Button.h>
#include <Slider.h>
#include <CheckBox.h>
#include <ComboBox.h>
#include <Text.h>

#include <shlwapi.h>
#include <wrl/client.h>
#pragma comment(lib, "shlwapi.lib")

namespace nsk
{

NifCompareControlPanel::NifCompareControlPanel(const std::wstring& name)
    : FD2D::StackPanel(name, FD2D::Orientation::Horizontal)
{
    SetSpacing(kGroupGap);
    SetPadding(8.0f);

    // One labeled ribbon group: dim uppercase header over a vertical stack
    // of content. Registered in m_groups so OnRender can place the
    // separator hairlines between neighbors.
    auto makeGroup = [this, &name](const wchar_t* id, const wchar_t* title)
    {
        auto group = std::make_shared<FD2D::StackPanel>(name + L"_" + id, FD2D::Orientation::Vertical);
        group->SetSpacing(5.0f);
        auto header = std::make_shared<FD2D::Text>(name + L"_" + id + L"Hdr");
        header->SetText(title);
        header->SetFont(L"Segoe UI", 10.5f);
        header->SetColor(D2D1::ColorF(0.52f, 0.56f, 0.61f));
        group->AddChild(header);
        AddChild(group);
        m_groups.push_back(group);
        return group;
    };
    auto makeRow = [](const std::wstring& rowName, float spacing)
    {
        auto row = std::make_shared<FD2D::StackPanel>(rowName, FD2D::Orientation::Horizontal);
        row->SetSpacing(spacing);
        return row;
    };
    auto makeColumn = [](const std::wstring& colName, float spacing)
    {
        auto col = std::make_shared<FD2D::StackPanel>(colName, FD2D::Orientation::Vertical);
        col->SetSpacing(spacing);
        return col;
    };

    // --- PANES: whole-view actions -------------------------------------
    auto panes = makeGroup(L"Panes", L"PANES");

    m_addPaneBtn = std::make_shared<FD2D::Button>(L"AddPane");
    m_addPaneBtn->SetLabel(L"+ Add Pane");
    panes->AddChild(m_addPaneBtn);

    m_resetBtn = std::make_shared<FD2D::Button>(L"ResetCameras");
    m_resetBtn->SetLabel(L"Reset View");
    panes->AddChild(m_resetBtn);

    // --- VIEW: camera preset + cross-pane sync --------------------------
    auto view = makeGroup(L"View", L"VIEW");

    m_orientationCombo = std::make_shared<FD2D::ComboBox>(L"Orientation");
    m_orientationCombo->SetItems({ L"Front", L"Back", L"Left", L"Right", L"Top", L"Bottom" });
    m_orientationCombo->SetSelectedIndex(0);
    view->AddChild(m_orientationCombo);

    m_syncViewsChk = std::make_shared<FD2D::CheckBox>(L"SyncViews");
    m_syncViewsChk->SetLabel(L"Sync Views");
    m_syncViewsChk->SetChecked(true);
    view->AddChild(m_syncViewsChk);

    m_syncLightingChk = std::make_shared<FD2D::CheckBox>(L"SyncLighting");
    m_syncLightingChk->SetLabel(L"Sync Lighting");
    m_syncLightingChk->SetChecked(true);
    view->AddChild(m_syncLightingChk);

    // Selecting a thumbnail in one pane loads the same file name from every
    // other pane's folder (compare a mesh across mods). On by default; a
    // no-op when no other pane's folder holds that name.
    m_syncFilesChk = std::make_shared<FD2D::CheckBox>(L"SyncFiles");
    m_syncFilesChk->SetLabel(L"Sync Files");
    m_syncFilesChk->SetChecked(true);
    view->AddChild(m_syncFilesChk);

    // Folder thumbnail strip on/off (the ThumbnailStrip browser). On by
    // default; unchecking hides the strip and idles its loader.
    m_thumbnailStripChk = std::make_shared<FD2D::CheckBox>(L"ThumbnailStrip");
    m_thumbnailStripChk->SetLabel(L"Thumbnails");
    m_thumbnailStripChk->SetChecked(true);
    view->AddChild(m_thumbnailStripChk);

    // --- NAVIGATION: camera-feel tuning + projection/behavior toggles ---
    // Two columns like LIGHTING: sensitivity sliders on the left, FOV + the
    // projection/behavior checkboxes on the right. Global (applied to every
    // pane) to match the rest of this strip.
    auto navigation = makeGroup(L"Navigation", L"NAVIGATION");
    auto navRow = makeRow(name + L"_NavRow", 14.0f);

    auto navCol1 = makeColumn(name + L"_NavCol1", 4.0f);
    // 0.2-5.0 (default 1.0): the wide top end gives headroom for fast traversal
    // and for close-up work (paired with NifViewport's close-up pan floor).
    m_moveSensSlider = std::make_shared<FD2D::Slider>(L"MoveSensitivity");
    m_moveSensSlider->SetLabel(L"Move");
    m_moveSensSlider->SetRange(0.2f, 5.0f);
    m_moveSensSlider->SetValue(1.0f);
    navCol1->AddChild(m_moveSensSlider);

    m_zoomSensSlider = std::make_shared<FD2D::Slider>(L"ZoomSensitivity");
    m_zoomSensSlider->SetLabel(L"Zoom");
    m_zoomSensSlider->SetRange(0.2f, 5.0f);
    m_zoomSensSlider->SetValue(1.0f);
    navCol1->AddChild(m_zoomSensSlider);

    m_rotateSensSlider = std::make_shared<FD2D::Slider>(L"RotateSensitivity");
    m_rotateSensSlider->SetLabel(L"Rotate");
    m_rotateSensSlider->SetRange(0.2f, 5.0f);
    m_rotateSensSlider->SetValue(1.0f);
    navCol1->AddChild(m_rotateSensSlider);
    navRow->AddChild(navCol1);

    auto navCol2 = makeColumn(name + L"_NavCol2", 4.0f);
    // Default matches NifViewport's 0.9 rad vertical FOV (~51.6 deg).
    m_fovSlider = std::make_shared<FD2D::Slider>(L"FieldOfView");
    m_fovSlider->SetLabel(L"FOV");
    m_fovSlider->SetRange(20.0f, 90.0f);
    m_fovSlider->SetValue(51.6f);
    navCol2->AddChild(m_fovSlider);

    m_orthoChk = std::make_shared<FD2D::CheckBox>(L"Orthographic");
    m_orthoChk->SetLabel(L"Orthographic");
    navCol2->AddChild(m_orthoChk);

    m_orbitSelChk = std::make_shared<FD2D::CheckBox>(L"OrbitSelection");
    m_orbitSelChk->SetLabel(L"Orbit Sel");
    m_orbitSelChk->SetChecked(true);
    navCol2->AddChild(m_orbitSelChk);

    m_zoomCursorChk = std::make_shared<FD2D::CheckBox>(L"ZoomToCursor");
    m_zoomCursorChk->SetLabel(L"Zoom Cursor");
    m_zoomCursorChk->SetChecked(true);
    navCol2->AddChild(m_zoomCursorChk);
    navRow->AddChild(navCol2);

    navigation->AddChild(navRow);

    // --- DISPLAY: scene decorations ------------------------------------
    auto display = makeGroup(L"Display", L"DISPLAY");

    m_showGridChk = std::make_shared<FD2D::CheckBox>(L"ShowGrid");
    m_showGridChk->SetLabel(L"Grid");
    m_showGridChk->SetChecked(true);
    display->AddChild(m_showGridChk);

    m_showAxesChk = std::make_shared<FD2D::CheckBox>(L"ShowAxes");
    m_showAxesChk->SetLabel(L"Axes");
    m_showAxesChk->SetChecked(true);
    display->AddChild(m_showAxesChk);

    m_wireframeChk = std::make_shared<FD2D::CheckBox>(L"Wireframe");
    m_wireframeChk->SetLabel(L"Wireframe");
    display->AddChild(m_wireframeChk);

    // NifSkope's vertex normal/tangent line overlays (gltools drawNormals):
    // cyan normal / magenta tangent segments, for the selected mesh when one
    // is picked, every mesh otherwise. For debugging normal/tangent data.
    m_showNormalsChk = std::make_shared<FD2D::CheckBox>(L"ShowNormals");
    m_showNormalsChk->SetLabel(L"Normals");
    display->AddChild(m_showNormalsChk);

    m_showTangentsChk = std::make_shared<FD2D::CheckBox>(L"ShowTangents");
    m_showTangentsChk->SetLabel(L"Tangents");
    display->AddChild(m_showTangentsChk);

    // 4x multisample antialiasing (NifSkope's DoMultisampling), on by default.
    m_msaaChk = std::make_shared<FD2D::CheckBox>(L"MSAA");
    m_msaaChk->SetLabel(L"MSAA 4x");
    m_msaaChk->SetChecked(true);
    display->AddChild(m_msaaChk);

    // NifSkope's "Show Hidden": NiAVObject-hidden subtrees (furniture
    // marker rigs, editor markers) are culled by default; this opts them
    // back in for inspection.
    m_showHiddenChk = std::make_shared<FD2D::CheckBox>(L"ShowHidden");
    m_showHiddenChk->SetLabel(L"Hidden");
    display->AddChild(m_showHiddenChk);

    // --- LIGHTING: two slider columns + frontal-light mode --------------
    auto lighting = makeGroup(L"Lighting", L"LIGHTING");
    auto lightRow = makeRow(name + L"_LightRow", 14.0f);

    auto lightCol1 = makeColumn(name + L"_LightCol1", 4.0f);
    m_brightnessSlider = std::make_shared<FD2D::Slider>(L"Brightness");
    m_brightnessSlider->SetLabel(L"Brightness");
    m_brightnessSlider->SetRange(0.0f, 3.0f);
    m_brightnessSlider->SetValue(1.0f);
    lightCol1->AddChild(m_brightnessSlider);

    m_ambientSlider = std::make_shared<FD2D::Slider>(L"Ambient");
    m_ambientSlider->SetLabel(L"Ambient");
    m_ambientSlider->SetRange(0.0f, 1.0f);
    m_ambientSlider->SetValue(0.35f);
    lightCol1->AddChild(m_ambientSlider);

    m_frontalLightChk = std::make_shared<FD2D::CheckBox>(L"FrontalLight");
    m_frontalLightChk->SetLabel(L"Frontal");
    lightCol1->AddChild(m_frontalLightChk);
    lightRow->AddChild(lightCol1);

    auto lightCol2 = makeColumn(name + L"_LightCol2", 4.0f);
    m_declinationSlider = std::make_shared<FD2D::Slider>(L"Declination");
    m_declinationSlider->SetLabel(L"Declination");
    m_declinationSlider->SetRange(-90.0f, 90.0f);
    m_declinationSlider->SetValue(45.0f);
    lightCol2->AddChild(m_declinationSlider);

    m_planarAngleSlider = std::make_shared<FD2D::Slider>(L"PlanarAngle");
    m_planarAngleSlider->SetLabel(L"Planar Angle");
    m_planarAngleSlider->SetRange(-180.0f, 180.0f);
    m_planarAngleSlider->SetValue(45.0f);
    lightCol2->AddChild(m_planarAngleSlider);
    lightRow->AddChild(lightCol2);

    lighting->AddChild(lightRow);

    // --- MATERIALS: extended-material toggles + POM height ---------------
    // Toggles off = the closest legacy interpretation (no POM, complex
    // materials as plain env masks, PBR through the vanilla lit path) -
    // handy for before/after comparisons. Everything starts disabled;
    // NifCompareView enables a control while some loaded NIF carries
    // material it would affect.
    auto materials = makeGroup(L"Materials", L"MATERIALS");
    auto matRow = makeRow(name + L"_MatRow", 14.0f);

    auto matCol = makeColumn(name + L"_MatToggles", 4.0f);
    m_parallaxChk = std::make_shared<FD2D::CheckBox>(L"EnableParallax");
    m_parallaxChk->SetLabel(L"Parallax");
    m_parallaxChk->SetChecked(true);
    m_parallaxChk->SetEnabled(false);
    matCol->AddChild(m_parallaxChk);

    m_complexMaterialChk = std::make_shared<FD2D::CheckBox>(L"EnableComplexMaterial");
    m_complexMaterialChk->SetLabel(L"Complex Mat");
    m_complexMaterialChk->SetChecked(true);
    m_complexMaterialChk->SetEnabled(false);
    matCol->AddChild(m_complexMaterialChk);

    m_pbrChk = std::make_shared<FD2D::CheckBox>(L"EnablePBR");
    m_pbrChk->SetLabel(L"True PBR");
    m_pbrChk->SetChecked(true);
    m_pbrChk->SetEnabled(false);
    matCol->AddChild(m_pbrChk);
    matRow->AddChild(matCol);

    // POM height budget: HeightScale in CS terms (0.1*scale UV depth).
    m_parallaxSlider = std::make_shared<FD2D::Slider>(L"ParallaxHeight");
    m_parallaxSlider->SetLabel(L"Height Scale");
    m_parallaxSlider->SetRange(0.0f, 5.0f);
    m_parallaxSlider->SetValue(2.0f);
    m_parallaxSlider->SetEnabled(false);
    matRow->AddChild(m_parallaxSlider);

    materials->AddChild(matRow);

    // --- CHANNELS: render-channel isolation toggles -----------------------
    // Diff analysis: switch one shading input off at a time to find which
    // channel makes two panes differ. All on by default; unlike the
    // MATERIALS toggles these stay enabled regardless of content (every
    // lit mesh has all five inputs conceptually).
    auto channels = makeGroup(L"Channels", L"CHANNELS");
    auto chRow = makeRow(name + L"_ChRow", 10.0f);

    auto chCol1 = makeColumn(name + L"_ChCol1", 4.0f);
    m_texturesChk = std::make_shared<FD2D::CheckBox>(L"ChannelTextures");
    m_texturesChk->SetLabel(L"Diffuse");
    m_texturesChk->SetChecked(true);
    chCol1->AddChild(m_texturesChk);

    m_vertexColorsChk = std::make_shared<FD2D::CheckBox>(L"ChannelVertexColors");
    m_vertexColorsChk->SetLabel(L"Vtx Color");
    m_vertexColorsChk->SetChecked(true);
    chCol1->AddChild(m_vertexColorsChk);

    m_specularChk = std::make_shared<FD2D::CheckBox>(L"ChannelSpecular");
    m_specularChk->SetLabel(L"Specular");
    m_specularChk->SetChecked(true);
    chCol1->AddChild(m_specularChk);
    chRow->AddChild(chCol1);

    auto chCol2 = makeColumn(name + L"_ChCol2", 4.0f);
    m_glowChk = std::make_shared<FD2D::CheckBox>(L"ChannelGlow");
    m_glowChk->SetLabel(L"Emissive");
    m_glowChk->SetChecked(true);
    chCol2->AddChild(m_glowChk);

    m_lightingChk = std::make_shared<FD2D::CheckBox>(L"ChannelLighting");
    m_lightingChk->SetLabel(L"Lighting");
    m_lightingChk->SetChecked(true);
    chCol2->AddChild(m_lightingChk);
    chRow->AddChild(chCol2);

    channels->AddChild(chRow);

    // --- RESOURCES: Game Data / override folders -------------------------
    auto resources = makeGroup(L"Resources", L"RESOURCES");

    m_gameDataLabel = std::make_shared<FD2D::Text>(L"GameDataLabel");
    m_gameDataLabel->SetText(L"Game Data: (not set)");
    m_gameDataLabel->SetFont(L"Segoe UI", 12.5f);
    // Wide enough for a middle-compacted install path (see
    // SetGameDataLabel); the trailing ellipsis stays on as a backstop for
    // pathological paths the compaction can't shrink.
    m_gameDataLabel->SetFixedWidth(330.0f);
    m_gameDataLabel->SetEllipsisTrimmingEnabled(true);
    // Hover shows the full install path (set in SetGameDataLabel, since the
    // displayed text is middle-compacted); right-click copies it.
    m_gameDataLabel->SetCopyTextOnRightClick(true);
    resources->AddChild(m_gameDataLabel);

    auto resRow = makeRow(name + L"_ResRow", 8.0f);

    m_browseGameDataBtn = std::make_shared<FD2D::Button>(L"BrowseGameData");
    m_browseGameDataBtn->SetLabel(L"Browse...");
    resRow->AddChild(m_browseGameDataBtn);

    m_detectGameDataBtn = std::make_shared<FD2D::Button>(L"DetectGameData");
    m_detectGameDataBtn->SetLabel(L"Detect");
    resRow->AddChild(m_detectGameDataBtn);

    m_overrideLabel = std::make_shared<FD2D::Text>(L"OverrideLabel");
    m_overrideLabel->SetText(L"Overrides: 0");
    m_overrideLabel->SetFont(L"Segoe UI", 12.5f);
    resRow->AddChild(m_overrideLabel);

    m_addOverrideBtn = std::make_shared<FD2D::Button>(L"AddOverride");
    m_addOverrideBtn->SetLabel(L"Add...");
    resRow->AddChild(m_addOverrideBtn);

    m_clearOverridesBtn = std::make_shared<FD2D::Button>(L"ClearOverrides");
    m_clearOverridesBtn->SetLabel(L"Clear");
    resRow->AddChild(m_clearOverridesBtn);

    resources->AddChild(resRow);
}

void NifCompareControlPanel::OnRender(ID2D1RenderTarget* target)
{
    // Strip top border + vertical hairlines centered in the gaps between
    // groups. Drawn before the children so the controls paint on top.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (target != nullptr
        && SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f), &brush)))
    {
        const D2D1_RECT_F rc = LayoutRect();
        target->DrawLine({ rc.left, rc.top + 0.5f }, { rc.right, rc.top + 0.5f }, brush.Get(), 1.0f);
        for (std::size_t i = 0; i + 1 < m_groups.size(); ++i)
        {
            const D2D1_RECT_F g = m_groups[i]->LayoutRect();
            const float x = g.right + kGroupGap * 0.5f;
            target->DrawLine({ x, rc.top + 10.0f }, { x, rc.bottom - 10.0f }, brush.Get(), 1.0f);
        }
    }
    FD2D::StackPanel::OnRender(target);
}

void NifCompareControlPanel::SetOnAddPane(std::function<void()> handler) { m_addPaneBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnResetCameras(std::function<void()> handler) { m_resetBtn->OnClick(std::move(handler)); }

void NifCompareControlPanel::SetOnSyncViewsChanged(std::function<void(bool)> handler) { m_syncViewsChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnSyncLightingChanged(std::function<void(bool)> handler) { m_syncLightingChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnSyncFilesChanged(std::function<void(bool)> handler) { m_syncFilesChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnThumbnailStripChanged(std::function<void(bool)> handler) { m_thumbnailStripChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetThumbnailStripChecked(bool checked, bool notify) { m_thumbnailStripChk->SetChecked(checked, notify); }
bool NifCompareControlPanel::ThumbnailStripChecked() const { return m_thumbnailStripChk->Checked(); }
void NifCompareControlPanel::ToggleThumbnailStrip() { m_thumbnailStripChk->SetChecked(!m_thumbnailStripChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::SetOnOrientationChanged(std::function<void(int)> handler) { m_orientationCombo->OnSelectionChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnFrontalLightChanged(std::function<void(bool)> handler) { m_frontalLightChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowGridChanged(std::function<void(bool)> handler) { m_showGridChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowAxesChanged(std::function<void(bool)> handler) { m_showAxesChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnWireframeChanged(std::function<void(bool)> handler) { m_wireframeChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowHiddenChanged(std::function<void(bool)> handler) { m_showHiddenChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowNormalsChanged(std::function<void(bool)> handler) { m_showNormalsChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnShowTangentsChanged(std::function<void(bool)> handler) { m_showTangentsChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnMsaaChanged(std::function<void(bool)> handler) { m_msaaChk->OnCheckedChanged(std::move(handler)); }

void NifCompareControlPanel::ToggleShowGrid()     { m_showGridChk->SetChecked(!m_showGridChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleShowAxes()     { m_showAxesChk->SetChecked(!m_showAxesChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleWireframe()    { m_wireframeChk->SetChecked(!m_wireframeChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleShowHidden()   { m_showHiddenChk->SetChecked(!m_showHiddenChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleShowNormals()  { m_showNormalsChk->SetChecked(!m_showNormalsChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleShowTangents() { m_showTangentsChk->SetChecked(!m_showTangentsChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::ToggleMsaa()         { m_msaaChk->SetChecked(!m_msaaChk->Checked(), /*notify=*/true); }

void NifCompareControlPanel::CycleOrientation(int delta)
{
    constexpr int kOrientationCount = 6; // Front/Back/Left/Right/Top/Bottom
    const int idx = ((m_orientationCombo->SelectedIndex() + delta) % kOrientationCount + kOrientationCount) % kOrientationCount;
    m_orientationCombo->SetSelectedIndex(idx, /*notify=*/true);
}

void NifCompareControlPanel::SetOrientation(int index)
{
    // notify=true fires OnOrientationChanged -> ApplyOrientationPreset, so a
    // keyboard/numpad preset drives the same path as picking from the dropdown
    // (and the combo label stays in sync with the actual view).
    m_orientationCombo->SetSelectedIndex(index, /*notify=*/true);
}

void NifCompareControlPanel::SetOnMoveSensitivityChanged(std::function<void(float)> handler) { m_moveSensSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnZoomSensitivityChanged(std::function<void(float)> handler) { m_zoomSensSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnRotateSensitivityChanged(std::function<void(float)> handler) { m_rotateSensSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnFovChanged(std::function<void(float)> handler) { m_fovSlider->OnValueChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnOrthographicChanged(std::function<void(bool)> handler) { m_orthoChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOrthographicChecked(bool checked, bool notify) { m_orthoChk->SetChecked(checked, notify); }
void NifCompareControlPanel::ToggleOrthographic() { m_orthoChk->SetChecked(!m_orthoChk->Checked(), /*notify=*/true); }
void NifCompareControlPanel::SetOnOrbitSelectionChanged(std::function<void(bool)> handler) { m_orbitSelChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnZoomToCursorChanged(std::function<void(bool)> handler) { m_zoomCursorChk->OnCheckedChanged(std::move(handler)); }

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
void NifCompareControlPanel::SetOnTexturesEnabledChanged(std::function<void(bool)> handler) { m_texturesChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnVertexColorsEnabledChanged(std::function<void(bool)> handler) { m_vertexColorsChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnSpecularEnabledChanged(std::function<void(bool)> handler) { m_specularChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnGlowEnabledChanged(std::function<void(bool)> handler) { m_glowChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetOnLightingEnabledChanged(std::function<void(bool)> handler) { m_lightingChk->OnCheckedChanged(std::move(handler)); }
void NifCompareControlPanel::SetParallaxToggleEnabled(bool enabled) { m_parallaxChk->SetEnabled(enabled); }
void NifCompareControlPanel::SetComplexMaterialToggleEnabled(bool enabled) { m_complexMaterialChk->SetEnabled(enabled); }
void NifCompareControlPanel::SetPBRToggleEnabled(bool enabled) { m_pbrChk->SetEnabled(enabled); }

void NifCompareControlPanel::SetOnBrowseGameData(std::function<void()> handler) { m_browseGameDataBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnDetectGameData(std::function<void()> handler) { m_detectGameDataBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnAddOverrideFolder(std::function<void()> handler) { m_addOverrideBtn->OnClick(std::move(handler)); }
void NifCompareControlPanel::SetOnClearOverrides(std::function<void()> handler) { m_clearOverridesBtn->OnClick(std::move(handler)); }

void NifCompareControlPanel::SetGameDataLabel(const std::wstring& text)
{
    if (text.empty())
    {
        m_gameDataLabel->SetText(L"Game Data: (not set)");
        m_gameDataLabel->SetTooltipText(L"");
        m_gameDataLabel->SetCopyText(L"");
        return;
    }
    // A Data path's tail is the informative part ("...\Skyrim Special
    // Edition\Data"), so DirectWrite's trailing ellipsis is the wrong
    // trim for it. Compact the middle at path separators instead
    // (PathCompactPathEx), sized so prefix + path fits the label's fixed
    // width at its 13pt font.
    constexpr UINT kMaxPathChars = 44;
    wchar_t compact[512] {};
    const bool compacted = text.size() > kMaxPathChars - 1
        && PathCompactPathExW(compact, text.c_str(), kMaxPathChars, 0) && compact[0] != L'\0';
    m_gameDataLabel->SetText(L"Game Data: " + std::wstring(compacted ? compact : text.c_str()));
    // Full path for hover (only when the display was actually shortened) and
    // for the right-click copy (always).
    m_gameDataLabel->SetTooltipText(compacted ? text : std::wstring());
    m_gameDataLabel->SetCopyText(text);
}

void NifCompareControlPanel::SetOverrideCountLabel(std::size_t count)
{
    m_overrideLabel->SetText(L"Overrides: " + std::to_wstring(count));
}

} // namespace nsk
