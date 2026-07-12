// main.cpp - placeholder entry point.
//
// NIFDiff is scaffolded but not yet implemented: NIF parsing (NifDocument),
// scene building, the D3D11 renderer, and the side-by-side Compare UI still
// need to land in this repo's core/, render/, ui/ folders - see README.md's
// "Origins" and "Next steps" sections. Until that happens, this just proves
// the FD2D/Floar/ImageCore/DirectXTex toolchain links.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    MessageBoxW(nullptr,
        L"NIFDiff scaffold placeholder.\n\n"
        L"Core/render/ui not ported yet - see README.md.",
        L"NIFDiff",
        MB_OK | MB_ICONINFORMATION);
    return 0;
}
