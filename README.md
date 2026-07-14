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

NIF parsing (`core/`), D3D11 rendering (`render/`), and a dynamic 1-8 pane
FICture2-style compare UI (`ui/`, `app/`) are implemented and build/run -
confirmed with an actual `cmake --build build --config Debug` run producing
`NIFDiff.exe` (the app), `NifValidate.exe` (console parser smoke-test) and
`ResourceResolveTest.exe` (Bethesda search-order smoke-test), plus manual
verification of loading NIF files into panes, dynamically adding/removing
panes (up to 8, equal-width), and camera/lighting sync across panes.

Third-party submodules (`FD2D`, `Floar`, `external/DirectXTex`,
`external/spdlog`) are independent clones from their GitHub URLs, not
references into the local `D:\Works\Ficture2` checkout - this repo owns its
own copies.

## Features

### Side-by-side comparison

- **1-8 panes** in an equal-width grid (1-4 in a single row, 5-6 as 3x2,
  7-8 as 4x2), added and removed at runtime; per-pane Open/Close via the
  pane's own buttons or the right-click context menu.
- **Synced cameras and lighting** across panes (each sync individually
  toggleable), so one orbit/pan/zoom or light change hits every model at
  once.
- **Click-to-select**: a left click picks the sub-mesh under the cursor,
  overlays it in wireframe, and reports its shader classification; each
  pane also shows a shader-kind summary (e.g. `Default x8 · Parallax x3`)
  and total triangle count in its status line.
- Orientation presets, Center/Reset view, wireframe/grid/axes toggles
  (plus a "Hidden" toggle for NiAVObject-hidden marker subtrees),
  brightness/ambient sliders, light declination/planar angle, and a
  frontal-light mode.
- **Keyboard shortcuts**: `F` reset view, `G` grid, `X` axes,
  `W` wireframe, `H` hidden, `PgUp`/`PgDn` cycle the camera preset,
  `Ctrl+O` open a file into a pane, `F12` save a pane screenshot.
- **Per-pane context menu**: open/close the pane, open the containing
  folder in Explorer with the file selected, and save the pane's 3D
  render as a PNG (defaults next to the .nif, auto-numbered).

### Bethesda resource pipeline

- Loads Skyrim LE/SE and Fallout 4 NIFs for static bind-pose comparison
  (no skinning/morphs/particles/controllers - see `SceneBuilder.h`'s
  scope note).
- Textures resolve in engine order: override folders -> the NIF's own
  directory -> Game Data -> BSA/BA2 archives (via Floar), with Game Data
  auto-detection and an override-folder UI in the bottom strip. A
  named-but-unresolved diffuse renders magenta instead of blending in as
  a plain white surface.
- Textures are pooled process-wide - each unique resolved source is
  decoded and uploaded once, shared across panes - and BSA scanning runs
  in parallel in the background at startup (cold start around half a
  second).
- Session persistence (open files, Game Data path, override folders),
  `.nif` file association, and single-instance forwarding: opening a
  file whose NAME matches a document already open lands in a new pane of
  the existing window - the "compare two mods' versions of the same
  mesh" workflow; unrelated files start their own window.

### Material / shader coverage

- **Vanilla lit path**: specular, glow/emissive, environment mapping
  with per-material cubemaps, soft/rim/back lighting, model-space
  normals (with external specular maps), multilayer parallax, skin/hair
  tint, face tint/detail masks, effect shaders (falloff, greyscale
  palettes, weapon blood), NiAlphaProperty blend functions and alpha
  test, decals (depth-biased), double-sided materials; refraction planes
  are omitted rather than drawn wrong.
- **Parallax** (`_p.dds`): parallax occlusion mapping with height-field
  self-shadowing; the "Parallax Height" slider drives the vanilla/`_m`
  height scale.
- **Complex material** (ENB / Community Shaders `_m.dds`): detected by
  the ecosystem's non-opaque-coarsest-mip rule, channels read as
  R/G/B/A = reflection/glossiness/metalness/height. Complex-material
  parallax only runs over alphas that are real height fields - a
  CPU-side probe rejects flat alphas and binary 0/255 masks, and BC1
  sources are skipped outright (1-bit alpha cannot carry a height
  gradient).
- **True PBR** (Community Shaders / PBRNifPatcher): detected by the
  SLSF2_Unused01 marker; reads the albedo/normal/displacement/RMAOS
  (+ optional subsurface) slot conventions, shades with GGX/
  Cook-Torrance, runs displacement POM at the authored scale, and feeds
  ambient specular from a procedural sky/ground IBL cubemap so metals
  get directional, colored reflections. Brightness is calibrated to sit
  comparably next to the legacy path - a credible preview, not CS-exact.
- **Extended-material toggles**: Parallax / Complex Mat / True PBR
  checkboxes in the bottom strip, each enabled only while some loaded
  pane carries material it would affect. OFF renders the closest legacy
  interpretation instead: no POM/displacement, complex materials as
  plain env-mapped surfaces, True PBR through the vanilla lit path.

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
- **external/DirectXTex** - DDS decode + D3D11 SRV creation for
  `render/TextureRepository.cpp` (material textures on NIF models). ImageCore
  and the vendored `CommonUtil.h`/`AppLog.h` headers were carried over too
  but removed in a later dependency cleanup once FD2D stopped requiring
  them - see CMakeLists.txt's dependency notes.
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
- `render/D3D11Renderer.h/.cpp` / `render/TextureRepository.h/.cpp` (+ the
  per-viewport `render/TextureCache.h` memo) replace
  NifSkope's Qt/OpenGL `src/gl/renderer.cpp/.h` and
  `gltex.cpp`+`gltexloaders.cpp` with a D3D11 draw path (feeding off
  DirectXTex for DDS decode/SRV creation instead of NifSkope's own
  texture loaders).
- `ui/NifCompareView.h/.cpp` reimplements the layout of NifSkope's own
  `src/ui/nifdiffviewer.cpp`/`.h` (the Qt `File -> NIF Diff Viewer...`
  feature), documented in `D:\Works\nifskope\NIF_DIFF_VIEWER.md` - laid
  out FICture2-style (see the FICture2 section above).
- `schema_reference/nif.xml` (once added) is vendored from the upstream
  [niftools/nifxml](https://github.com/niftools/nifxml) project that
  NifSkope itself uses, kept here for reference only (not parsed at
  build/run time - see `schema_reference/NOTES.md`).
- `test_assets/*.nif` are small Skyrim SE/Fallout 4 NIF fixtures used to
  smoke-test the parser - kept local-only (gitignored), since game-derived
  meshes are not suitable for redistribution in a public repo.

NifSkope is distributed under a permissive 3-clause-BSD-style license that
requires retaining its copyright notice and disclaimer in redistributed
source/binaries derived from it. That notice is reproduced verbatim in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and must stay alongside
any ported `core/`/`render/`/`ui` code that traces back to NifSkope
sources - this is a separate, additional obligation on top of NIFDiff's
own MIT license, since the two pieces of code carry different licenses.

### ENB / Community Shaders / True PBR - conventions only

The extended-material features (parallax `_p.dds`, ENB/CS "complex
material" `_m.dds`, Community Shaders "True PBR" via PBRNifPatcher
markings) implement community **file-format conventions**, not ported
code:

- The `_m.dds` channel layout (R=reflection, G=glossiness, B=metalness,
  A=parallax height) and its "non-opaque coarsest mip" detection rule are
  published texture-format conventions of the ENB / Community Shaders
  ecosystem. ENB is closed-source; nothing of it is (or could be) included.
- True PBR support reads PBRNifPatcher's flag/slot conventions
  (SLSF2_Unused01 as the PBR marker, RMAOS packing) from the NIF; the
  PBR shading itself is textbook GGX/Cook-Torrance written for this
  viewer ("a credible preview, not CS-exact").
- The parallax-occlusion-mapping and height-field self-shadow functions in
  `render/shaders/Lit.hlsl` are an independent implementation of the
  standard public technique (Tatarchuk, "Practical Parallax Occlusion
  Mapping", GDC 2006). An earlier development revision ported Community
  Shaders' GPL-3.0 `ExtendedMaterials.hlsli` here; it was rewritten
  specifically to remove that GPL derivation, so the current tree contains
  no Community Shaders code.

## License

NIFDiff is MIT licensed - see [LICENSE](LICENSE). Redistributions must
also carry the third-party notices collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): the NifSkope BSD notice
(for the ported NIF/3D logic), the LZ4 BSD-2 notice, and the MIT/zlib
texts of FD2D, DirectXTex, spdlog, and zlib. That file also documents
which referenced projects contributed *conventions only* with no code
included (ENB, Community Shaders, PBRNifPatcher).

## Layout

```
app/               app shell: NIFDiffApp.h/.cpp bootstrap, main.cpp entry point,
                   AppSettings.h (INI persistence), AppIpc.h/.cpp (single-instance
                   forwarding), FileDialog.h/.cpp,
                   ValidateNif.cpp/ResourceResolveTest.cpp console smoke-tests
core/              NIF parsing (NifDocument) / scene building (SceneBuilder) / Camera /
                   ResourceResolver (override -> nif dir -> Game Data -> BSA/BA2 order)
render/            D3D11 renderer, HLSL shaders (shaders/, fxc-precompiled at build time),
                   TextureRepository (process-wide pooled DDS decode + complex-material
                   probes) with a per-viewport TextureCache memo on top
ui/                NifViewport (single 3D view), NifComparePane (view + Open/Close),
                   NifCompareSplitCoordinator (1-8 pane two-row equal-width grid),
                   NifCompareControlPanel (bottom strip), NifCompareView (top-level)
schema_reference/  nif.xml reference (vendored from niftools/nifxml, not parsed at runtime)
test_assets/       local-only smoke-test .nif fixtures (gitignored - game-
                   derived meshes are not redistributed with this repo)
third_party/       git submodules: FD2D, Floar, external/DirectXTex (DDS decode
                   + D3D11 SRV creation), external/spdlog
CMakeLists.txt     sets up third-party wiring + NIFDiff/NifValidate/ResourceResolveTest targets
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

## Status

Implemented: NIF parsing/scene-building/D3D11 rendering (a Qt-free
reimplementation of the relevant parts of
[NifSkope](https://github.com/niftools/nifskope) - see "Origins" above -
since extended with the material/shader coverage listed under Features),
and a dynamic **1-8 pane** compare UI laid out FICture2-style (bottom
control strip, two-row equal-width pane grid via
`NifCompareSplitCoordinator`, ported/adapted from FICture2's own
`ImageBrowserSplitCoordinator`) - not NifSkope's fixed
2-pane/right-sidebar diff layout. App shell (window bootstrap, INI session
persistence for up to 8 files, Win32 file dialogs, single-instance IPC)
hosts one `NifCompareView`.

Not yet ported/implemented: skinning/morphs/particles/controllers (out of
scope per `SceneBuilder.h`'s documented scope note - static bind-pose
comparison only), a `.ico`/app icon, and pane-splitter-ratio persistence
across add/remove (each add/remove currently resets to equal widths).

## Next steps

Possible follow-ups, not required for the current feature set:
1. Persist per-pane split ratios across add/remove (FICture2's
   `CaptureHorizontalSplitRatios`/`ApplyHorizontalSplitRatios` pattern).
2. File drag-and-drop onto a pane (FICture2's `ImageBrowserDragController`
   equivalent) as an alternative to the "Open..." button.
3. An app icon / `.ico` (none exists yet).
