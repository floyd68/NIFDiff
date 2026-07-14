// NifCompareControlPanel.h - shared bottom control strip for NifCompareView.
//
// FICture2-style placement: a compact horizontal strip docked at the bottom
// of the window (see ImageBrowser.cpp's rootSplit pattern - a ~0.85-ratio
// vertical SplitPanel with the second pane's extent clamped to its content),
// not liteviewer's original wide vertical sidebar on the right. The control
// API below is unchanged from liteviewer's version (every callback already
// applies to "all panes", never hardcoded to a Left/Right pair), only the
// internal layout (StackPanel(Vertical) of StackPanel(Horizontal) rows
// instead of one tall StackPanel(Vertical)) and the new SetOnAddPane hook
// (the "+ Add Pane" control lives here since it is a whole-view action,
// unlike each pane's own per-pane Open/Close buttons - see NifComparePane.h)
// are new.
#pragma once

#include <StackPanel.h>
#include <functional>
#include <memory>
#include <string>

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
    void SetOnOrientationChanged(std::function<void(int)> handler);
    void SetOnFrontalLightChanged(std::function<void(bool)> handler);
    void SetOnShowGridChanged(std::function<void(bool)> handler);
    void SetOnShowAxesChanged(std::function<void(bool)> handler);
    void SetOnWireframeChanged(std::function<void(bool)> handler);

    void SetOnBrightnessChanged(std::function<void(float)> handler);
    void SetOnAmbientChanged(std::function<void(float)> handler);
    void SetOnDeclinationChanged(std::function<void(float)> handler);
    void SetOnPlanarAngleChanged(std::function<void(float)> handler);

    // POM height budget (vanilla/_m parallax HeightScale). The slider is
    // grayed out unless some loaded NIF actually runs height parallax -
    // see NifCompareView::RefreshParallaxSliderEnabled.
    void SetOnParallaxHeightChanged(std::function<void(float)> handler);
    void SetParallaxHeightEnabled(bool enabled);
    float ParallaxHeightValue() const;

    // Resources (Game Data / Override folders).
    void SetOnBrowseGameData(std::function<void()> handler);
    void SetOnDetectGameData(std::function<void()> handler);
    void SetOnAddOverrideFolder(std::function<void()> handler);
    void SetOnClearOverrides(std::function<void()> handler);
    void SetGameDataLabel(const std::wstring& text);
    void SetOverrideCountLabel(std::size_t count);

private:
    std::shared_ptr<FD2D::Button> m_addPaneBtn;
    std::shared_ptr<FD2D::Button> m_resetBtn;
    std::shared_ptr<FD2D::CheckBox> m_syncViewsChk;
    std::shared_ptr<FD2D::CheckBox> m_syncLightingChk;
    std::shared_ptr<FD2D::ComboBox> m_orientationCombo;
    std::shared_ptr<FD2D::CheckBox> m_frontalLightChk;
    std::shared_ptr<FD2D::CheckBox> m_showGridChk;
    std::shared_ptr<FD2D::CheckBox> m_showAxesChk;
    std::shared_ptr<FD2D::CheckBox> m_wireframeChk;
    std::shared_ptr<FD2D::Slider> m_brightnessSlider;
    std::shared_ptr<FD2D::Slider> m_ambientSlider;
    std::shared_ptr<FD2D::Slider> m_declinationSlider;
    std::shared_ptr<FD2D::Slider> m_planarAngleSlider;
    std::shared_ptr<FD2D::Slider> m_parallaxSlider;

    std::shared_ptr<FD2D::Text> m_gameDataLabel;
    std::shared_ptr<FD2D::Text> m_overrideLabel;
    std::shared_ptr<FD2D::Button> m_browseGameDataBtn;
    std::shared_ptr<FD2D::Button> m_detectGameDataBtn;
    std::shared_ptr<FD2D::Button> m_addOverrideBtn;
    std::shared_ptr<FD2D::Button> m_clearOverridesBtn;
};

} // namespace nsk
