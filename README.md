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
  pane's own buttons or the right-click context menu. Adding or removing a
  pane resets every pane to an equal width (so opening several same-named
  NIFs to compare them lines them up evenly); a dragged splitter layout is
  still saved with the session and re-applied on the next launch (FICture2's
  capture/apply-split-ratios pattern).
- **Synced cameras and lighting** across panes (each sync individually
  toggleable), so one orbit/pan/zoom or light change hits every model at
  once.
- **Synced file selection** ("Sync Files"): picking a .nif from one pane's
  thumbnail strip also loads the same file name from every other pane's own
  folder, so you can step through a mesh set and see each variant (e.g. a
  parallax mod vs. vanilla) side by side. Panes whose folder lacks that name
  are left unchanged.
- **Click-to-select**: a left click picks the sub-mesh under the cursor,
  overlays it in wireframe, and reports its shader classification; each
  pane also shows a shader-kind summary (e.g. `Default x8 · Parallax x3`)
  and total triangle count in its status line. A double-click selects
  AND frames the mesh under the cursor (empty space re-frames the whole
  scene), and the mouse wheel zooms toward the cursor - together with
  clip planes that adapt to the scene's size, this keeps tiny
  silverware and huge exteriors equally navigable.
- Orientation presets, Center/Reset view, wireframe/grid/axes toggles
  (plus a "Hidden" toggle for NiAVObject-hidden marker subtrees),
  brightness/ambient sliders, light declination/planar angle, and a
  frontal-light mode.
- **4x MSAA** (toggle, on by default): each pane renders into a multisampled
  offscreen target and resolves to the composited image, antialiasing model
  silhouettes, the grid/axes, and the wireframe/normal overlays (clamped to
  what the GPU supports; NifSkope's DoMultisampling equivalent). Toggle with
  the DISPLAY "MSAA 4x" checkbox or the `M` key.
- **Vertex normal/tangent overlays** (NifSkope's gltools line-drawing
  equivalent): cyan normal / magenta tangent line segments per vertex,
  for debugging normal-map/tangent-space issues. Drawn for the selected
  sub-mesh when one is picked, every mesh otherwise; segment length
  scales with each mesh's own bounds.
- **Active pane** (FICture2's focused-browser equivalent): any click in
  a pane makes it active, drawn with an accent border while several
  panes are open; pane-context shortcuts target it.
- **Synced/unique pane badges**: each pane shows a small corner badge -
  teal "SYNCED" when another pane holds a file of the same name (the
  compare-the-same-mesh-across-mods group), amber "UNIQUE" when its file
  name is one of a kind - so an odd-one-out pane (e.g. a mod missing that
  mesh, so a different file stayed loaded) is obvious at a glance. The panes
  that share the ACTIVE pane's file name also get a green inner-glow frame
  (fading inward from the edges; the active pane gets a stronger blue glow),
  so the group being compared against the focused pane stands out. The glow is
  chrome only, so every pane's 3D background stays identical and the model
  comparison isn't biased.
- **Keyboard shortcuts**: `F` reset all views, `G` grid, `X` axes,
  `W` wireframe, `H` hidden, `N` normals, `Shift+N` tangents, `M` MSAA,
  `PgUp`/`PgDn` cycle the camera preset;
  pane-context keys acting on the active pane: `1`-`8` / `Tab` /
  `Shift+Tab` select it, `R` resets its camera, `C` focuses its
  selected sub-mesh, `Ctrl+O` opens a file into it, `Ctrl+Shift+O` opens one
  into a new pane, `Ctrl+W` (or `Ctrl+F4`) closes it,
  `Del` clears its document, `Ctrl+E` shows its file in Explorer,
  `F12` saves its screenshot; thumbnail-strip navigation (FICture2's browser
  keys, and Sync Files mirrors each pick into the other panes): `←`/`→`
  (or `,`/`.`) step to the previous/next sibling .nif, `Home`/`End` jump to
  the first/last, `Backspace` (or `Ctrl+Up`) browses to the parent folder;
  `I` toggles the material diff panel and `T` the texture inspector.
- **Per-pane context menu**: open/close the pane, reopen a **recent file**
  into it (an MRU submenu of the last dozen opened .nifs, most-recent
  first, with a "Clear Recent Files" entry), open the containing folder in
  Explorer with the file selected, and save the pane's 3D render as a PNG
  (defaults next to the .nif, auto-numbered).
- **Path tooltips + copy**: every control that shows a path (a pane's
  .nif path strip, the Game Data label) reveals the full path in a hover
  tooltip when the strip is too narrow to show it in full, and copies the
  path to the clipboard on right-click with a brief confirmation banner.
- **Per-pane folder thumbnail strips** (FICture2's ThumbnailPane equivalent):
  every pane hosts its own scrollable strip along its bottom edge that lists
  the folder of THAT pane's currently-open .nif - so when comparing files from
  different folders, each pane browses its own. Sibling .nif files appear as
  3D thumbnails (each parsed + scene-built on the app-wide background load pool
  - a shared `ResourceManager`, one bounded thread pool for every strip - then
  rendered headlessly through the shared render core on the UI thread a few per
  frame, so even a large folder never blocks or stutters the UI). Parsing goes
  through a shared NIF cache with in-flight de-duplication, so a file is parsed
  exactly once no matter how many panes or strips want it: opening the same mesh
  into several panes, or a pane's own file showing up in its thumbnail strip,
  reuses one parsed document instead of re-reading and re-parsing it. Background
  reads pass through an I/O gate that bounds how many run at once, so a folder
  full of thumbnails can't thrash the disk; an interactive file-open is never
  throttled behind them. Each thumbnail is framed from a slight 3/4 angle (a small
  yaw off dead-on frontal) with an orthographic camera fitted tightly to the
  model's non-hidden bounds with equal margins, so cards take the model's own aspect
  ratio - wide meshes get wide cards, tall meshes tall ones - all sized to the
  strip height.
  subfolders and an ".." tile appear as folder icons, and the pane's current
  file is drawn with a blue highlight. Clicking a sibling loads it into that pane (the highlight
  follows); clicking a folder or ".." navigates that pane's strip in place.
  Each card's file name is centered and ellipsized under its thumbnail, and
  hovering shows the full path as a tooltip. The VIEW group's "Thumbnails"
  checkbox (mirrored by a "Hide/Show Thumbnail Strips" context-menu item)
  turns every pane's strip off at once - they collapse and their loaders idle
  so no thumbnails are generated. Card size is adjustable - drag the strip's
  top edge to resize it live (all panes' strips follow), or pick a
  Small/Medium/Large preset from the "Thumbnail Size" context-menu submenu.
  The on/off state and the size are remembered across sessions.
- **Drag & drop from Explorer** (FICture2's drag-controller semantics):
  hovering the left 75% of a pane shows a red overlay and the drop
  REPLACES that pane's document; the right 25% shows a green strip and
  the drop INSERTS a new pane right after it. Multi-file drops place
  the first file per the drop point and the rest into empty/new panes.

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
  second). Loading a model no longer blocks on its textures: the DDS
  archive extraction, DDS decode and GPU upload for every texture a scene
  references run on the shared load pool (IoGate-bounded so they don't thrash
  the disk) and each texture pops into the model as it finishes, so opening a
  heavy PBR exterior leaves the UI responsive instead of freezing on the read.
  Only the cheap "which file/archive entry is this" lookup stays on the UI
  thread; the expensive BSA byte-extraction happens on a worker.
- Opening a file is asynchronous end to end: the NIF parse and scene build
  run on the load pool too, so a pane switches to a placeholder (its grid with
  a "Loading…" overlay) immediately and the model appears when it's ready, and
  the UI thread only does the final hand-off. At launch the window opens with
  every pane already present and labelled with its file name (the last
  session's pane count, restored first) while the models load in behind them.
  Model parsing needs no archive scan, so the models parse and appear DURING
  the several-second BSA/BA2 scan (plainly shaded until their textures
  resolve) rather than only after it; once the scan lands, each loaded pane's
  archive-backed textures are re-resolved and pop in.
  Every pane's model NIF is loaded ahead of the (lower-priority) folder
  thumbnails - the shared queue always drains the main models first - so
  opening several panes fills them all with models before the thumbnail strips
  populate. A burst of opens
  (session restore, multi-file drag&drop, single-instance forwarding) fills
  every pane at once and each model streams in independently; re-pointing or
  closing a pane mid-load drops the superseded load instead of letting a
  stale result land in the wrong pane.
- Session persistence (open files, splitter ratios, recent-files list,
  Game Data path, override folders),
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
- **Render-channel toggles** (CHANNELS group): switch Diffuse (white),
  Vertex Colors, Specular (legacy AND PBR GGX), Emissive, or Lighting
  (raw unlit texture) off one at a time to isolate which shading input
  makes two panes differ.
- **Material diff panel** (`I` to toggle): while a sub-mesh is selected,
  an overlay table shows its BSLightingShaderProperty data - shader
  type/flags, texture slots (with loose/bsa resolve markers, so two
  panes resolving the same path to different sources light up),
  specular/emissive/alpha/UV values, alpha and depth state - side by
  side with the same-named mesh in every other loaded pane,
  highlighting exactly which values differ. Hover a texture cell for a
  tooltip with its full relative path and resolved source (loose
  absolute path or `archive -> entry`); **right-click** a texture cell
  to copy that path to the clipboard. Click the header row to
  **collapse/expand** the panel (the `▾`/`▸` chevron), and drag the
  bottom-left grip to widen or narrow the value columns.
- **Texture inspector** (`T` to toggle): lists the selected sub-mesh's
  bound texture slots with resolution, pixel format, mip count and the
  resolved source (loose file path vs BSA archive - mod-conflict
  diagnosis), and previews the clicked slot in 2D with R/G/B/A channel
  isolation (click the preview to cycle) - reading `_rmaos`/`_m`
  channel content without leaving the viewer.

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
- `render/RenderDevice.h/.cpp` (the single app-wide render core: shaders,
  state objects, IBL, fallback textures) with per-view `render/RenderTarget`
  framebuffers and `render/RenderMeshCache` geometry caches, plus
  `render/TextureRepository.h/.cpp` (+ the per-viewport `render/TextureCache.h`
  memo) replace NifSkope's Qt/OpenGL `src/gl/renderer.cpp/.h` and
  `gltex.cpp`+`gltexloaders.cpp` with a D3D11 draw path (feeding off
  DirectXTex for DDS decode/SRV creation instead of NifSkope's own
  texture loaders). Splitting the old per-viewport `D3D11Renderer` into a
  shared device + lightweight targets means the shaders/IBL are built once
  instead of once per pane, and any scene can be rendered into an arbitrary
  offscreen target.
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
comparison only) and a `.ico`/app icon.

## Next steps

Possible follow-ups, not required for the current feature set:
1. An app icon / `.ico` (none exists yet).
