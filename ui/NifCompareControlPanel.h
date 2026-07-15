// NifCompareControlPanel.h - shared bottom control strip for NifCompareView.
//
// FICture2-style placement: a compact horizontal strip docked at the bottom
// of the window (see ImageBrowser.cpp's rootSplit pattern - a ~0.85-ratio
// vertical SplitPanel with the second pane's extent clamped to its content),
// not liteviewer's original wide vertical sidebar on the right.
//
// Layout: a ribbon of six labeled GROUPS side by side (PANES / VIEW /
// DISPLAY / LIGHTING / MATERIALS / RESOURCES), each a vertical stack under
// a dim uppercase header, separated by hairlines drawn in OnRender. This
// replaced the original three ad-hoc mixed rows: grouping by function is
// what makes ~20 controls scannable, and the headers carry the context so
// the individual labels can stay short ("Grid", not "Show Grid"). The
// control API below is unchanged from liteviewer's version (every callback
// already applies to "all panes", never hardcoded to a Left/Right pair).
#pragma once

#include <StackPanel.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace FD2D { class Button; class Slider; class CheckBox; class ComboBox; class Text; }

namespace nsk
{

class NifCompareControlPanel : public FD2D::StackPanel
{
public:
    explicit NifCompareControlPanel(const std::wstring& name);

    void SetOnAddPane(std::function<void()> handler);
    void SetOnResetCameras(std::function<void()> handler);

    void SetOnSyncViewsChanged(std::function<void(bool)> handler);
    void SetOnSyncLightingChanged(std::function<void(bool)> handler);
    // Selecting a thumbnail in one pane also loads the same-named .nif from
    // every other pane's own folder (for comparing variants across mods).
    void SetOnSyncFilesChanged(std::function<void(bool)> handler);
    // Folder thumbnail strip master on/off (the ThumbnailStrip browser). The
    // checkbox is the whole-UI mirror of the same context-menu toggle.
    void SetOnThumbnailStripChanged(std::function<void(bool)> handler);
    void SetThumbnailStripChecked(bool checked, bool notify = false);
    bool ThumbnailStripChecked() const;
    void ToggleThumbnailStrip();
    void SetOnOrientationChanged(std::function<void(int)> handler);
    void SetOnFrontalLightChanged(std::function<void(bool)> handler);
    void SetOnShowGridChanged(std::function<void(bool)> handler);
    void SetOnShowAxesChanged(std::function<void(bool)> handler);
    void SetOnWireframeChanged(std::function<void(bool)> handler);
    // NifSkope's "Show Hidden": render NiAVObject-hidden subtrees too.
    void SetOnShowHiddenChanged(std::function<void(bool)> handler);
    // NifSkope's vertex normal/tangent line overlays (gltools drawNormals).
    void SetOnShowNormalsChanged(std::function<void(bool)> handler);
    void SetOnShowTangentsChanged(std::function<void(bool)> handler);
    // 4x MSAA on/off (NifSkope's DoMultisampling).
    void SetOnMsaaChanged(std::function<void(bool)> handler);

    // Keyboard-shortcut hooks (see NifCompareView's key handling): flip a
    // display checkbox exactly as a click would (the wired handler runs),
    // or step the orientation preset with wrap-around.
    void ToggleShowGrid();
    void ToggleShowAxes();
    void ToggleWireframe();
    void ToggleShowHidden();
    void ToggleShowNormals();
    void ToggleShowTangents();
    void ToggleMsaa();
    void CycleOrientation(int delta);

    void SetOnBrightnessChanged(std::function<void(float)> handler);
    void SetOnAmbientChanged(std::function<void(float)> handler);
    void SetOnDeclinationChanged(std::function<void(float)> handler);
    void SetOnPlanarAngleChanged(std::function<void(float)> handler);

    // POM height budget (vanilla/_m parallax HeightScale). The slider is
    // grayed out unless some loaded NIF actually runs height parallax -
    // see NifCompareView::RefreshExtendedMaterialControls.
    void SetOnParallaxHeightChanged(std::function<void(float)> handler);
    void SetParallaxHeightEnabled(bool enabled);
    float ParallaxHeightValue() const;

    // Extended-material feature toggles (Parallax / Complex Material /
    // True PBR); off renders the closest legacy interpretation. Each
    // checkbox is grayed out unless some loaded NIF carries material the
    // toggle would affect - see NifCompareView's
    // RefreshExtendedMaterialControls.
    void SetOnParallaxEnabledChanged(std::function<void(bool)> handler);
    void SetOnComplexMaterialEnabledChanged(std::function<void(bool)> handler);
    void SetOnPBREnabledChanged(std::function<void(bool)> handler);
    void SetParallaxToggleEnabled(bool enabled);
    void SetComplexMaterialToggleEnabled(bool enabled);
    void SetPBRToggleEnabled(bool enabled);

    // Render-channel toggles (CHANNELS group): switch one shading input
    // off at a time to isolate why two panes differ. Always enabled.
    void SetOnTexturesEnabledChanged(std::function<void(bool)> handler);
    void SetOnVertexColorsEnabledChanged(std::function<void(bool)> handler);
    void SetOnSpecularEnabledChanged(std::function<void(bool)> handler);
    void SetOnGlowEnabledChanged(std::function<void(bool)> handler);
    void SetOnLightingEnabledChanged(std::function<void(bool)> handler);

    // Resources (Game Data / Override folders).
    void SetOnBrowseGameData(std::function<void()> handler);
    void SetOnDetectGameData(std::function<void()> handler);
    void SetOnAddOverrideFolder(std::function<void()> handler);
    void SetOnClearOverrides(std::function<void()> handler);
    void SetGameDataLabel(const std::wstring& text);
    void SetOverrideCountLabel(std::size_t count);

    // Draws the group separator hairlines + the strip's top border, then
    // renders the children as usual.
    void OnRender(ID2D1RenderTarget* target) override;

private:
    static constexpr float kGroupGap = 14.0f;

    std::vector<std::shared_ptr<FD2D::StackPanel>> m_groups; // for separator placement
    std::shared_ptr<FD2D::Button> m_addPaneBtn;
    std::shared_ptr<FD2D::Button> m_resetBtn;
    std::shared_ptr<FD2D::CheckBox> m_syncViewsChk;
    std::shared_ptr<FD2D::CheckBox> m_syncLightingChk;
    std::shared_ptr<FD2D::CheckBox> m_syncFilesChk;
    std::shared_ptr<FD2D::CheckBox> m_thumbnailStripChk;
    std::shared_ptr<FD2D::ComboBox> m_orientationCombo;
    std::shared_ptr<FD2D::CheckBox> m_frontalLightChk;
    std::shared_ptr<FD2D::CheckBox> m_showGridChk;
    std::shared_ptr<FD2D::CheckBox> m_showAxesChk;
    std::shared_ptr<FD2D::CheckBox> m_wireframeChk;
    std::shared_ptr<FD2D::CheckBox> m_showHiddenChk;
    std::shared_ptr<FD2D::CheckBox> m_showNormalsChk;
    std::shared_ptr<FD2D::CheckBox> m_showTangentsChk;
    std::shared_ptr<FD2D::CheckBox> m_msaaChk;
    std::shared_ptr<FD2D::Slider> m_brightnessSlider;
    std::shared_ptr<FD2D::Slider> m_ambientSlider;
    std::shared_ptr<FD2D::Slider> m_declinationSlider;
    std::shared_ptr<FD2D::Slider> m_planarAngleSlider;
    std::shared_ptr<FD2D::Slider> m_parallaxSlider;
    std::shared_ptr<FD2D::CheckBox> m_parallaxChk;
    std::shared_ptr<FD2D::CheckBox> m_complexMaterialChk;
    std::shared_ptr<FD2D::CheckBox> m_pbrChk;
    std::shared_ptr<FD2D::CheckBox> m_texturesChk;
    std::shared_ptr<FD2D::CheckBox> m_vertexColorsChk;
    std::shared_ptr<FD2D::CheckBox> m_specularChk;
    std::shared_ptr<FD2D::CheckBox> m_glowChk;
    std::shared_ptr<FD2D::CheckBox> m_lightingChk;

    std::shared_ptr<FD2D::Text> m_gameDataLabel;
    std::shared_ptr<FD2D::Text> m_overrideLabel;
    std::shared_ptr<FD2D::Button> m_browseGameDataBtn;
    std::shared_ptr<FD2D::Button> m_detectGameDataBtn;
    std::shared_ptr<FD2D::Button> m_addOverrideBtn;
    std::shared_ptr<FD2D::Button> m_clearOverridesBtn;
};

} // namespace nsk
