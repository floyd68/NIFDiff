# core/

Empty scaffold placeholder - git does not track empty directories, so this
file holds the spot.

To implement here:
- `NifTypes.h`, `NifStream.h`, `NifValue.h`, `NifItem.h` - Qt-free primitives
  and binary stream reader.
- `NifDocument.h/.cpp` - curated direct-parser for Skyrim LE/SE/FO4 block
  types (see the scope note at the top of that header before extending it).
- `SceneBuilder.h/.cpp` - flattens the parsed block graph into a render list
  (world transforms only; no skinning/morphs/particles/controllers - static
  bind-pose comparison is the actual feature).
- `Camera.h`, `ResourceResolver.h/.cpp` - view camera and Bethesda-style
  loose/BSA/BA2 texture search order (via Floar).

`NifDocument`/`SceneBuilder` are themselves Qt-free ports of
[NifSkope](https://github.com/niftools/nifskope)'s
`src/model/basemodel.h`+`src/model/nifmodel.h` and `src/gl/glscene.h`+
`glnode.h` respectively - see README.md's "Origins" section (NifSkope
subsection) for the license note that comes with that. `ResourceResolver`'s
Bethesda loose/BSA/BA2 search order reuses FICture2's own Floar submodule -
see the FICture2 subsection there.

See README.md at the repo root for the overall porting plan.
