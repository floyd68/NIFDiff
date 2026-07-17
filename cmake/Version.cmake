# NIFDiff version - the MAJOR.MINOR half, hand-maintained.
#
# The third component (revision) is NOT here: it is the git commit count,
# computed at build time by cmake/GenerateVersion.cmake, so every commit
# produces a distinct, monotonically increasing version without anyone
# editing a file. Full version = MAJOR.MINOR.<commit count>.
#
# Release.ps1 -Bump major|minor rewrites the two numbers below; that edit is
# part of the release commit itself (see RELEASING.md).
set(NIFDIFF_VERSION_MAJOR 1)
set(NIFDIFF_VERSION_MINOR 0)
