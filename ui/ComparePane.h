// ComparePane.h - common base for the panes a NifCompareView lays out.
//
// NifCompareView was written for a homogeneous set of NifComparePane; the
// texture view/compare port adds a second pane kind (an image/texture pane)
// that must sit in the SAME host tree and be arranged freely alongside NIF
// panes. This base carves out the small surface the view needs to treat any
// pane generically - identity, the load/clear lifecycle, and its kind - so the
// view can hold a heterogeneous list. Kind-specific behaviour (a NIF pane's
// 3D viewport, material/animation/texture tooling; an image pane's zoom/pan)
// stays on the concrete panes and is reached by the view via a kind check.
#pragma once

#include <DockPanel.h>

#include <string>

namespace nsk
{

class ComparePane : public FD2D::DockPanel
{
public:
    enum class Kind
    {
        Nif,   // NifComparePane: a 3D NifViewport
        Image, // ImagePane: a decoded texture (added by the texture-view port)
    };

    explicit ComparePane(const std::wstring& name) : FD2D::DockPanel(name) {}
    ~ComparePane() override = default;

    // Which concrete pane this is; the view uses it to gate kind-specific work.
    virtual Kind PaneKind() const = 0;

    // Full path of the file shown (or loading) here; empty when the pane is
    // free. The single string every open path funnels through, used for the
    // active-file title, session persistence, IPC snapshot and sync matching.
    virtual std::wstring CurrentPath() const = 0;

    // Open `path` into this pane. Returns false only when the path can't be
    // accepted. The choke point for every open route (dialog, drag&drop,
    // command line, session restore, IPC).
    virtual bool Load(const std::wstring& path, std::string* error = nullptr) = 0;

    // Return the pane to its empty state (no document/image shown).
    virtual void Clear() = 0;
};

} // namespace nsk
