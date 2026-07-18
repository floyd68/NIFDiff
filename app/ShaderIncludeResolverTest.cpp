#include "../render/ShaderIncludeResolver.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

bool WriteText(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream output(
        path,
        std::ios::binary);
    output.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

}

int main()
{
    namespace fs = std::filesystem;

    const fs::path root =
        fs::temp_directory_path() /
        "nifdiff_shader_include_test";
    const fs::path source =
        root /
        "custom" /
        "Probe.hlsl";
    std::error_code error;
    fs::remove_all(root, error);

    if (!WriteText(
            source,
            "#include \"Common.hlsli\"\n"
            "float4 PSMain() : SV_Target { return TestColor(); }\n") ||
        !WriteText(
            root / "Common.hlsli",
            "#include \"nested/Types.hlsli\"\n"
            "float4 TestColor() { return NestedColor(); }\n") ||
        !WriteText(
            root / "nested" / "Types.hlsli",
            "float4 NestedColor() { return float4(1, 0, 0, 1); }\n"))
    {
        return 1;
    }

    const std::vector<fs::path> roots { root };
    const fs::path resolved =
        nsk::ResolveShaderIncludePath(
            "Common.hlsli",
            source.parent_path(),
            roots);
    if (resolved != root / "Common.hlsli")
    {
        return 2;
    }

    nsk::ShaderIncludeResolver resolver(
        source,
        roots);
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompileFromFile(
        source.c_str(),
        nullptr,
        &resolver,
        "PSMain",
        "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &bytecode,
        &errors);
    if (FAILED(result) ||
        !bytecode)
    {
        return 3;
    }

    if (!WriteText(
            source.parent_path() /
                "Common.hlsli",
            "float4 TestColor() { return float4(0, 1, 0, 1); }\n"))
    {
        return 4;
    }
    const fs::path localResolved =
        nsk::ResolveShaderIncludePath(
            "Common.hlsli",
            source.parent_path(),
            roots);
    if (localResolved !=
        source.parent_path() /
            "Common.hlsli")
    {
        return 5;
    }

    fs::remove_all(root, error);
    return 0;
}
