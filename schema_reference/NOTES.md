# schema_reference/

Empty scaffold placeholder - git does not track empty directories, so this
file holds the spot.

`nif.xml` belongs here, vendored from the upstream
[niftools/nifxml](https://github.com/niftools/nifxml) project that
[NifSkope](https://github.com/niftools/nifskope) itself consumes, as a
reference only - it is not parsed at build or run time.
`core/NifDocument.cpp`'s curated block parsers should be hand-written by
cross-referencing this file for the Skyrim LE (BS Version 83), Skyrim SE
(BS Version 100), and Fallout 4 (BS Version 130) configurations; cite the
relevant fields/versions in that file's comments once written. See
README.md's "Origins" section (NifSkope subsection) for the license note
that applies to code (not this XML file itself) derived from NifSkope.
