# NIFDiff - Qt-free NIF file viewer/diff tool for Bethesda games.
#
# NIFDiff is FICture2 (D:\Works\Ficture2, the author's own DDS/image
# compare tool, MIT licensed) with a NIF Model View feature added: its FD2D
# UI framework, Floar VFS/BSA-BA2 support, and app-shell conventions carry
# over directly. The NIF parsing/scene/compare-view logic FICture2 never
# had is ported in from NifSkope
# (https://www.nifskope.com/, https://github.com/niftools/nifskope,
# permissive 3-clause-BSD-style license, D:\Works\nifskope\LICENSE.md).
# See README.md's "Origins" section for exactly which source maps to which
# file here, and the license notes that come with each.
#
# Dependencies live as git submodules
# under third_party/ (never as hard-coded paths into another local checkout):
#   - FD2D          https://github.com/floyd68/FD2D.git
#   - ImageCore     https://github.com/floyd68/ImageCore.git
#   - DirectXTex    https://github.com/floyd68/DirectXTex.git
#   - Floar         https://github.com/floyd68/Floar.git
#                   (VirtualPath/VFS + BSA/BA2; nested zlib/xz/libarchive)
#   - spdlog        https://github.com/gabime/spdlog.git, at
#                   third_party/external/spdlog (header-only; FD2D's own
#                   CMakeLists.txt adds its include/ path relative to its
#                   own location, so it needs no wiring here)
#   - CommonUtil.h  small header-only util still included by FD2D as
#                   ../CommonUtil.h (vendored copy next to the FD2D submodule)
#   - AppLog.h      startup-timing/logging macros FD2D includes as
#                   ../AppLog.h (vendored copy next to the FD2D submodule;
#                   AppLog.cpp/spdlog::init are NOT vendored, so the macros
#                   are no-ops until NIFDiff wants real logging - see the
#                   comment at the top of third_party/AppLog.h)
cmake_minimum_required(VERSION 3.21)

project(NIFDiff LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(NIFDIFF_THIRD_PARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(NOT EXISTS "${NIFDIFF_THIRD_PARTY_ROOT}/FD2D/CMakeLists.txt")
    message(FATAL_ERROR
        "NIFDiff: could not find FD2D at "
        "'${NIFDIFF_THIRD_PARTY_ROOT}/FD2D'. Run "
        "'git submodule update --init --recursive' from the repo root.")
endif()
if(NOT EXISTS "${NIFDIFF_THIRD_PARTY_ROOT}/Floar/CMakeLists.txt")
    message(FATAL_ERROR
        "NIFDiff: could not find Floar at "
        "'${NIFDIFF_THIRD_PARTY_ROOT}/Floar'. Run "
        "'git submodule update --init --recursive' from the repo root.")
endif()

function(nifdiff_set_cache_bool name value)
    set(${name} ${value} CACHE BOOL "Forced by NIFDiff" FORCE)
endfunction()

# DirectXTex: only decode/format-conversion via ImageCore.
nifdiff_set_cache_bool(BUILD_TOOLS OFF)
nifdiff_set_cache_bool(BUILD_SAMPLE OFF)
nifdiff_set_cache_bool(BUILD_SHARED_LIBS OFF)
nifdiff_set_cache_bool(BUILD_DX11 OFF)
nifdiff_set_cache_bool(BUILD_DX12 OFF)

# Floar: BSA/BA2 is enough for Bethesda texture search; skip ZIP/7z/RAR
# (and Floar's xz/libarchive nest) unless a future feature needs them.
nifdiff_set_cache_bool(FLOAR_ENABLE_COMMON_ARCHIVES OFF)

add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/external/DirectXTex" "${CMAKE_BINARY_DIR}/_deps/DirectXTex")
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/ImageCore" "${CMAKE_BINARY_DIR}/_deps/ImageCore")
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/FD2D" "${CMAKE_BINARY_DIR}/_deps/FD2D")
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/Floar" "${CMAKE_BINARY_DIR}/_deps/Floar")

# ---------------------------------------------------------------------------
# NIFDiff executable
# ---------------------------------------------------------------------------
# NOTE: only app/main.cpp exists so far (placeholder). As core/render/ui are
# implemented (see README.md's "Origins" section for the NifSkope/FICture2
# source mapping), add their files here, e.g.:
#   core/NifTypes.h  core/NifStream.h  core/NifValue.h  core/NifItem.h
#   core/NifDocument.h/.cpp  core/SceneBuilder.h/.cpp  core/Camera.h
#   core/ResourceResolver.h/.cpp
#   render/Shaders.h  render/D3D11Renderer.h/.cpp  render/TextureCache.h/.cpp
#   ui/NifViewport.h/.cpp  ui/NifCompareControlPanel.h/.cpp
#   ui/NifCompareView.h/.cpp
add_executable(NIFDiff WIN32
    app/main.cpp
)

set_target_properties(NIFDiff PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

target_include_directories(NIFDiff PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    # CommonUtil.h lives at third_party/ for FD2D's "../CommonUtil.h" includes.
    ${NIFDIFF_THIRD_PARTY_ROOT}
)

target_compile_definitions(NIFDiff PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
    NOMINMAX
)

target_link_libraries(NIFDiff PRIVATE
    FD2D
    ImageCore
    DirectXTex
    Floar
    comctl32
    ole32
    shell32
    windowscodecs
    d2d1
    dwrite
    d3d11
    dxgi
    d3dcompiler
    advapi32
)

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    target_compile_options(NIFDiff PRIVATE /permissive- /utf-8 /W3)
endif()
