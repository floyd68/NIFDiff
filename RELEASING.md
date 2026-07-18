# Releasing NIFDiff

## Versioning

```
MAJOR . MINOR . REVISION
  |       |        `-- patch/release increment
  |       `----------- feature release increment
  `------------------- breaking/landmark release increment
```

- All three components live in
  [app/res/version.h.in](app/res/version.h.in), the single hand-maintained
  version source.
- Update the header manually or run
  `Release.ps1 -Plan -Bump patch|minor|major`. An AI release agent may make the
  same edit while preparing the release.
- CMake copies the header to `build/generated/version.h`. The title bar, About
  dialog and embedded VERSIONINFO (Explorer -> Properties -> Details) all use
  that value.
- Tag the release commit with the matching `v<version>` tag so each released
  binary still maps to one source revision.

Where the version surfaces:

| Where | Value |
|---|---|
| Title bar | `NIFDiff 1.0.156 - NIF Model Compare` |
| Explorer → Properties → Details | FileVersion / ProductVersion `1.0.156` |
| Git | tag `v1.0.156` |
| Nexus | version field `1.0.156` |

## Cutting a release

The mechanical half is scripted; the half that needs judgement (deciding what
to tell users) is not. Hence two phases.

### 1. Plan

```powershell
.\Release.ps1 -Plan -Bump patch    # patch release:   1.0.156 -> 1.0.157
.\Release.ps1 -Plan -Bump minor    # feature release: 1.0.156 -> 1.1.0
.\Release.ps1 -Plan -Bump major    # landmark:        1.0.156 -> 2.0.0
.\Release.ps1 -Plan                # inspect without changing the version
```

Prints the last tag, every commit since it, the changed files, and the fixed
release version. `-Bump` also rewrites `app/res/version.h.in` - leave that edit
uncommitted while writing the release notes.

### 2. Write the release up

From the commit list, update:

1. **`CHANGELOG.md`** - a new `## <version>` section at the top. User-facing
   language only: what changed in the app, not which files moved. This is the
   source the other two copy from.
2. **`NexusMods_Mod_Description.bbcode`** - the release link at the top and a
   `What's New in vX.Y.Z` list right under the demo video (see how FICture2's
   description keeps the last 3-4 releases and lets older ones scroll off).
3. **`README.md`** - only if behaviour or features actually changed.

Then commit the version bump, changelog and documentation. `-Publish` verifies
that the fixed header version, changelog, executable VERSIONINFO and tag all
agree before shipping.

### 3. Publish

```powershell
.\Release.ps1 -Publish -DryRun     # build + package + verify, no tag/push
.\Release.ps1 -Publish
```

What it enforces before anything is published:

- working tree clean (uncommitted work would not be in the tagged commit)
- the tag does not already exist
- `CHANGELOG.md`'s newest entry matches the computed version
- the built exe's embedded FileVersion matches the computed version
  (catches a stale or dirty build)
- warns if the bbcode never mentions the new version

Then it builds Release, packages `dist\NIFDiff-<version>.zip`, tags
`v<version>` and pushes the commit + tag.

### 4. Upload

- **Nexus**: upload `dist\NIFDiff-<version>.zip`, set the version field to
  `<version>`, paste `NexusMods_Mod_Description.bbcode` into the description.
- **GitHub** (optional, and the link the bbcode header points at):
  ```powershell
  gh release create v1.0.156 dist\NIFDiff-1.0.156.zip --title "NIFDiff 1.0.156" --notes-file CHANGELOG.md
  ```

## Package contents

```
NIFDiff.exe                 the app (single exe, no runtime deps)
shaders\Common.hlsli        shader contract for custom mesh shaders
shaders\Lit.hlsl            the uber-shader (every material path)
shaders\Unlit.hlsl          grid/axes lines
shaders\Highlight.hlsl      selection overlay
shaders\shaders.ini         binding manifest template (all commented out)
LICENSE                     MIT
THIRD_PARTY_NOTICES.md      required notices (NifSkope BSD, LZ4, zlib, ...)
README.md                   usage + feature reference
CHANGELOG.md                release history
```

The shader sources ship loose on purpose: the app prefers them over its
embedded bytecode and hot-reloads edits, which is what makes user shader
customisation work. Deleting them is harmless - the embedded copies take over.
