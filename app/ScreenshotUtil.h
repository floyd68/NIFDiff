#pragma once

#include <d2d1.h>

#include <string>

namespace FD2D
{
class Backplate;
}

namespace nsk::ScreenshotUtil
{

bool SaveRenderSurfaceRectPng(
    FD2D::Backplate& backplate,
    const D2D1_RECT_F& logicalRect,
    const std::wstring& pngPath);

}
