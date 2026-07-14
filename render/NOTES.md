# render/

Empty scaffold placeholder - git does not track empty directories, so this
file holds the spot.

To implement here:
- `D3D11Renderer.h/.cpp` - the actual D3D11 draw path for a `SceneBuilder`
  render list.
- `shaders/*.hlsl` - HLSL source, precompiled at build time by fxc into
  bytecode headers (see the top-level CMakeLists.txt "Shader precompilation"
  section; formerly inline strings in a `Shaders.h`, compiled at runtime).
- `TextureCache.h/.cpp` - decodes DDS (via `lib/gli`, not yet vendored here)
  and other formats via ImageCore/DirectXTex, keyed by resolved
  `ResourceBytes`.

`D3D11Renderer` replaces [NifSkope](https://github.com/niftools/nifskope)'s
Qt/OpenGL `src/gl/renderer.cpp/.h` with a plain D3D11 draw path consuming
`core/SceneBuilder`'s render list; `TextureCache` plays the same role as
NifSkope's `src/gl/gltex.cpp`+`gltexloaders.cpp`, but decodes through
FICture2's own ImageCore/DirectXTex pipeline instead. See README.md's
"Origins" section for the license notes that come with each (NifSkope
subsection for the render-path logic, FICture2 subsection for the decode
pipeline).

See README.md at the repo root for the overall porting plan.
