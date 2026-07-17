#include "ComparePane.h"

#include <string>

namespace nsk
{

ComparePane::ComparePane(const std::wstring& name)
    : FD2D::DockPanel(name)
{
    // The base owns the strip; each derived pane docks it (bottom) in its ctor.
    m_thumbStrip = std::make_shared<ThumbnailStrip>(name + L"_Thumbs");
    m_thumbStrip->SetOrientation(ThumbnailStrip::Orientation::Horizontal);
    m_thumbStrip->SetOnActivated([this](const std::wstring& path)
    {
        // Route through the owner so the pick is opened by file kind and
        // mirrored to the other panes; fall back to a direct load when unowned.
        if (m_onThumbnailChosen)
        {
            m_onThumbnailChosen(path);
        }
        else
        {
            std::string err;
            Load(path, &err);
            Invalidate();
        }
    });
    m_thumbStrip->SetOnResize([this](float ext, bool committed)
    {
        if (m_onThumbStripResize)
            m_onThumbStripResize(ext, committed);
    });
}

} // namespace nsk
