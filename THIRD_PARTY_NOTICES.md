# Third-Party Notices

NIFDiff itself is MIT licensed (see [LICENSE](LICENSE)). This file collects
the licenses and required notices for everything else that ships in the
source tree or gets linked into the built binaries, plus provenance notes
for referenced-but-not-included projects.

Summary of what the built `NIFDiff.exe` actually contains:

| Component | Role | License | Full text |
|---|---|---|---|
| Ported NifSkope code | NIF parsing / scene / render logic in `core/`, `render/`, `ui/` | BSD-3-style | notice below |
| FD2D | Win32/Direct2D UI framework | MIT | `third_party/FD2D/LICENSE` |
| Floar | VFS + BSA/BA2 archive readers | MIT (same author as NIFDiff) | `third_party/Floar/LICENSE` |
| DirectXTex | DDS decode + D3D11 SRV creation | MIT (c) Microsoft Corporation | `third_party/external/DirectXTex/LICENSE` |
| spdlog | logging | MIT (c) Gabi Melman & contributors | `third_party/external/spdlog/LICENSE` |
| zlib | BSA zlib decompression (via Floar) | zlib license | `third_party/Floar/external/zlib/LICENSE` |
| LZ4 | BSA LZ4 decompression (via Floar) | BSD 2-Clause (c) 2011-2023 Yann Collet | notice below |

Present in the source tree but **not** compiled into NIFDiff binaries
(`FLOAR_ENABLE_COMMON_ARCHIVES=OFF`): libarchive
(`third_party/Floar/external/libarchive/COPYING`) and xz
(`third_party/Floar/external/xz/COPYING*`).

---

## NifSkope (ported source code)

Parts of `core/`, `render/`, and `ui/` are Qt-free reimplementations/ports
of [NifSkope](https://github.com/niftools/nifskope) sources - see
README.md's "Origins" section for the file-by-file mapping. NifSkope's
license requires this notice to accompany source and binary
redistributions of the derived code:

```
NIFSKOPE LICENSE

Copyright (c) 2005-2014, NIF File Format Library and Tools.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the NIF File Format Library and Tools project may not be
   used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

(NifSkope's own bundled libraries - Havok, Qhull - were not ported and are
not part of NIFDiff.)

## LZ4

`third_party/Floar/external/lz4/lz4.c`/`lz4.h` are vendored inside Floar
for BSA LZ4-block decompression. BSD 2-Clause; the notice below must be
reproduced with binary redistributions:

```
LZ4 - Fast LZ compression algorithm
Copyright (C) 2011-2023, Yann Collet.

BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## DirectXTex / spdlog / zlib / FD2D

Permissive licenses with their full text in the submodule paths listed in
the summary table. For binary-only distributions, copy those license texts
alongside this file.

---

## Referenced conventions (no code included)

NIFDiff renders several community *file-format conventions*. Conventions
and published interfaces are implemented here from their observable
behavior/documentation; **no source code from the projects below is
included**:

- **ENB "complex material"** (`_m.dds` with R=reflection, G=glossiness,
  B=metalness, A=parallax height, detected by a non-opaque coarsest-mip
  alpha): texture-format convention only. ENB is closed-source.
- **Community Shaders / PBRNifPatcher "True PBR"** (SLSF2_Unused01 as the
  PBR marker, repurposed shader flags/slots, RMAOS packing): flag and slot
  conventions only. The parallax-occlusion-mapping and height-field
  self-shadow functions in `render/shaders/Lit.hlsl` are an independent
  implementation of the standard public technique (Tatarchuk, GDC 2006);
  an earlier development revision had ported Community Shaders' GPL-3.0
  `ExtendedMaterials.hlsli` and was rewritten to remove that derivation.

## schema_reference/nif.xml

Vendored from [niftools/nifxml](https://github.com/niftools/nifxml) as a
format reference only - it is not parsed at build or run time and is not
part of the built binaries. It remains under its upstream project's terms.
