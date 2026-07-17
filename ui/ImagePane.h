// ImagePane.h - a ComparePane that shows a decoded texture/image.
//
// The image counterpart to NifComparePane, so NifCompareView can lay a
// texture pane out beside a 3D NIF pane. Decoding runs on ImageCore's own
// worker pool (WIC + DirectXTex); the decoded CPU BGRA8 payload is marshalled
// back to the UI thread via the backplate's async-redraw token and uploaded
// into an FD2D::Image (which handles aspect-fit draw, and later zoom/pan).
// Lifetime-safe: a decode completion is dropped if it is stale (generation)
// or the pane is gone (weak_ptr), so no callback ever touches a dead pane.
#pragma once

#include "PaneContent.h"

#include <Text.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class ImagePane : public PaneContent
{
public:
    explicit ImagePane(const std::wstring& name);
    ~ImagePane() override;

    // PaneContent: this is the 2D image content.
    Kind PaneKind() const override { return Kind::Image; }
    std::wstring CurrentPath() const override { return m_path; }
    bool Load(const std::wstring& path, std::string* error = nullptr) override;
    void Clear() override;

    // Texture-view controls (forwarded to the image display), driven by the
    // view's keyboard shortcuts while an image pane is active.
    void SetChannelMode(int mode);      // 0=RGBA,1=R,2=G,3=B,4=A; re-select toggles off
    void ToggleAlphaCheckerboard();
    void RotateCW();
    void RotateCCW();
    void ResetView();

    // Shareable view transform, for syncing zoom/pan/channel across image panes
    // (the "Sync Views" compare mode). Pan is in client pixels - fine for the
    // equal-width grid where panes are the same size.
    struct ImageViewState
    {
        float zoom { 1.0f };
        float panX { 0.0f };
        float panY { 0.0f };
        int rotation { 0 };
        int channelMode { 0 };
        bool checkerboard { false };
    };
    ImageViewState ViewState() const;
    void SetViewState(const ImageViewState& state);          // applies without re-notifying
    void SetOnViewChanged(std::function<void(const ImageViewState&)> handler);

private:
    // FD2D::Image subclass that stages a decoded payload off-thread and uploads
    // it to a device bitmap at render time; defined in the .cpp.
    class ImageView;
    // Shared with the decode callback: a generation to drop superseded/stale
    // completions and a weak view so a completion after destruction is a no-op.
    struct LoadGuard;

    void UpdatePathLabel();

    std::shared_ptr<ImageView> m_image;
    std::shared_ptr<FD2D::Text> m_pathLabel;
    std::wstring m_path;

    std::shared_ptr<LoadGuard> m_guard;
    std::uint64_t m_handle = 0; // last ImageCore request; cancelled on retarget
};

} // namespace nsk
