#include "FloarPathByteSource.h"

#include <VirtualFileSystem.h>
#include <VirtualPath.h>

namespace nsk
{

std::wstring FloarPathByteSource::ResolveHostPath(const std::wstring& path) const
{
    auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
        return path;
    return vpath->hostPath.wstring();
}

std::vector<std::uint8_t> FloarPathByteSource::ReadAll(const std::wstring& path) const
{
    auto vpath = Floar::VirtualPath::Parse(path);
    if (!vpath)
        return {};
    return Floar::VirtualFileSystem::ReadFile(*vpath);
}

} // namespace nsk
