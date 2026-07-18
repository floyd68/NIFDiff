#include "ShaderIncludeResolver.h"

#include <windows.h>

#include <fstream>
#include <new>

namespace nsk
{

std::filesystem::path ResolveShaderIncludePath(
    const std::filesystem::path& includePath,
    const std::filesystem::path& parentDirectory,
    const std::vector<std::filesystem::path>& shaderRoots)
{
    std::error_code error;
    if (!parentDirectory.empty())
    {
        const std::filesystem::path candidate =
            parentDirectory / includePath;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate;
        }
        error.clear();
    }

    for (const auto& root : shaderRoots)
    {
        if (root.empty())
        {
            continue;
        }
        const std::filesystem::path candidate =
            root / includePath;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate;
        }
        error.clear();
    }
    return {};
}

ShaderIncludeResolver::ShaderIncludeResolver(
    std::filesystem::path sourceFile,
    std::vector<std::filesystem::path> shaderRoots)
    : m_sourceDirectory(std::move(sourceFile).parent_path())
    , m_shaderRoots(std::move(shaderRoots))
{
}

HRESULT ShaderIncludeResolver::Open(
    D3D_INCLUDE_TYPE,
    LPCSTR fileName,
    LPCVOID parentData,
    LPCVOID* data,
    UINT* bytes)
{
    if (fileName == nullptr ||
        data == nullptr ||
        bytes == nullptr)
    {
        return E_INVALIDARG;
    }

    std::filesystem::path parentDirectory =
        m_sourceDirectory;
    if (parentData != nullptr)
    {
        const auto parent =
            m_openDirectories.find(parentData);
        if (parent != m_openDirectories.end())
        {
            parentDirectory = parent->second;
        }
    }

    const std::filesystem::path resolved =
        ResolveShaderIncludePath(
            std::filesystem::path(fileName),
            parentDirectory,
            m_shaderRoots);
    if (resolved.empty())
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::ifstream input(
        resolved,
        std::ios::binary |
            std::ios::ate);
    if (!input)
    {
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }
    const std::streamoff length = input.tellg();
    if (length < 0 ||
        static_cast<unsigned long long>(length) >
            static_cast<unsigned long long>(UINT_MAX))
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    const UINT size = static_cast<UINT>(length);
    auto* buffer =
        new (std::nothrow) unsigned char[
            size == 0 ? 1 : size];
    if (buffer == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    input.seekg(0);
    if (size != 0)
    {
        input.read(
            reinterpret_cast<char*>(buffer),
            size);
        if (!input)
        {
            delete[] buffer;
            return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        }
    }

    *data = buffer;
    *bytes = size;
    m_openDirectories.emplace(
        buffer,
        resolved.parent_path());
    return S_OK;
}

HRESULT ShaderIncludeResolver::Close(LPCVOID data)
{
    if (data == nullptr)
    {
        return E_INVALIDARG;
    }
    m_openDirectories.erase(data);
    delete[] static_cast<const unsigned char*>(data);
    return S_OK;
}

}
