// FloarPathByteSource.h - adapts Floar's VFS (loose disk + BSA/BA2/ZIP/7z/RAR
// members) to ImageCore's IPathByteSource, so ImageCore can decode an image the
// thumbnail strip listed by its virtual path WITHOUT ImageCore depending on
// Floar. The app installs one instance at startup (see NIFDiffApp.cpp); without
// it, archive-internal images (and their thumbnails) fail to decode because
// ImageCore would treat the virtual path as a plain filesystem path.
#pragma once

#include "ImageCore/IPathByteSource.h"

namespace nsk
{

class FloarPathByteSource : public ImageCore::IPathByteSource
{
public:
    // A display/virtual path -> the host file used for volume queries (the
    // archive file for a member, the file itself for a loose path). Returns the
    // path unchanged when Floar can't parse it (a plain disk path).
    std::wstring ResolveHostPath(const std::wstring& path) const override;

    // Whole-file bytes for a loose disk path or an archive member. Empty on a
    // parse/read failure.
    std::vector<std::uint8_t> ReadAll(const std::wstring& path) const override;
};

} // namespace nsk
