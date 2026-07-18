# Changelog

Versions are fixed `MAJOR.MINOR.REVISION` values maintained in
`app/res/version.h.in`. Every released binary maps back to the commit carrying
the matching `v<version>` tag. See [RELEASING.md](RELEASING.md).

Entries are written for **users**, not for the git history: what changed in
the app, not which files moved.

## 1.1.161

### Textures
- Any pane can show a texture (`.dds`/`.png`/`.tga`/`.bmp`/...) in place of a
  NIF - the same pane, the same thumbnail strip, swapping content as you browse
- Smooth pointer zoom, rotation-aware clamped pan, 90°/180° rotation, fit to
  screen, and a switchable smooth/nearest sampling mode
- A live dimensions/format/zoom/rotation info bar, plus a full detail dialog
  (`I`) that adds mip level and source compression
- Browse a DDS's mip chain directly (`Ctrl+[` / `Ctrl+]`, or the context
  menu's Mip Level submenu), each level labelled with its resolution
- Checkerboard backdrop behind transparency, R/G/B/A channel isolation, and a
  per-pane Alpha Channel override (Auto / Transparency / Opaque) for the rare
  case the automatic policy gets it wrong
- Lossless alpha handling throughout - loose/uncompressed alpha is treated as
  transparency, block-compressed alpha as data, so a parallax normal map is
  no longer darkened by its own height channel
- Compressed DDS (BC1-BC7) uploads straight to the GPU in its native block
  format - 4K textures open effectively instantly, and a cache keyed on path
  and mip level makes revisiting one instant too
- A loading spinner and a "Failed to load image" state, so a bad file never
  leaves stale content on screen or pollutes Recent Files
- Screenshot (`F12` / context menu) now captures an image pane's actual
  on-screen view, not just NIF panes

### Resources
- Register Game Data folders for every supported game at once - **Detect**
  finds and registers Skyrim LE, Skyrim SE and Fallout 4 in one click,
  **Set Active** assigns a folder to whichever game the active pane's NIF
  belongs to
- Texture lookups are scoped to the NIF's own game, so two games' assets
  sharing a relative path can no longer cross-resolve to the wrong one
- A saved session's file living inside a BSA/BA2/archive now restores
  correctly on restart (previously only plain filesystem paths did)

### Fixes
- Texture Inspector previews render correctly again (a prior refactor let a
  NIF pane's own 3D render silently cover them)
- A custom shader placed in a subfolder can `#include "Common.hlsli"` and
  its own nested includes correctly, instead of silently falling back to the
  built-in shader

## 1.0.116

First public release.

### Compare
- 1-8 panes in an equal-width grid, added and removed at runtime
- Synced views / lighting / file selection across panes, each toggleable
- SYNCED / UNIQUE corner badges flag a pane whose file name is the odd one out
- Single-instance forwarding: opening a .nif whose name matches one already
  open lands in a new pane of the same window - so a mod manager's
  "open every conflicting file" fills one window with all of them
- Material diff panel (`I`): the selected sub-mesh's shader/texture-slot/
  material values tabled against the same-named mesh in every other pane,
  differences highlighted, loose-vs-BSA source markers included
- Texture inspector (`T`): per-slot format / size / mip count / resolved
  source with R/G/B/A channel isolation

### Rendering
- Full Skyrim/SE vanilla lit path (specular, glow, env maps, model-space
  normals, multilayer parallax, tint masks, effect shaders, decals, alpha)
- Parallax (`_p.dds`) with parallax occlusion mapping and self-shadowing
- Complex material (ENB / Community Shaders `_m.dds`) with a probe that
  rejects flat/binary alphas rather than running POM on a fake height field
- True PBR (Community Shaders / PBRNifPatcher) with GGX/Cook-Torrance,
  displacement POM and procedural IBL ambient specular
- Per-path toggles (Parallax / Complex Mat / True PBR) and per-channel
  toggles (Diffuse / Vertex Colors / Specular / Emissive / Lighting)
- 4x MSAA, vertex normal/tangent overlays, wireframe, grid/axes

### Navigation
- Orbit / pan / zoom-to-cursor plus Alt chords (Maya/Blender muscle memory)
- On-screen XYZ gizmo per pane; click a nub to animation-snap to that view
- Blender numpad views, orthographic mode, orbit-around-selection
- Close-up floors so pan/zoom never freeze up close; zoom dollies through
  the pivot into a model's interior

### Animation
- NIF-embedded transform animations play back: Play/Pause, sequence
  dropdown, scrubbable Time slider, Loop, Speed 0.25-2x
- Sync Anim keeps every pane phase-locked on one clock
- Idle CPU stays at zero when nothing is playing

### Resources
- Engine-order texture resolution: override folders -> the NIF's directory
  -> Game Data -> BSA/BA2, with Game Data auto-detection
- Unresolved diffuse renders magenta rather than passing as plain white
- Process-wide texture pooling; async parse/load/decode so panes appear
  immediately and models stream in during the archive scan

### Shell
- Per-pane folder thumbnail strips with real 3D thumbnails, keyboard
  navigation and type-to-select
- Drag & drop (left 75% replaces, right 25% inserts a new pane)
- `.nif` file association, session persistence in
  `%LOCALAPPDATA%\NIFDiff\NIFDiff.ini`, window placement clamped to a
  monitor's work area
- Collapsible, responsive bottom control strip
- User-editable HLSL shaders with 1s hot reload, a content-hash bytecode
  cache, and `shaders.ini` binding rules for routing meshes to custom shaders
