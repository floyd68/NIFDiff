#pragma once

#include <d3dcompiler.h>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace nsk
{

std::filesystem::path ResolveShaderIncludePath(
    const std::filesystem::path& includePath,
    const std::filesystem::path& parentDirectory,
    const std::vector<std::filesystem::path>& shaderRoots);

class ShaderIncludeResolver final : public ID3DInclude
{
public:
    ShaderIncludeResolver(
        std::filesystem::path sourceFile,
        std::vector<std::filesystem::path> shaderRoots);

    HRESULT Open(
        D3D_INCLUDE_TYPE includeType,
        LPCSTR fileName,
        LPCVOID parentData,
        LPCVOID* data,
        UINT* bytes) override;

    HRESULT Close(LPCVOID data) override;

private:
    std::filesystem::path m_sourceDirectory;
    std::vector<std::filesystem::path> m_shaderRoots;
    std::unordered_map<const void*, std::filesystem::path> m_openDirectories;
};

}
