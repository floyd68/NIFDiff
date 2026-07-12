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
#   - gli           https://github.com/g-truc/gli.git, at
#                   third_party/external/gli (header-only DDS decode for
#                   render/TextureCache.cpp; not a floyd68 project, so it
#                   lives under external/ alongside DirectXTex/spdlog rather
#                   than directly under third_party/ like FD2D/Floar/ImageCore)
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
add_executable(NIFDiff WIN32
    app/AppSettings.h
    app/FileDialog.h
    app/FileDialog.cpp
    app/NIFDiffApp.h
    app/NIFDiffApp.cpp
    app/main.cpp
    core/NifTypes.h
    core/NifStream.h
    core/NifValue.h
    core/NifItem.h
    core/NifDocument.h
    core/NifDocument.cpp
    core/SceneBuilder.h
    core/SceneBuilder.cpp
    core/NifLog.h
    core/Camera.h
    core/ResourceResolver.h
    core/ResourceResolver.cpp
    render/Shaders.h
    render/D3D11Renderer.h
    render/D3D11Renderer.cpp
    render/TextureCache.h
    render/TextureCache.cpp
    ui/NifViewport.h
    ui/NifViewport.cpp
    ui/NifComparePane.h
    ui/NifComparePane.cpp
    ui/NifCompareSplitCoordinator.h
    ui/NifCompareSplitCoordinator.cpp
    ui/NifCompareControlPanel.h
    ui/NifCompareControlPanel.cpp
    ui/NifCompareView.h
    ui/NifCompareView.cpp
)

set_target_properties(NIFDiff PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

target_include_directories(NIFDiff PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/gli
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/gli/external
    # core/NifLog.h logs via spdlog directly (header-only; see its own
    # comment for why this isn't routed through third_party/AppLog.h).
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/spdlog/include
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

# ---------------------------------------------------------------------------
# NifValidate - console-mode parser smoke-test
# ---------------------------------------------------------------------------
add_executable(NifValidate
    app/ValidateNif.cpp
    core/NifTypes.h
    core/NifStream.h
    core/NifValue.h
    core/NifItem.h
    core/NifDocument.h
    core/NifDocument.cpp
    core/SceneBuilder.h
    core/SceneBuilder.cpp
    core/NifLog.h
)
target_include_directories(NifValidate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/spdlog/include
)
target_compile_definitions(NifValidate PRIVATE NOMINMAX)
set_target_properties(NifValidate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
if(MSVC)
    target_compile_options(NifValidate PRIVATE /permissive- /utf-8 /W3)
endif()

# ---------------------------------------------------------------------------
# ResourceResolveTest - Bethesda search-order smoke test (uses Floar for BSA)
# ---------------------------------------------------------------------------
add_executable(ResourceResolveTest
    app/ResourceResolveTest.cpp
    core/ResourceResolver.h
    core/ResourceResolver.cpp
)
target_include_directories(ResourceResolveTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(ResourceResolveTest PRIVATE Floar advapi32)
target_compile_definitions(ResourceResolveTest PRIVATE NOMINMAX)
set_target_properties(ResourceResolveTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
if(MSVC)
    target_compile_options(ResourceResolveTest PRIVATE /permissive- /utf-8 /W3)
endif()
