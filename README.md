# NIFDiff

A standalone, Qt-free tool for viewing and diffing NIF files (Bethesda's
NetImmerse/Gamebryo model format - Skyrim, Skyrim SE, Fallout 4, etc.)
side by side in 3D.

**[Demo video](https://youtu.be/Ho_BjrLK8lI)** - a walkthrough of the compare
panes, navigation, animation playback, custom shaders, and the
Vortex-conflict / Explorer open flows. The Nexus Mods description text lives in
[NexusMods_Mod_Description.bbcode](NexusMods_Mod_Description.bbcode).

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

## Using NIFDiff

NIFDiff opens one to eight NIF models side by side and keeps their cameras,
lighting, and file selection in sync, so you can see exactly how two versions
of a mesh differ - a modded parallax/PBR mesh vs. vanilla, two mods' takes on
the same asset, or a before/after of your own edit. This section is a
task-oriented walkthrough; the [Feature reference](#feature-reference) below
documents every control in detail.

### 1. Open some models

- **Launch** `NIFDiff.exe`. It restores your last session (pane count, files,
  splitter layout) if you have one, otherwise it opens with two empty panes.
- **Register Game Data roots** via the RESOURCES group. **Detect** registers all
  installed Skyrim LE, Skyrim SE, and Fallout 4 `Data` folders; **Set Active**
  assigns a folder to the game identified by the active NIF. Texture lookup is
  scoped to that game, so identical paths in different games cannot collide.
  Add **override folders** there for loose texture mods that should win over
  Game Data.
- **Load a file** into a pane: drag a `.nif` from Explorer onto it, use the
  pane's Open button / right-click → Open, or `Ctrl+O`. Dropping onto the left
  ~75% of a pane *replaces* it; the right ~25% *inserts* a new pane. A pane
  takes **textures** (`.dds`/`.png`/…) just as readily as `.nif`s - it becomes a
  2D image view in place (see [Textures view/compare](#feature-reference)).
- **Open a folder or archive** the same way - as a command-line argument, a drag
  & drop, or **right-click → "Open Folder in This Pane…"** (a folder path can't
  come through the file-open dialog, so it has its own picker). Opening a
  container lists it in the strip and picks a sensible default: if it holds a
  viewable file (a NIF or texture) the **first one loads** so the pane shows real
  content straight away; if it holds none, the pane names the folder over the
  empty grid and the strip highlights the **first subfolder** (or the ".." tile)
  so you can step further in. Folders, `.bsa`/`.ba2`/`.zip`/`.7z`/`.rar`
  archives, NIFs and textures are all accepted at every one of these entry
  points - and each is remembered: it lands in the **Open Recent** (MRU) list and
  is restored on the next launch.
- **Compare the same mesh across mods**: opening a `.nif` whose *name* matches
  one already open lands in a new pane of the same window (single-instance
  forwarding) - the core "compare two mods' version of one mesh" flow. Dropping
  several same-named `.nif`s at once lines them up in equal-width panes.

### 2. Read the screen

- **Panes** fill the window in an equal-width grid (1-4 in a row, 5-8 in two
  rows). Each pane shows its file path on top, the 3D view, a status line
  (shader kinds + triangle counts), and its own **thumbnail strip** browsing
  that file's folder along the bottom.
- The **bottom control strip** is grouped left to right: **PANES** (add / reset
  view), **VIEW** (orientation preset + the Sync toggles), **NAVIGATION**
  (camera feel), **DISPLAY** (grid / axes / wireframe / normals / … overlays),
  **LIGHTING**, **MATERIALS** and **CHANNELS** (shader toggles), and
  **RESOURCES** (Game Data + override folders). The strip is **responsive**:
  as the window narrows the groups wrap onto additional rows, the multi-column
  groups (NAVIGATION, LIGHTING, ...) collapse to a single column, and a
  vertical scrollbar appears only as a last resort - every control stays
  reachable at any window size.
- The **active pane** (the last one you clicked) carries an accent border;
  pane-specific keys and the right-click context menu act on it. The
  **window title** follows it too - it shows the active pane's file name
  ahead of the app name, so a taskbar full of NIFDiff windows (or a bug-report
  screenshot) is still identifiable at a glance.

### 3. Navigate a model

- **Orbit** = left-drag, **pan** = middle/right-drag, **zoom** = mouse wheel
  (toward the cursor). The same gestures are also on **Alt chords** (Alt+left /
  Alt+middle / Alt+right-drag) for Maya/Blender muscle memory.
- **Jump to a view**: pick Front/Back/… from the VIEW dropdown, click an axis
  nub on the top-left **XYZ gizmo**, or use the numpad (`1/3/7` = Front/Right/
  Top, add `Ctrl` for the opposite face). Snaps animate smoothly.
- **Frame** what matters: double-click a sub-mesh to select *and* frame it,
  `Numpad .` frames the current selection, `Numpad 0` frames the whole scene,
  and `F` (Reset View) returns to the default framing.
- **Tune the feel** in the NAVIGATION group: Move / Zoom / Rotate sensitivity,
  an FOV slider, an **Orthographic** toggle (ideal for lining up two
  silhouettes), and **Orbit Sel** / **Zoom Cursor** switches. Close-up stays
  usable - pan and zoom don't freeze as you approach - and zoom can dolly
  *through* the pivot into a model's interior.
- With **Sync Views** on (the default) every one of these moves mirrors to all
  panes at once.

### 4. Compare

- Keep **Sync Views / Sync Lighting / Sync Files** on to move, light, and step
  through files in lockstep; turn one off to hold the other panes still for an
  isolated before/after.
- **Corner badges** flag the odd one out - "SYNCED" (another pane holds the
  same-named file) vs "UNIQUE" - and the panes sharing the active pane's file
  name get a green glow, so a mod that's missing a mesh stands out at a glance.
- **Select a sub-mesh** (left click) to compare it directly: the **Material
  diff panel** (`I`) tables its shader / texture-slot / material values against
  the same-named mesh in every other pane and highlights what differs (with
  loose-vs-BSA source markers); the **Texture inspector** (`T`) shows each bound
  texture's format / size / source and previews its channels.
- **Isolate a difference** with the CHANNELS toggles (Diffuse / Vertex Colors /
  Specular / Emissive / Lighting) - switch one input off at a time to find which
  one makes two panes look different - and the MATERIALS toggles (Parallax /
  Complex Mat / True PBR) to view a mesh with vs. without each extended path.

### 5. Typical workflows

- **"Does this mod's mesh actually change anything vs vanilla?"** Open both
  (same name → same window), Sync Views on, orbit around; select a shared
  sub-mesh and open the Material diff panel to see texture-slot / flag / value
  differences, including when the two resolve the same path to different sources
  (loose vs BSA).
- **"Why does one look darker / shinier?"** Use the CHANNELS toggles to isolate
  the channel, the Texture inspector to check the actual `_m` / `_rmaos` data,
  and the MATERIALS toggles to confirm which shading path each mesh is taking.
- **"Step through a whole set."** Sync Files on, then `←`/`→` (or clicking the
  thumbnail strip) walks every pane through its folder's siblings together.

### Keyboard reference

| Keys | Action |
|---|---|
| Left / Middle-Right drag / Wheel | Orbit / Pan / Zoom-to-cursor |
| `Alt`+Left / `Alt`+Middle / `Alt`+Right drag | Orbit / Pan / Dolly (DCC chords) |
| `PgUp` / `PgDn` | Cycle orientation preset; page thumbnail tiles while the strip is focused |
| `Numpad 1/3/7` (`Ctrl+` = opposite face) | Front / Right / Top (Back / Left / Bottom) |
| `Numpad 5` | Orthographic ↔ perspective |
| `Numpad .` / `Numpad 0` | Frame selection / whole scene |
| `F` / `R` | Reset all views / active pane's view |
| `C` | Focus the active pane's selected sub-mesh |
| `G` `X` `W` `H` `N` `Shift+N` `M` | Grid / Axes / Wireframe / Hidden / Normals / Tangents / MSAA |
| `I` / `T` | Material diff panel / Texture inspector |
| `1`-`8` / `Tab` / `Shift+Tab` | Select the active pane |
| `Ctrl+O` / `Ctrl+Shift+O` | Open a file into the active pane / a new pane |
| `Ctrl+W` (`Ctrl+F4`) / `Del` | Close the active pane / clear its document |
| `←`/`→` (`,`/`.`), `Home`/`End`, `PgUp`/`PgDn` | Thumbnail strip: previous/next tile, first/last tile, previous/next visible page |
| `Enter` / `Backspace` (`Ctrl+Up`) | Enter the selected folder / jump to the parent folder |
| Type a name (strip focused) | Type-to-select: jump to the matching tile; repeat the key to cycle |
| `Ctrl+E` / `F12` | Show active pane's file in Explorer / save its screenshot |
| Image pane: wheel / drag / `F` | Smooth pointer zoom / clamped pan / fit to screen |
| Image pane: `[` / `]` / `\`, `Ctrl+[` / `Ctrl+]`, `Alt+Q`, `I` | Rotate left/right/180°, previous/next DDS mip, sampling quality, image information |

> Numpad view keys need NumLock on. Thumbnail-strip navigation acts on the
> active pane and, with Sync Files on, mirrors each pick into the others.

## Feature reference

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
- **Navigation** (Maya/Blender-style, dual scheme): left-drag orbits,
  middle/right-drag pans, wheel zooms toward the cursor; the same actions
  are also on the DCC-standard **Alt chords** - `Alt`+left-drag orbit,
  `Alt`+middle-drag pan, `Alt`+right-drag dolly-zoom - so users of any 3D
  package feel at home. When a sub-mesh is selected the orbit **pivots on
  it** (a rigid rotation of the whole rig, so the selection stays put on
  screen while the rest revolves around it) rather than the scene center.
  Preset snaps (the orientation dropdown / `PgUp`/`PgDn`) and Center/Focus
  **animate** smoothly (~180ms eased tween) instead of jumping, and the
  transition mirrors across Sync-View panes in lockstep.
- **On-screen axis gizmo** (Blender-style, per pane): a small XYZ widget in
  each pane's top-left corner projects the six world axes (X red, Y green,
  Z blue; positive axes lettered and lined, negatives as hollow rings) through
  the live camera, so the current orbit orientation is readable at a glance -
  axes pointing toward you are opaque, those pointing away fade back. **Click
  an axis nub** to animation-snap to that view (e.g. the `Y` nub → top-down),
  matching the orientation presets; the snap mirrors across Sync-View panes and
  never selects the mesh behind the widget.
- **NAVIGATION control group**: tune the camera feel from the bottom strip -
  **Move / Zoom / Rotate** sensitivity multipliers, an **FOV** slider (the
  perspective field of view, kept in lock-step with the pick ray so clicks stay
  accurate), an **Orthographic** checkbox (mirrors the `Numpad 5` toggle both
  ways), and **Orbit Sel** / **Zoom Cursor** switches for the orbit-around-
  selection and zoom-toward-cursor behaviors. Every setting applies to all
  panes and is inherited by new ones. The whole bottom strip can be
  **collapsed/expanded** with the small chevron tab centered on its top edge -
  collapsed gives the 3D views the full window height, and the state is
  remembered across sessions. Panning **and zooming** also have a
  **close-up floor**: their step scales with the eye-to-target distance, so at
  extreme close-ups that step would collapse to nothing (freezing the pan and
  making it take dozens of scroll notches to back out) - both are floored at a
  fraction of the focus radius (the selected sub-mesh's, or the whole scene's)
  so close-up navigation stays usable. Select the sub-mesh you're inspecting to
  scale the floor to it. Zooming can also **dolly through the pivot**: once the
  eye reaches the target it keeps flying forward (past the origin / axis gizmo,
  e.g. into a model's interior) instead of stopping, bounded to a few scene
  radii so it never runs away - Reset View reframes if you overshoot.
- **Animation playback** (ANIMATION group): NIF-embedded transform animations -
  the animated statics of the game world (water wheels, windmills, traps,
  animated doors) that carry `NiControllerManager` sequences or standalone
  `NiTransformController`s - play back in the viewer: Play/Pause, a sequence
  dropdown ("Idle", "Open", ...), a scrubbable Time slider, Loop and Speed
  (0.25-2x). **Sync Anim** (default on) keeps every pane on the same clock, so
  two mods' versions of the same animated mesh run side by side in phase.
  Playback rides the same on-demand ~60fps timer as the camera tweens - idle
  CPU stays at zero when nothing plays. Rigid node animations only: actor
  skeletal animations live in external Havok `.hkx` behavior graphs (not in
  the NIF; NifSkope cannot play them either), and skinned meshes hold their
  bind pose.
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
- **Orthographic projection** (`Numpad 5`): drops perspective
  foreshortening so two meshes' silhouettes line up 1:1 for a compare
  (parallel edges stay parallel; the ortho view height matches the
  perspective frustum at the target, so toggling never resizes the model).
  Applied to every pane so the comparison stays on equal footing.
- **Keyboard shortcuts**: `F` reset all views, `G` grid, `X` axes,
  `W` wireframe, `H` hidden, `N` normals, `Shift+N` tangents, `M` MSAA,
  `PgUp`/`PgDn` cycle the camera preset (or page the thumbnail strip while it
  holds focus). Blender-style numpad views
  (NumLock on): `Numpad 1/3/7` = Front/Right/Top, `Ctrl+` the opposite
  face, `Numpad 5` toggles ortho/perspective, `Numpad .` frames the
  selection and `Numpad 0` frames the whole scene;
  pane-context keys acting on the active pane: `1`-`8` / `Tab` /
  `Shift+Tab` select it, `R` resets its camera, `C` focuses its
  selected sub-mesh, `Ctrl+O` opens a file into it, `Ctrl+Shift+O` opens one
  into a new pane, `Ctrl+W` (or `Ctrl+F4`) closes it,
  `Del` clears its document, `Ctrl+E` shows its file in Explorer,
  `F12` saves its screenshot; thumbnail-strip navigation (FICture2's browser
  keys, and Sync Files mirrors each file pick into the other panes): `←`/`→`
  (or `,`/`.`) step the selection through every tile, `Home`/`End` jump to the
  first/last, and strip-focused `PgUp`/`PgDn` move by the number of variable-width
  cards that fit in one viewport - landing on a sibling .nif loads it, while a
  subfolder or the ".." tile is only **selected** (highlighted) until `Enter`
  navigates into it
  (`Backspace` / `Ctrl+Up` still jump straight to the parent). While the strip
  holds keyboard focus (after a click or any of those keys), **type-to-select**
  is active: typing a name prefix jumps the selection to the next matching tile
  (files and folders), and repeating the key cycles through the matches - the
  letter shortcuts below resume once you click back into the 3D view.
  `I` toggles the material diff panel and `T` the texture inspector. In the
  inspector, click a row to select it, click the preview to cycle channels,
  and double-click a row to open the resolved loose/archive texture in an
  empty or newly added ImagePane.
- **Per-pane context menu**: open/close the pane, reopen a **recent file**
  into it (an MRU submenu of the last dozen opened .nifs, most-recent
  first, with a "Clear Recent Files" entry), open the containing folder in
  Explorer with the file selected, and save the pane's rendered content as a
  PNG (`F12`; defaults next to the file, auto-numbered) - a NIF pane's clean
  3D render, or an image pane's actual on-screen presentation (zoom, pan,
  rotation, channel isolation, checkerboard - whatever is currently shown),
  read back from the app's own composited D3D+D2D frame rather than a desktop
  screen-grab so an occluding window can never leak into the saved image. An
  image pane's context menu additionally has an **Image View** submenu (Fit to
  Screen, Rotate 90°/Left/Right/180°, Reset Rotation), a **Sampling**
  smooth/nearest toggle, an **Image Information** item, and - for a DDS with
  more than one mip - a **Mip Level** submenu.
- **Path tooltips + copy**: every control that shows paths (a pane's
  .nif path strip, the Game Data summary) reveals full details in a hover
  tooltip and copies them to the clipboard on right-click with a brief
  confirmation banner.
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
  strip height. Sibling texture files get the same treatment: a real decoded
  preview of the image, not a placeholder glyph.
  subfolders and an ".." tile appear as folder icons, and the pane's current
  file is drawn with a blue highlight. Clicking a sibling loads it into that pane (the highlight
  follows); clicking a folder or ".." navigates that pane's strip in place.
  **Archives are browsable too** (BSA, BA2, ZIP, 7z, RAR - every format Floar
  supports): the strip lists through Floar's virtual filesystem, so an archive
  sitting in the folder appears as a folder tile you can step into, its
  subfolders and `.nif` members list just like loose files, and clicking one
  thumbnails and opens it straight out of the archive (extracted to memory - no
  unpacking). ".." walks back out of the archive. You can also open an archive
  directly from **"Open .nif in This Pane"** - the dialog's file-type dropdown
  has an **Archives** filter, and picking one drops the pane's strip inside it to
  browse for a mesh.
  Each card's file name is centered and ellipsized under its thumbnail, and
  hovering shows the full path as a tooltip. The VIEW group's "Thumbnails"
  checkbox (mirrored by a "Hide/Show Thumbnail Strips" context-menu item)
  turns every pane's strip off at once - they collapse and their loaders idle
  so no thumbnails are generated. Card size is adjustable - drag the strip's
  top edge to resize it live (all panes' strips follow), or pick a
  Small/Medium/Large preset from the "Thumbnail Size" context-menu submenu.
  When the cards overflow, a draggable horizontal scrollbar appears below
  their labels; clicking its track moves by a viewport. The on/off state and
  the size are remembered across sessions.
- **Drag & drop from Explorer** (FICture2's drag-controller semantics):
  hovering the left 75% of a pane shows a red overlay and the drop
  REPLACES that pane's document; the right 25% shows a green strip and
  the drop INSERTS a new pane right after it. Multi-file drops place
  the first file per the drop point and the rest into empty/new panes.
- **Textures view/compare in the same panes** (FICture2's original texture
  viewer, folded back in): a pane is a persistent frame - its thumbnail strip
  along the bottom, a **swappable content area** filling the rest - and the
  content follows whatever file you open. Open a `.nif` and the content is the
  3D viewport; open a texture (`.dds`, `.png`, `.tga`, `.bmp`, and the rest of
  the formats ImageCore decodes) and the *same pane* becomes a 2D image view -
  no separate window, no new pane. Every entry point treats textures, NIFs,
  folders and archives at the same grade: command-line arguments, drag & drop,
  **"Open in This Pane"**, and the thumbnail strip all accept any of them.
  Because the strip is never torn down, picking a texture while browsing a NIF's
  folder (or a `.nif` while browsing textures) **swaps the content in place** and
  browsing continues - the strip's folder, scroll and selection survive the
  swap. Image content supports **smooth pointer zoom**, rotation-aware
  **clamped pan**, 90°/180° rotation, fit/reset commands, switchable
  smooth/nearest sampling, and a live dimensions/format/zoom information bar
  (`I` opens the same details, plus mip level and source compression, in a
  dialog). A DDS with a mip chain can be **browsed mip by mip**
  (`Ctrl+[`/`Ctrl+]`, or the context menu's **Mip Level** submenu with each
  entry labelled by that level's resolution) - useful for judging how a
  texture holds up at a size smaller than its authored resolution. A **Loading**
  spinner covers the pane while a texture decodes, and a red **"Failed to load
  image"** overlay (with the path label itself turned red) marks a decode
  failure without leaving stale content on screen - a failed path is also kept
  out of Recent Files and the saved session. It also provides a **checkerboard**
  backdrop behind transparent areas, and **channel isolation** (view R, G, B, or
  A alone, or the full RGBA) - and with Sync Views on, pan/zoom/channel changes
  mirror across
  panes just like the 3D cameras do, so you can line a diffuse up against its
  normal map or two mods' versions of one texture. Compressed DDS (BC1-BC7) is
  uploaded straight to the GPU as its native block format rather than decoded to
  RGBA on the CPU, so 4K textures open effectively instantly. Once uploaded, a
  texture's GPU view is kept in a small **path+mip->SRV cache** (device-generation
  aware, LRU/VRAM-budget capped), so stepping back to a recently-viewed texture -
  or mip level - in the strip re-shows it synchronously - no decode round-trip,
  no re-upload. (Textures referenced *by a NIF* have their own richer equivalent:
  a process-wide `TextureRepository` keyed on the engine-resolved source, so
  panes comparing the same-named mesh share one upload of each shared texture.)
- **Alpha, done right for game textures.** An alpha channel might be *coverage*
  (transparency) or packed *data* (a parallax height, a specular mask) - and DDS
  metadata often doesn't say. NIFDiff decodes losslessly (never premultiplying
  away the original channels) and applies an **Auto** policy: loose PNG/TGA and
  uncompressed alpha are treated as transparency, while a block-compressed
  straight/unknown alpha (usually data on Bethesda meshes) is shown opaque - so a
  parallax normal map displays at full brightness instead of being darkened by
  its height alpha. For the rare misjudgment, the pane's context menu has an
  **Alpha Channel** override (Auto / Treat as Transparency / Treat as Opaque),
  reset to Auto per image. Channel isolation always reads the true straight value
  regardless.

### Bethesda resource pipeline

- Loads Skyrim LE/SE and Fallout 4 NIFs for static bind-pose comparison
  (no skinning/morphs/particles/controllers - see `SceneBuilder.h`'s
  scope note).
- Textures resolve in engine order: override folders -> the NIF's own
  directory -> the NIF's **derived Data root** -> matching game's loose Data
  files -> matching game's BSA/BA2 archives (via Floar). Skyrim LE, Skyrim SE,
  and Fallout 4 roots are registered independently. A NIF inside a registered
  Data root inherits that root's game; loose sources fall back to BS Version
  (including the common BS83-in-Skyrim-SE compatibility case). The derived
  Data root handles Bethesda's
  Data-rooted convention: a NIF at `<mod>\meshes\...` references its textures
  as `textures\...` relative to `<mod>`, so opening a loose mod folder (e.g. a
  Vortex/MO2 staging folder) shows its own textures even when the mod is not
  deployed to the game's `Data` directory. A named-but-unresolved diffuse
  renders magenta instead of blending in as a plain white surface.
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
- Session persistence (open files - including a pane that was showing a file
  or implicit folder *inside* a BSA/BA2/ZIP/7z/RAR archive, restored through
  the same Floar virtual-filesystem lookup the browsing strip uses rather than
  a plain on-disk existence check, so archive members survive a restart too -
  splitter ratios, recent-files list, per-game Data paths, override folders, and
  the window size/position - restored
  clamped to a monitor's work area so it never comes back off-screen after a
  display change), stored per-user in `%LOCALAPPDATA%\NIFDiff\NIFDiff.ini`
  (the Windows-recommended location, so it works from a read-only install
  directory too; a legacy INI next to the exe is migrated over once),
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
- **User-editable shaders**: the HLSL sources ship next to the exe in
  `shaders\` (`Lit.hlsl` - the uber-shader covering every material path -,
  `Unlit.hlsl`, `Highlight.hlsl`, and `Common.hlsli` - the shared contract:
  the constant buffers, texture registers and vertex/interpolant structs a
  custom mesh shader `#include`s to interoperate with the renderer). Edit
  one and save: the app **hot-reloads**
  it within a second, no restart. A compile error keeps the previous shaders,
  falls back to the built-in (embedded) copies and shows a toast with the log
  pointer - a broken edit can never blank the viewer. Compiled bytecode is
  cached per content hash under `%LOCALAPPDATA%\NIFDiff\shadercache\`, so only
  the first launch after an edit pays the compile; untouched installs skip the
  whole machinery (embedded bytecode, zero startup cost). Overrides are also
  picked up from `%LOCALAPPDATA%\NIFDiff\shaders\` when the install dir is
  read-only.
- **User shader binding** (`shaders\shaders.ini`): declare your own shader
  programs and route the renderer's mesh draws to them per material kind -
  `[Programs] MyToon=custom\MyToon.hlsl` plus first-match `[Bind.Mesh]` rules
  with selectors `pbr` / `complexmat` / `parallax` / `envmap` / `effect` /
  `decal` / `node:<substring>` / `tex:<substring>` / `*`. Custom programs
  `#include "Common.hlsli"` and provide VSMain/PSMain. Includes are searched
  beside the custom source and then at the `shaders\` root, so programs in
  subfolders need no copied contract file. A rule naming a broken or missing
  program falls back to the built-in Lit. The manifest and every
  program source hot-reload live, so e.g. `Rule1=pbr -> MyToon` restyles only
  the True PBR meshes of a compare - thumbnails included - while everything
  else keeps the stock shading. Delete the file (or leave it commented, the
  shipped default) for stock behavior.
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
- `core/AnimController.h/.cpp` is a Qt-free port of the animation-playback
  math in NifSkope's `src/gl/glcontroller.cpp` (Controller::ctrlTime's
  extrapolation, the cached key search, the per-KeyType interpolation and
  quaternion slerp) and of `src/gl/controllers.cpp`'s
  `ControllerManager::setSequence` binding logic (AnimPlayer::bind).
- `nif.xml` from [niftools/nifxml](https://github.com/niftools/nifxml) is
  **referenced as documentation only and not vendored** - it is GPL-3.0,
  so it is consulted upstream instead of being redistributed in this MIT
  repo (see `schema_reference/NOTES.md` for the pinned version the parser
  comments cite).
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
                   IniStore.h (INI persistence), AppIpc.h/.cpp (single-instance
                   forwarding), FileDialog.h/.cpp,
                   ValidateNif.cpp/ResourceResolveTest.cpp console smoke-tests,
                   res/ (icon, VERSIONINFO .rc, version.h.in MAJOR/MINOR source)
cmake/             project-specific CMake helpers
core/              NIF parsing (NifDocument) / scene building (SceneBuilder) / Camera /
                   ResourceResolver (override -> nif dir -> derived Data root -> Game Data -> BSA/BA2 order)
render/            D3D11 renderer, HLSL shaders (shaders/, fxc-precompiled at build time),
                   TextureRepository (process-wide pooled DDS decode + complex-material
                   probes) with a per-viewport TextureCache memo on top
ui/                NifViewport (single 3D view), NifComparePane (view + Open/Close),
                   NifCompareSplitCoordinator (1-8 pane two-row equal-width grid),
                   NifCompareControlPanel (bottom strip), NifCompareView (top-level)
schema_reference/  NOTES.md pointing at the upstream niftools/nifxml reference (GPL-3.0,
                   consulted upstream - deliberately not vendored into this MIT repo)
test_assets/       local-only smoke-test .nif fixtures (gitignored - game-
                   derived meshes are not redistributed with this repo)
third_party/       git submodules: FD2D, Floar, external/DirectXTex (DDS decode
                   + D3D11 SRV creation), external/spdlog
CMakeLists.txt     sets up third-party wiring + NIFDiff/NifValidate/ResourceResolveTest targets
GenerateVS2026.ps1/.bat   configures a VS2026 solution under build/
Release.ps1        release automation (-Plan / -Publish); see RELEASING.md
NexusMods_Mod_Description.bbcode   the Nexus Mods description page text
```

## Build

```powershell
git submodule update --init --recursive   # first time only
.\GenerateVS2026.ps1
cmake --build build --config Debug
```

`GenerateVS2026.ps1` runs the submodule update for you if `third_party/FD2D`
or `third_party/Floar` look uninitialized.

## Versioning and releases

Versions are `MAJOR.MINOR.REVISION` (e.g. `1.0.116`): `MAJOR`/`MINOR` are
hand-maintained in [app/res/version.h.in](app/res/version.h.in), while
`REVISION` is the **git commit count**, stamped at build time into
`build/generated/version.h` - so every commit yields a distinct version
nobody has to maintain, and version `1.0.116` is exactly the commit tagged
`v1.0.116`. The number shows up in the title bar (with a `+dev` suffix when
built from a dirty tree, so a local build can't pass for the released one) and
in the exe's VERSIONINFO (Explorer -> Properties -> Details).

Releases are cut with [Release.ps1](Release.ps1) - `-Plan` reports what
changed since the last tag and the version the next commit will carry;
`-Publish` verifies the tree and the changelog, builds Release, packages
`dist\NIFDiff-<version>.zip` for Nexus, tags and pushes. Full walkthrough in
[RELEASING.md](RELEASING.md); user-facing history in
[CHANGELOG.md](CHANGELOG.md).

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

Not yet ported/implemented: skeletal (bone) animation with per-frame
re-skinning, morphs, particles, and property controllers (rigid NIF-embedded
transform animations DO play - see the ANIMATION group under Features; actor
animations are external Havok .hkx, unplayable even in NifSkope).
