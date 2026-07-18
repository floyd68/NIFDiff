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
#   - Floar         https://github.com/floyd68/Floar.git
#                   (VirtualPath/VFS + BSA/BA2; nested zlib/xz/libarchive)
#   - DirectXTex    https://github.com/floyd68/DirectXTex.git, at
#                   third_party/external/DirectXTex (DDS decode + D3D11 SRV
#                   creation for render/TextureCache.cpp)
#   - spdlog        https://github.com/gabime/spdlog.git, at
#                   third_party/external/spdlog (header-only; FD2D's own
#                   CMakeLists.txt adds its include/ path relative to its
#                   own location, so it needs no wiring here)
#
# Removed in the dependency cleanup (all were vestiges of FD2D's former
# image-viewer stack, before it was slimmed to a handle-only Image widget):
# the ImageCore submodule (was forced as an FD2D sibling; never called by
# NIFDiff code), the gli submodule (the pre-DirectXTex DDS decoder inherited
# from the OpenGL-era liteviewer port), and the vendored CommonUtil.h /
# AppLog.h loose headers (FD2D no longer includes ../CommonUtil.h, and
# AppLog.h's FIC2_LOG_* macros were never wired up here - core/NifLog.h is
# NIFDiff's actual logging).
cmake_minimum_required(VERSION 3.21)

# app/res/version.h.in is the single, hand-maintained version source. Read its
# numeric components here so CMake project metadata and the generated header
# cannot drift apart.
set(NIFDIFF_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/app/res/version.h.in")
file(STRINGS "${NIFDIFF_VERSION_TEMPLATE}" NIFDIFF_VERSION_DEFINES
    REGEX "^#define NIFDIFF_VERSION_(MAJOR|MINOR|REVISION)[ \t]+[0-9]+")
foreach(component MAJOR MINOR REVISION)
    foreach(line IN LISTS NIFDIFF_VERSION_DEFINES)
        if(line MATCHES "^#define NIFDIFF_VERSION_${component}[ \t]+([0-9]+)")
            set(NIFDIFF_VERSION_${component} "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT DEFINED NIFDIFF_VERSION_${component})
        message(FATAL_ERROR
            "NIFDiff: NIFDIFF_VERSION_${component} is missing from "
            "${NIFDIFF_VERSION_TEMPLATE}")
    endif()
endforeach()

set(NIFDIFF_VERSION
    "${NIFDIFF_VERSION_MAJOR}.${NIFDIFF_VERSION_MINOR}.${NIFDIFF_VERSION_REVISION}")
file(READ "${NIFDIFF_VERSION_TEMPLATE}" NIFDIFF_VERSION_HEADER_TEXT)
foreach(string_macro STRING WSTR)
    if(NOT NIFDIFF_VERSION_HEADER_TEXT MATCHES
        "#define[ \t]+NIFDIFF_VERSION_${string_macro}[ \t]+L?\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
        message(FATAL_ERROR
            "NIFDiff: NIFDIFF_VERSION_${string_macro} is missing from "
            "${NIFDIFF_VERSION_TEMPLATE}")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL NIFDIFF_VERSION)
        message(FATAL_ERROR
            "NIFDiff: NIFDIFF_VERSION_${string_macro} (${CMAKE_MATCH_1}) does "
            "not match the numeric version (${NIFDIFF_VERSION})")
    endif()
endforeach()

project(NIFDiff
    VERSION ${NIFDIFF_VERSION}
    LANGUAGES C CXX)
enable_testing()

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

# DirectXTex: DDS decode + SRV creation for render/TextureCache.cpp.
nifdiff_set_cache_bool(BUILD_TOOLS OFF)
nifdiff_set_cache_bool(BUILD_SAMPLE OFF)
nifdiff_set_cache_bool(BUILD_SHARED_LIBS OFF)
# BUILD_DX11 provides DirectXTexD3D11.cpp's CreateShaderResourceView/
# CreateTexture helpers, which render/TextureCache.cpp uses for DDS -> SRV
# upload.
nifdiff_set_cache_bool(BUILD_DX11 ON)
nifdiff_set_cache_bool(BUILD_DX12 OFF)

# Floar archive support. BSA/BA2 cover Bethesda's texture search, but the
# thumbnail-strip browser opens ANY archive Floar supports, so enable the common
# formats (ZIP/7z/RAR via Floar's libarchive/xz nest) too - the dialog filter
# and strip navigation follow ArchiveReaderFactory::GetSupportedExtensions().
nifdiff_set_cache_bool(FLOAR_ENABLE_COMMON_ARCHIVES ON)

add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/external/DirectXTex" "${CMAKE_BINARY_DIR}/_deps/DirectXTex")
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/FD2D" "${CMAKE_BINARY_DIR}/_deps/FD2D")
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/Floar" "${CMAKE_BINARY_DIR}/_deps/Floar")
# ImageCore (floyd68/ImageCore): async image decode (WIC + DirectXTex) for the
# ported texture view/compare. Its CMakeLists expects DirectXTex at
# ../external/DirectXTex - true for this layout - and links the DirectXTex
# target added just above, so it must follow it.
add_subdirectory("${NIFDIFF_THIRD_PARTY_ROOT}/ImageCore" "${CMAKE_BINARY_DIR}/_deps/ImageCore")

# ---------------------------------------------------------------------------
# Shader precompilation: render/shaders/*.hlsl -> bytecode headers via fxc.
# RenderDevice.cpp embeds the generated g_* arrays directly, so startup pays
# no D3DCompile cost (~260ms for the six entry points, measured via
# core/StartupTrace.h) and the exe keeps zero runtime shader-file
# dependencies. /Ges mirrors the D3DCOMPILE_ENABLE_STRICTNESS flag the old
# runtime D3DCompile path used; optimization stays at fxc's default (POM
# SampleLevel loops TDR the GPU when left unoptimized - see the comment
# that used to live in RenderDevice::CreateShaders).
# ---------------------------------------------------------------------------
find_program(NIFDIFF_FXC_TOOL FXC.EXE
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_SYSTEM_VERSION}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NOT NIFDIFF_FXC_TOOL)
    message(FATAL_ERROR "NIFDiff: fxc.exe not found (install a Windows 10/11 SDK).")
endif()
message(STATUS "NIFDiff: compiling shaders with ${NIFDIFF_FXC_TOOL}")

set(NIFDIFF_SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${NIFDIFF_SHADER_OUT_DIR}")

set(NIFDIFF_COMPILED_SHADER_HEADERS "")
function(nifdiff_compile_shader hlsl entry profile varname header)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/${hlsl}")
    set(out "${NIFDIFF_SHADER_OUT_DIR}/${header}")
    add_custom_command(
        OUTPUT "${out}"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/Common.hlsli"
        COMMAND "${NIFDIFF_FXC_TOOL}" /nologo /T ${profile} /E ${entry} /Ges
                /Fh "${out}" /Vn ${varname} "${src}"
        COMMENT "fxc ${hlsl} ${entry} (${profile}) -> ${header}"
        VERBATIM)
    list(APPEND NIFDIFF_COMPILED_SHADER_HEADERS "${out}")
    set(NIFDIFF_COMPILED_SHADER_HEADERS "${NIFDIFF_COMPILED_SHADER_HEADERS}" PARENT_SCOPE)
endfunction()

nifdiff_compile_shader(Lit.hlsl       VSMain vs_5_0 g_LitVS       LitVS.h)
nifdiff_compile_shader(Lit.hlsl       PSMain ps_5_0 g_LitPS       LitPS.h)
nifdiff_compile_shader(Unlit.hlsl     VSMain vs_5_0 g_UnlitVS     UnlitVS.h)
nifdiff_compile_shader(Unlit.hlsl     PSMain ps_5_0 g_UnlitPS     UnlitPS.h)
nifdiff_compile_shader(Highlight.hlsl VSMain vs_5_0 g_HighlightVS HighlightVS.h)
nifdiff_compile_shader(Highlight.hlsl PSMain ps_5_0 g_HighlightPS HighlightPS.h)

# Keep the .hlsl sources visible in the IDE without Visual Studio's built-in
# FXC step double-compiling them (the custom commands above own that).
set(NIFDIFF_SHADER_SOURCES
    render/shaders/Common.hlsli
    render/shaders/Lit.hlsl
    render/shaders/Unlit.hlsl
    render/shaders/Highlight.hlsl
)
set_source_files_properties(${NIFDIFF_SHADER_SOURCES} PROPERTIES VS_TOOL_OVERRIDE "None")

# ---------------------------------------------------------------------------
# Version header (generated/version.h)
#
# The checked-in template contains the fixed release version. configure_file
# keeps the build tree header available to both C++ and RC; changing the source
# template automatically causes CMake to regenerate it.
# ---------------------------------------------------------------------------
set(NIFDIFF_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${NIFDIFF_GENERATED_DIR}")
configure_file(
    "${NIFDIFF_VERSION_TEMPLATE}"
    "${NIFDIFF_GENERATED_DIR}/version.h"
    COPYONLY)

# ---------------------------------------------------------------------------
# NIFDiff executable
# ---------------------------------------------------------------------------
add_executable(NIFDiff WIN32
    ${NIFDIFF_SHADER_SOURCES}
    ${NIFDIFF_COMPILED_SHADER_HEADERS}
    app/AppIpc.h
    app/AppIpc.cpp
    app/IniStore.h
    app/AppSetup.h
    app/AppSetup.cpp
    app/FileDialog.h
    app/FileDialog.cpp
    app/ScreenshotUtil.h
    app/ScreenshotUtil.cpp
    app/FloarPathByteSource.h
    app/FloarPathByteSource.cpp
    app/NIFDiffApp.h
    app/NIFDiffApp.cpp
    app/main.cpp
    app/res/resource.h
    app/res/NIFDiff.rc
    app/res/version.h.in
    core/NifTypes.h
    core/NifStream.h
    core/NifValue.h
    core/NifItem.h
    core/NifDocument.h
    core/NifDocument.cpp
    core/AnimController.h
    core/AnimController.cpp
    core/SceneBuilder.h
    core/SceneBuilder.cpp
    core/NifLog.h
    core/StartupTrace.h
    core/Camera.h
    core/ResourceResolver.h
    core/ResourceResolver.cpp
    core/ResourceManager.h
    core/ResourceManager.cpp
    render/RenderDevice.h
    render/RenderDevice.cpp
    render/ShaderIncludeResolver.h
    render/ShaderIncludeResolver.cpp
    render/RenderTarget.h
    render/RenderTarget.cpp
    render/RenderMeshCache.h
    render/TextureCache.h
    render/TexturePipelineState.h
    render/TexturePipelineState.cpp
    render/TextureRepository.h
    render/TextureRepository.cpp
    ui/IpcOpenRequest.h
    ui/NifViewport.h
    ui/NifViewport.cpp
    ui/PaneContent.h
    ui/ComparePane.h
    ui/ComparePane.cpp
    ui/ImageAlphaPresentation.h
    ui/ImageAlphaPresentation.cpp
    ui/ImagePresentation.h
    ui/ImagePresentation.cpp
    ui/ImagePane.h
    ui/ImagePane.cpp
    ui/ImageGpuResourceCache.h
    ui/ImageGpuResourceCache.cpp
    ui/NifComparePane.h
    ui/NifComparePane.cpp
    ui/NifCompareSplitCoordinator.h
    ui/NifCompareSplitCoordinator.cpp
    ui/NifCompareControlPanel.h
    ui/NifCompareControlPanel.cpp
    ui/NifCompareView.h
    ui/NifCompareView.cpp
    ui/ThumbnailStrip.h
    ui/ThumbnailStrip.cpp
)

set_target_properties(NIFDiff PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

# Ship the editable shader sources next to the exe (shaders\*.hlsl). At
# runtime RenderDevice prefers these over the embedded fxc bytecode, so users
# can modify the shaders (with the embedded copies as a safe fallback) - see
# RenderDevice.cpp's shader-override section.
add_custom_command(TARGET NIFDiff POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NIFDiff>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/Common.hlsli
            ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/Lit.hlsl
            ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/Unlit.hlsl
            ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/Highlight.hlsl
            ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/shaders.ini
            "$<TARGET_FILE_DIR:NIFDiff>/shaders"
    COMMENT "Copying editable shader sources to $<TARGET_FILE_DIR:NIFDiff>/shaders")

set_source_files_properties(app/res/version.h.in PROPERTIES HEADER_FILE_ONLY ON)

target_include_directories(NIFDiff PRIVATE
    ${NIFDIFF_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    # fxc-generated shader bytecode headers (see "Shader precompilation").
    ${NIFDIFF_SHADER_OUT_DIR}
    # core/NifLog.h logs via spdlog directly (header-only; see its own
    # comment for why this isn't routed through third_party/AppLog.h).
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/spdlog/include
    # so ImagePane.cpp can include "ImageCore/<hdr>.h" (texture-view port).
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
    DirectXTex
    Floar
    ImageCore
    comctl32
    ole32
    shell32
    windowscodecs
    d2d1
    dwrite
    d3d11
    dxgi
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
    core/AnimController.h
    core/AnimController.cpp
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
    # ResourceResolver.cpp now logs its archive-scan timings via core/NifLog.h
    ${NIFDIFF_THIRD_PARTY_ROOT}/external/spdlog/include
)
target_link_libraries(ResourceResolveTest PRIVATE Floar advapi32)
target_compile_definitions(ResourceResolveTest PRIVATE NOMINMAX)
set_target_properties(ResourceResolveTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
add_test(NAME ResourceResolveTest COMMAND ResourceResolveTest)

# ---------------------------------------------------------------------------
# ImageViewTest - P1 validation harness for the texture view/compare port:
# a standalone window that decodes an image via ImageCore and displays it
# through FD2D::Image (proves the ported decode + display stack fits inside
# NIFDiff's build before the larger pane-abstraction work). Not shipped.
# ---------------------------------------------------------------------------
add_executable(ImageViewTest WIN32 app/ImageViewTest.cpp)
target_include_directories(ImageViewTest PRIVATE
    ${NIFDIFF_THIRD_PARTY_ROOT}          # so "ImageCore/<hdr>.h" resolves
)
target_link_libraries(ImageViewTest PRIVATE
    FD2D
    ImageCore
    DirectXTex
    d2d1
    d3d11
    dxgi
    dwrite          # FD2D::Core (DWriteCreateFactory)
    d3dcompiler     # FD2D::Image's GPU quad shaders (D3DCompile) - main app never pulls Image.obj
    windowscodecs
    ole32
    shell32
)
target_compile_definitions(ImageViewTest PRIVATE FD2D_STATIC UNICODE _UNICODE NOMINMAX)
set_target_properties(ImageViewTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
if(MSVC)
    target_compile_options(ImageViewTest PRIVATE /permissive- /utf-8 /W3)
endif()
if(MSVC)
    target_compile_options(ResourceResolveTest PRIVATE /permissive- /utf-8 /W3)
endif()

# ---------------------------------------------------------------------------
# TexturePipelineTest - pure format/publication-generation invariants
# ---------------------------------------------------------------------------
add_executable(TexturePipelineTest
    app/TexturePipelineTest.cpp
    render/TexturePipelineState.h
    render/TexturePipelineState.cpp
)
target_include_directories(TexturePipelineTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/render
)
target_compile_definitions(TexturePipelineTest PRIVATE NOMINMAX)
if(MSVC)
    target_compile_options(TexturePipelineTest PRIVATE /permissive- /utf-8 /W3)
endif()
add_test(NAME TexturePipelineTest COMMAND TexturePipelineTest)

# ---------------------------------------------------------------------------
# ImageMipDecodeTest - DDS mip metadata, selection, range, and cache identity
# ---------------------------------------------------------------------------
add_executable(ImageMipDecodeTest
    app/ImageMipDecodeTest.cpp
)
target_include_directories(ImageMipDecodeTest PRIVATE
    ${NIFDIFF_THIRD_PARTY_ROOT}
)
target_link_libraries(ImageMipDecodeTest PRIVATE
    ImageCore
    DirectXTex
    windowscodecs
    ole32
)
target_compile_definitions(ImageMipDecodeTest PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)
if(MSVC)
    target_compile_options(ImageMipDecodeTest PRIVATE
        /permissive-
        /utf-8
        /W3
    )
endif()
add_test(NAME ImageMipDecodeTest COMMAND ImageMipDecodeTest)

# ---------------------------------------------------------------------------
# RenderSurfaceCaptureTest - occlusion-independent composed-frame readback
# ---------------------------------------------------------------------------
add_executable(RenderSurfaceCaptureTest
    app/RenderSurfaceCaptureTest.cpp
)
target_include_directories(RenderSurfaceCaptureTest PRIVATE
    ${NIFDIFF_THIRD_PARTY_ROOT}/FD2D
)
target_link_libraries(RenderSurfaceCaptureTest PRIVATE
    FD2D
    d2d1
    dwrite
    d3d11
    dxgi
    d3dcompiler
    ole32
)
target_compile_definitions(RenderSurfaceCaptureTest PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
    NOMINMAX
)
if(MSVC)
    target_compile_options(RenderSurfaceCaptureTest PRIVATE /permissive- /utf-8 /W3)
endif()
add_test(NAME RenderSurfaceCaptureTest COMMAND RenderSurfaceCaptureTest)

# ---------------------------------------------------------------------------
# VirtualFileSystemTest - archive existence + root/subdirectory listing
# ---------------------------------------------------------------------------
add_executable(VirtualFileSystemTest
    app/VirtualFileSystemTest.cpp
)
target_link_libraries(VirtualFileSystemTest PRIVATE Floar)
target_compile_definitions(VirtualFileSystemTest PRIVATE NOMINMAX)
if(MSVC)
    target_compile_options(VirtualFileSystemTest PRIVATE /permissive- /utf-8 /W3)
endif()
add_test(NAME VirtualFileSystemTest COMMAND VirtualFileSystemTest)

# ---------------------------------------------------------------------------
# ShaderIncludeResolverTest - source-directory + shader-root include lookup
# ---------------------------------------------------------------------------
add_executable(ShaderIncludeResolverTest
    app/ShaderIncludeResolverTest.cpp
    render/ShaderIncludeResolver.h
    render/ShaderIncludeResolver.cpp
)
target_link_libraries(ShaderIncludeResolverTest PRIVATE d3dcompiler)
target_compile_definitions(ShaderIncludeResolverTest PRIVATE NOMINMAX)
if(MSVC)
    target_compile_options(ShaderIncludeResolverTest PRIVATE /permissive- /utf-8 /W3)
endif()
add_test(NAME ShaderIncludeResolverTest COMMAND ShaderIncludeResolverTest)
