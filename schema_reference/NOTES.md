# schema_reference/

This directory used to vendor `nif.xml` from the upstream
[niftools/nifxml](https://github.com/niftools/nifxml) project as a format
reference. It was **removed on purpose**: nifxml is licensed **GPL-3.0**,
and this repository is MIT - redistributing the XML here would require
carrying the GPL license text alongside it for a file that is never parsed
at build or run time anyway. (The XML being GPL places no requirement on
NIFDiff's code: the parsers are an independent implementation of the NIF
*format*, written by cross-referencing the XML as documentation.)

To consult the reference while working on `core/NifDocument.cpp`, fetch it
from upstream instead:

- https://github.com/niftools/nifxml/blob/master/nif.xml (GPL-3.0)

The field/line citations in `core/NifDocument.cpp`'s comments were written
against **nifxml 0.10.0.0** (`<niftoolsxml version="0.10.0.0">`), for the
Skyrim LE (BS Version 83), Skyrim SE (BS Version 100), and Fallout 4
(BS Version 130) configurations.
