# Releasing NIFDiff

## Versioning

```
MAJOR . MINOR . REVISION
  |       |        `-- git commit count (rev-list --count HEAD), automatic
  |       `----------- hand-bumped for feature releases
  `------------------- hand-bumped for breaking/landmark releases
```

- **MAJOR/MINOR** live in [app/res/version.h.in](app/res/version.h.in) - the
  only two numbers a human maintains. `Release.ps1 -Bump minor|major` rewrites
  them.
- **REVISION** is the commit count, stamped at **build** time by
  [cmake/GenerateVersion.cmake](cmake/GenerateVersion.cmake) into
  `build/generated/version.h`. Nobody edits it, it never collides, and it only
  moves forward.
- A build from a dirty tree reports `1.0.116+dev` in the title bar, so a local
  build can never be mistaken for the released one. The embedded VERSIONINFO
  (Explorer -> Properties -> Details) and the git hash come from the same header.
- Every released binary maps to exactly one commit: version `1.0.116` is the
  commit tagged `v1.0.116`, whose `rev-list --count` is 116.

Where the version surfaces:

| Where | Value |
|---|---|
| Title bar | `NIFDiff 1.0.116 - NIF Model Compare` (`+dev` if dirty) |
| Explorer → Properties → Details | FileVersion / ProductVersion `1.0.116` |
| Git | tag `v1.0.116` |
| Nexus | version field `1.0.116` |

## Cutting a release

The mechanical half is scripted; the half that needs judgement (deciding what
to tell users) is not. Hence two phases.

### 1. Plan

```powershell
.\Release.ps1 -Plan                # patch-level: revision moves on its own
.\Release.ps1 -Plan -Bump minor    # feature release: 1.0.x -> 1.1.x
.\Release.ps1 -Plan -Bump major    # landmark:        1.x  -> 2.0
```

Prints the last tag, every commit since it, the changed files, and **the
version the next commit will carry**. `-Bump` also rewrites
`app/res/version.h.in` - leave that edit uncommitted for now.

### 2. Write the release up

From the commit list, update:

1. **`CHANGELOG.md`** - a new `## <version>` section at the top. User-facing
   language only: what changed in the app, not which files moved. This is the
   source the other two copy from.
2. **`NexusMods_Mod_Description.bbcode`** - the release link at the top and a
   `What's New in vX.Y.Z` list right under the demo video (see how FICture2's
   description keeps the last 3-4 releases and lets older ones scroll off).
3. **`README.md`** - only if behaviour or features actually changed.

Then commit **everything in exactly one commit** (the version bump, the
changelog, the docs). One commit, because the revision is the commit count -
two commits and the number you wrote in the changelog is off by one.
`-Publish` catches that mismatch rather than shipping it.

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
  gh release create v1.0.116 dist\NIFDiff-1.0.116.zip --title "NIFDiff 1.0.116" --notes-file CHANGELOG.md
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
