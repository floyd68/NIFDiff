# ui/

Empty scaffold placeholder - git does not track empty directories, so this
file holds the spot.

To implement here:
- `NifViewport.h/.cpp` - one 3D view (camera nav + D3D11Renderer host).
- `NifCompareControlPanel.h/.cpp` - shared bottom control strip (sync
  views/lighting, orientation presets, wireframe, Game Data/override
  folders).
- `NifCompareView.h/.cpp` - top-level layout: two side-by-side
  `NifViewport`s (each with its own "Open ..." button docked underneath)
  plus the shared `NifCompareControlPanel`, wired together with
  FD2D's `SplitPanel`/`DockPanel`.

`NifCompareView`'s layout - two panes + a shared bottom control strip - is
NifSkope's `src/ui/nifdiffviewer.cpp`+`.h` (the feature described in
`D:\Works\nifskope\NIF_DIFF_VIEWER.md`, the original Qt `File -> NIF Diff
Viewer...` window) reimplemented on FD2D and explicitly laid out to mirror
FICture2's own `ImageBrowser`-pair-plus-controls composition pattern - see
README.md's "Origins" section (both subsections apply here) for the
license notes that come with each, and the "Next steps" section for the
overall porting plan.
