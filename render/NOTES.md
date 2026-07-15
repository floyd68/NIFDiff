# render/

Empty scaffold placeholder - git does not track empty directories, so this
file holds the spot.

To implement here:
- `RenderDevice.h/.cpp` - the single app-wide D3D11 render core (shaders,
  state objects, IBL cubemap, fallback textures) and the draw path for a
  `SceneBuilder` render list. Split out of the former monolithic
  `D3D11Renderer`, which duplicated all of this per viewport.
- `RenderTarget.h/.cpp` - one offscreen color+depth framebuffer; each
  `NifViewport` owns one (and item 12's thumbnail renderer owns more).
- `RenderMeshCache.h` - a per-view cache of GPU geometry buffers keyed by
  borrowed `NifGeometry` pointers.
- `shaders/*.hlsl` - HLSL source, precompiled at build time by fxc into
  bytecode headers (see the top-level CMakeLists.txt "Shader precompilation"
  section; formerly inline strings in a `Shaders.h`, compiled at runtime).
- `TextureCache.h/.cpp` - decodes DDS (via `lib/gli`, not yet vendored here)
  and other formats via ImageCore/DirectXTex, keyed by resolved
  `ResourceBytes`.

`RenderDevice` replaces [NifSkope](https://github.com/niftools/nifskope)'s
Qt/OpenGL `src/gl/renderer.cpp/.h` with a plain D3D11 draw path consuming
`core/SceneBuilder`'s render list; `TextureCache` plays the same role as
NifSkope's `src/gl/gltex.cpp`+`gltexloaders.cpp`, but decodes through
FICture2's own ImageCore/DirectXTex pipeline instead. See README.md's
"Origins" section for the license notes that come with each (NifSkope
subsection for the render-path logic, FICture2 subsection for the decode
pipeline).

See README.md at the repo root for the overall porting plan.
