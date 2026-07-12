// NIFDiffApp.h - application bootstrap: FD2D::Application::Initialize ->
// create window -> message loop -> Shutdown, hosting a single
// nsk::NifCompareView (dynamic 2-4 pane NIF compare). Mirrors the wWinMain
// body shape used by FICture2.cpp and liteviewer's own
// NifLiteViewerApp.cpp, adapted from a hardcoded Left/Right pane pair to N
// panes (see NifCompareView.h) and NIFDiff's own branding (window title,
// NIFDiff.ini).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{

// Runs the full application lifecycle and returns the process exit code.
int RunNIFDiffApp(HINSTANCE hInstance, LPWSTR cmdLine, int nCmdShow);

} // namespace nsk
