# NIFDiff

A standalone, Qt-free tool for viewing and diffing NIF files (Bethesda's
NetImmerse/Gamebryo model format - Skyrim, Skyrim SE, Fallout 4, etc.)
side by side in 3D.

**NIFDiff is FICture2 (`D:\Works\Ficture2`, an image/DDS-texture viewer and
compare tool by the same author) with a NIF Model View feature added on
top.** FICture2 already solved "view/compare two-to-four things side by
side, synced, fast, Qt-free" for 2D textures; NIFDiff extends that same app
lineage - its FD2D UI framework, its Floar VFS/BSA-BA2 archive support, its
app-shell conventions - to 3D NIF models. The actual NIF-parsing/3D-scene/
compare-UI logic needed for that (which FICture2 never had) is ported in
from NifSkope - see "Origins" below.

This repo is currently an **empty scaffold**: directory layout, CMake/build
setup, and third-party wiring are in place, but no NIF parsing, rendering,
or compare-UI code has been ported in yet. `app/main.cpp` is a placeholder
that just proves the toolchain links - confirmed with an actual
`cmake --build build --config Debug` run.

Third-party submodules (`FD2D`, `ImageCore`, `Floar`, `external/DirectXTex`,
`external/spdlog`) are independent clones from their GitHub URLs, not
references into the local `D:\Works\Ficture2` checkout - this repo owns its
own copies.

## Origins

NIFDiff descends from two existing local projects, not written from
scratch:

### FICture2 - the parent app (same author)

**`D:\Works\Ficture2`** is "FICture2 - Ultra-Fast Image Compare & DDS
Texture Viewer for Modders", the author's own tool (MIT licensed,
Copyright (c) 2024 EunSuk, Lee / floyd) for viewing and comparing up to 4
DDS/PNG/JPG/etc. textures side by side with synced selection/zoom/pan, BSA/
BA2/archive browsing, and session persistence. NIFDiff is that same
lineage, pointed at 3D NIF models instead of 2D textures - not just reusing
its libraries, but continuing the same product idea ("compare things side
by side, fast, no Qt/Electron bloat").

What NIFDiff carries over from FICture2 directly:
- **FD2D** - the Win32 UI framework (Direct2D/DirectWrite + optional D3D11
  swapchain) FICture2's whole UI is built on.
- **Floar** - VFS/BSA-BA2 archive support (FICture2 already browses `.ba2`
  archives for textures; NIFDiff needs the same search order for meshes).
- **ImageCore** / **external/DirectXTex** - the decode pipeline NIFDiff's
  future `TextureCache` will reuse for material textures on NIF models.
- **`CommonUtil.h`** / **`AppLog.h`** - small vendored headers FD2D expects
  as `../CommonUtil.h`/`../AppLog.h` next to itself (see
  `third_party/CommonUtil.h`/`AppLog.h`).
- Its **side-by-side compare composition pattern** - the planned NIF
  Compare view (see below) is meant to explicitly mirror FICture2's own
  `ImageBrowser`-pair-plus-controls layout (two panes + a shared bottom
  control strip), not something designed from scratch.
- App-shell conventions (`Application.cpp`/`AppSetup`/`AppLog`-style
  bootstrap, per-user INI session persistence, submodule/CMake layout)
  once `app/` grows past the current placeholder.

FICture2 is MIT licensed (`D:\Works\Ficture2\LICENSE`) - permissive, no
notice-retention requirement beyond the standard MIT copyright/license
text.

### NifSkope - source of the NIF/3D logic

The one thing FICture2 never had - actually parsing a NIF file and
rendering it in 3D - is ported in as a Qt-free reimplementation of parts of
**[NifSkope](https://www.nifskope.com/)**
([github.com/niftools/nifskope](https://github.com/niftools/nifskope)),
the long-standing Qt-based NIF editor:

- `core/NifDocument.h/.cpp` (once implemented) is a Qt-free, curated
  reimplementation of NifSkope's generic `src/model/basemodel.h` +
  `src/model/nifmodel.h` (NifSkope drives these off `nif.xml` at runtime
  for full generality; NIFDiff instead direct-parses a fixed set of
  Skyrim LE/SE/FO4 block types - see the scope note this needs at the top
  of `NifDocument.h`).
- `core/SceneBuilder.h/.cpp` is a much-reduced stand-in for NifSkope's
  `src/gl/glscene.h`/`glnode.h` scenegraph (world-transform flattening
  only; no skinning/morphs/particles/controllers needed for a static
  bind-pose comparison).
- `render/D3D11Renderer.h/.cpp` / `render/TextureCache.h/.cpp` replace
  NifSkope's Qt/OpenGL `src/gl/renderer.cpp/.h` and
  `gltex.cpp`+`gltexloaders.cpp` with a D3D11 draw path (feeding off
  FICture2's ImageCore/DirectXTex decode pipeline instead of NifSkope's
  own texture loaders).
- `ui/NifCompareView.h/.cpp` reimplements the layout of NifSkope's own
  `src/ui/nifdiffviewer.cpp`/`.h` (the Qt `File -> NIF Diff Viewer...`
  feature), documented in `D:\Works\nifskope\NIF_DIFF_VIEWER.md` - laid
  out FICture2-style (see the FICture2 section above).
- `schema_reference/nif.xml` (once added) is vendored from the upstream
  [niftools/nifxml](https://github.com/niftools/nifxml) project that
  NifSkope itself uses, kept here for reference only (not parsed at
  build/run time - see `schema_reference/NOTES.md`).
- `test_assets/*.nif` (once added) are small Skyrim SE/Fallout 4 NIF
  fixtures used to smoke-test the parser.

NifSkope is distributed under a permissive 3-clause-BSD-style license (see
`D:\Works\nifskope\LICENSE.md`) that requires retaining its copyright
notice and disclaimer in redistributed source/binaries derived from it.
Keep that notice alongside any ported `core/`/`render/`/`ui` code that
traces back to NifSkope sources - this is a separate, additional
obligation on top of FICture2's own MIT license above, since the two
pieces of ported code carry different licenses.

## Layout

```
app/               placeholder entry point (main.cpp) - see "Next steps"
core/              NIF parsing / scene building (not yet implemented - NOTES.md)
render/            D3D11 renderer / texture cache (not yet implemented - NOTES.md)
ui/                Compare view / control panel (not yet implemented - NOTES.md)
schema_reference/  nif.xml reference (not yet added - NOTES.md)
test_assets/       smoke-test .nif fixtures (not yet added - NOTES.md)
third_party/       git submodules: FD2D, Floar, ImageCore, external/DirectXTex,
                   external/spdlog + vendored third_party/CommonUtil.h,
                   third_party/AppLog.h
CMakeLists.txt     sets up the FD2D/Floar/ImageCore/DirectXTex/spdlog
                   third-party wiring
GenerateVS2026.ps1/.bat   configures a VS2026 solution under build/
```

## Build

```powershell
git submodule update --init --recursive   # first time only
.\GenerateVS2026.ps1
cmake --build build --config Debug
```

`GenerateVS2026.ps1` runs the submodule update for you if `third_party/FD2D`
or `third_party/Floar` look uninitialized.

## Next steps

1. Implement `core/`, `render/`, `ui/` per the file breakdown in "Origins"
   above - NIF parsing/scene-building adapted from NifSkope's
   model/scenegraph, a D3D11 render path, and an FD2D Compare view laid
   out FICture2-style.
2. Add `schema_reference/nif.xml` (from niftools/nifxml, for parser
   reference) and a handful of Skyrim SE/Fallout 4 `test_assets/*.nif`
   fixtures.
3. Vendor `lib/gli` for DDS decode in `TextureCache.cpp` (not yet present
   in this repo).
4. Add those sources to `CMakeLists.txt`'s `NIFDiff` target (the file lists
   the expected set as a comment) and replace `app/main.cpp`'s placeholder
   with a real app shell (`AppSettings.h`, `FileDialog.h/.cpp`, and an
   `Application.cpp`-style bootstrap per FICture2's own conventions),
   hosting one Compare view.
5. Decide on naming/branding (window title, `.ico`, INI section names,
   C++ namespace) for the new code.
