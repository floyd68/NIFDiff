// main.cpp - entry point. Actual bootstrap lives in NIFDiffApp.cpp so it can
// be exercised independently of the CRT entry point shape (see README.md's
// "Origins"/"Next steps" sections).
#include "NIFDiffApp.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;
    return nsk::RunNIFDiffApp(hInstance, lpCmdLine, nCmdShow);
}
