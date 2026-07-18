#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <DirectXTex.h>

#include "ImageCore/DecodedImage.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageLoader.h"
#include "ImageCore/ImageRequest.h"

#include <filesystem>
#include <future>
#include <cstring>
#include <iostream>
#include <utility>

namespace
{

struct DecodeResult
{
    HRESULT result { E_FAIL };
    ImageCore::DecodedImage image {};
};

DecodeResult Decode(
    const std::wstring& path,
    uint32_t mipLevel)
{
    ImageCore::ImageRequest request(
        path,
        ImageCore::ImagePurpose::FullResolution);
    request.allowGpuCompressedDDS = false;
    request.mipLevel = mipLevel;

    std::promise<DecodeResult> promise;
    std::future<DecodeResult> future =
        promise.get_future();
    ImageCore::ImageLoader::Instance().RequestDecoded(
        request,
        [&promise](
            HRESULT result,
            ImageCore::DecodedImage image)
        {
            promise.set_value(
                DecodeResult {
                    result,
                    std::move(image)
                });
        });
    return future.get();
}

bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path TemporaryPath(
    const wchar_t* extension)
{
    wchar_t directory[MAX_PATH] {};
    GetTempPathW(MAX_PATH, directory);
    return std::filesystem::path(directory) /
        (L"NIFDiff_ImageMipDecode_" +
         std::to_wstring(GetCurrentProcessId()) +
         extension);
}

}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);
    if (FAILED(comResult))
    {
        return 1;
    }

    bool passed = true;
    const std::filesystem::path ddsPath =
        TemporaryPath(L".dds");
    const std::filesystem::path tgaPath =
        TemporaryPath(L".tga");

    DirectX::ScratchImage chain;
    HRESULT result = chain.Initialize2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        8,
        4,
        1,
        4);
    passed &= Check(
        SUCCEEDED(result),
        "Failed to initialize DDS mip chain");
    if (SUCCEEDED(result))
    {
        for (size_t mip = 0; mip < 4; ++mip)
        {
            const DirectX::Image* image =
                chain.GetImage(mip, 0, 0);
            if (image)
            {
                memset(
                    image->pixels,
                    static_cast<int>(0x20 + mip),
                    image->slicePitch);
            }
        }
        result = DirectX::SaveToDDSFile(
            chain.GetImages(),
            chain.GetImageCount(),
            chain.GetMetadata(),
            DirectX::DDS_FLAGS_NONE,
            ddsPath.c_str());
        passed &= Check(
            SUCCEEDED(result),
            "Failed to save DDS fixture");

        const DirectX::Image* top =
            chain.GetImage(0, 0, 0);
        if (top)
        {
            result = DirectX::SaveToTGAFile(
                *top,
                DirectX::TGA_FLAGS_NONE,
                tgaPath.c_str());
            passed &= Check(
                SUCCEEDED(result),
                "Failed to save TGA fixture");
        }
    }

    if (passed)
    {
        ImageCore::RegisterBuiltInDecoders();

        const DecodeResult mip0 =
            Decode(ddsPath.wstring(), 0);
        passed &= Check(
            SUCCEEDED(mip0.result) &&
                mip0.image.width == 8 &&
                mip0.image.height == 4 &&
                mip0.image.sourceMipLevels == 4 &&
                mip0.image.sourceMipIndex == 0,
            "DDS mip 0 metadata mismatch");

        const DecodeResult mip2 =
            Decode(ddsPath.wstring(), 2);
        passed &= Check(
            SUCCEEDED(mip2.result) &&
                mip2.image.width == 2 &&
                mip2.image.height == 1 &&
                mip2.image.sourceMipLevels == 4 &&
                mip2.image.sourceMipIndex == 2,
            "DDS mip 2 metadata mismatch");

        const DecodeResult outOfRange =
            Decode(ddsPath.wstring(), 4);
        if (outOfRange.result != E_INVALIDARG)
        {
            std::cerr
                << "Out-of-range result: 0x"
                << std::hex
                << static_cast<unsigned long>(
                    outOfRange.result)
                << std::dec
                << '\n';
        }
        passed &= Check(
            outOfRange.result == E_INVALIDARG,
            "Out-of-range DDS mip was not rejected");

        const DecodeResult nonDdsMip =
            Decode(tgaPath.wstring(), 1);
        passed &= Check(
            nonDdsMip.result == E_INVALIDARG,
            "Non-DDS mip request was not rejected");

        ImageCore::ImageRequest request0(
            ddsPath.wstring());
        ImageCore::ImageRequest request2 = request0;
        request2.mipLevel = 2;
        passed &= Check(
            !(request0 == request2) &&
                std::hash<ImageCore::ImageRequest> {}(
                    request0) !=
                std::hash<ImageCore::ImageRequest> {}(
                    request2),
            "Mip level is missing from request identity");

        ImageCore::ImageLoader::Instance().Shutdown();
    }

    std::error_code error;
    std::filesystem::remove(ddsPath, error);
    std::filesystem::remove(tgaPath, error);
    CoUninitialize();
    return passed ? 0 : 1;
}
