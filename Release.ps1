<#
.SYNOPSIS
    NIFDiff release automation: plan, package, tag and publish a Nexus build.

.DESCRIPTION
    Versions are fixed in app/res/version.h.in and are updated explicitly for
    each release, either by a person or by an AI agent.

    The release runs in two phases, because the interesting half (deciding
    what changed and writing it up) is not automatable:

      1. .\Release.ps1 -Plan [-Bump patch|minor|major]
         Reports the last release, every commit since it, and the version the
         release will carry. Optionally rewrites app/res/version.h.in.
         You then write CHANGELOG.md / the Nexus bbcode / README and COMMIT.

      2. .\Release.ps1 -Publish
         Verifies the tree and that CHANGELOG.md's top entry matches the
         computed version, builds Release, packages dist\NIFDiff-<ver>.zip,
         tags v<ver> and pushes the commit + tag.

    See RELEASING.md for the full walkthrough.

.EXAMPLE
    .\Release.ps1 -Plan -Bump minor
    .\Release.ps1 -Publish
#>
[CmdletBinding(DefaultParameterSetName = 'Plan')]
param(
    [Parameter(ParameterSetName = 'Plan')]
    [switch]$Plan,

    # Rewrites app/res/version.h.in. Commit that edit together with the docs.
    [Parameter(ParameterSetName = 'Plan')]
    [ValidateSet('major', 'minor', 'patch', 'none')]
    [string]$Bump = 'none',

    [Parameter(ParameterSetName = 'Publish', Mandatory = $true)]
    [switch]$Publish,

    # Build + package + verify, but make no tag and push nothing.
    [Parameter(ParameterSetName = 'Publish')]
    [switch]$DryRun,

    # Package from the existing build instead of recompiling.
    [Parameter(ParameterSetName = 'Publish')]
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root      = $PSScriptRoot
$BuildDir  = Join-Path $Root 'build'
$DistDir   = Join-Path $Root 'dist'
$VersionHeader = Join-Path $Root 'app\res\version.h.in'
$Changelog = Join-Path $Root 'CHANGELOG.md'
$Bbcode    = Join-Path $Root 'NexusMods_Mod_Description.bbcode'

function Info  ($m) { Write-Host "  $m" }
function Step  ($m) { Write-Host "`n=== $m" -ForegroundColor Cyan }
function Ok    ($m) { Write-Host "  OK   $m" -ForegroundColor Green }
function Warn  ($m) { Write-Host "  WARN $m" -ForegroundColor Yellow }
function Die   ($m) { Write-Host "  FAIL $m" -ForegroundColor Red; exit 1 }

# No param block on purpose: a declared parameter would prefix-match git's own
# short flags (PowerShell binds `Git-Out tag -a v1.0` by matching `-a` to a
# `-Args` parameter, not by passing it through). The automatic $args takes
# every argument verbatim instead.
function Git-Out {
    $out = & git -C $Root @args 2>&1
    if ($LASTEXITCODE -ne 0) { Die "git $($args -join ' ') failed:`n$out" }
    return ($out | Out-String).Trim()
}

# --- version helpers -------------------------------------------------------

function Get-Version {
    if (-not (Test-Path $VersionHeader)) { Die "missing $VersionHeader" }
    $t = Get-Content $VersionHeader -Raw
    if ($t -notmatch '(?m)^#define NIFDIFF_VERSION_MAJOR[ \t]+(\d+)[ \t]*$') { Die "no MAJOR in $VersionHeader" }
    $maj = [int]$Matches[1]
    if ($t -notmatch '(?m)^#define NIFDIFF_VERSION_MINOR[ \t]+(\d+)[ \t]*$') { Die "no MINOR in $VersionHeader" }
    $min = [int]$Matches[1]
    if ($t -notmatch '(?m)^#define NIFDIFF_VERSION_REVISION[ \t]+(\d+)[ \t]*$') { Die "no REVISION in $VersionHeader" }
    $rev = [int]$Matches[1]
    return @{ Major = $maj; Minor = $min; Revision = $rev }
}

function Set-Version([int]$Major, [int]$Minor, [int]$Revision) {
    $version = "$Major.$Minor.$Revision"
    $t = Get-Content $VersionHeader -Raw
    $t = $t -replace '(?m)^#define NIFDIFF_VERSION_MAJOR[ \t]+\d+[ \t]*$', "#define NIFDIFF_VERSION_MAJOR    $Major"
    $t = $t -replace '(?m)^#define NIFDIFF_VERSION_MINOR[ \t]+\d+[ \t]*$', "#define NIFDIFF_VERSION_MINOR    $Minor"
    $t = $t -replace '(?m)^#define NIFDIFF_VERSION_REVISION[ \t]+\d+[ \t]*$', "#define NIFDIFF_VERSION_REVISION $Revision"
    $t = $t -replace '(?m)^#define NIFDIFF_VERSION_STRING[ \t]+"[^"]+"[ \t]*$', "#define NIFDIFF_VERSION_STRING  `"$version`""
    $t = $t -replace '(?m)^#define NIFDIFF_VERSION_WSTR[ \t]+L"[^"]+"[ \t]*$', "#define NIFDIFF_VERSION_WSTR   L`"$version`""
    Set-Content $VersionHeader $t -NoNewline
}

function Get-LastTag {
    $t = & git -C $Root describe --tags --abbrev=0 --match 'v*' 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $t) { return $null }
    return $t.Trim()
}

# --- PLAN ------------------------------------------------------------------

if ($PSCmdlet.ParameterSetName -eq 'Plan') {
    Step 'Release plan'

    $v = Get-Version
    $lastTag = Get-LastTag

    if ($Bump -ne 'none') {
        switch ($Bump) {
            'major' { $v.Major++; $v.Minor = 0; $v.Revision = 0 }
            'minor' { $v.Minor++; $v.Revision = 0 }
            'patch' { $v.Revision++ }
        }
        Set-Version $v.Major $v.Minor $v.Revision
        Ok "app/res/version.h.in -> $($v.Major).$($v.Minor).$($v.Revision)  (commit this with the docs)"
    }

    $next = "$($v.Major).$($v.Minor).$($v.Revision)"

    Info ""
    if ($lastTag) {
        Info "Last release : $lastTag"
        $range = "$lastTag..HEAD"
    } else {
        Info "Last release : (none - this is the first)"
        $range = 'HEAD'
    }
    Info "Next version : $next"
    Info "Next tag     : v$next"

    Step "Commits since $(if ($lastTag) { $lastTag } else { 'the beginning' })"
    $log = Git-Out log --no-merges --pretty=format:'  %h %s' $range
    if (-not $log) { Warn 'no commits since the last release - nothing to publish' } else { Write-Host $log }

    Step 'Files changed'
    $stat = Git-Out diff --stat "$(if ($lastTag) { "$lastTag..HEAD" } else { '4b825dc642cb6eb9a060e54bf8d69288fbee4904..HEAD' })"
    Write-Host $stat

    Step 'Next steps (manual - this is the part that needs judgement)'
    Info "1. Write the CHANGELOG.md entry for $next from the log above."
    Info "2. Mirror the highlights into NexusMods_Mod_Description.bbcode's"
    Info "   'What's New' section, and update README.md if behaviour changed."
    Info "3. Commit the version header and release documentation."
    Info "4. .\Release.ps1 -Publish"
    exit 0
}

# --- PUBLISH ---------------------------------------------------------------

Step 'Preflight'

$branch = Git-Out rev-parse --abbrev-ref HEAD
if ($branch -ne 'master') { Warn "on branch '$branch', not master" } else { Ok "on $branch" }

$dirty = Git-Out status --porcelain --untracked-files=no
if ($dirty) { Die "working tree has uncommitted changes - commit the release first:`n$dirty" }
Ok 'working tree clean'

$v       = Get-Version
$version = "$($v.Major).$($v.Minor).$($v.Revision)"
$tag     = "v$version"
$hash    = Git-Out rev-parse --short HEAD
Ok "version $version  (HEAD $hash)"

$existing = & git -C $Root tag -l $tag
if ($existing) { Die "tag $tag already exists - the release commit is already tagged" }

# The changelog is the release's human half; refusing to publish without it
# is what keeps the two in sync.
if (-not (Test-Path $Changelog)) { Die "missing CHANGELOG.md" }
$clHead = (Get-Content $Changelog -Raw)
if ($clHead -notmatch '(?m)^##\s*\[?v?([0-9]+\.[0-9]+\.[0-9]+)\]?') {
    Die "CHANGELOG.md has no '## <version>' entry"
}
$clVersion = $Matches[1]
if ($clVersion -ne $version) {
    Die @"
CHANGELOG.md's newest entry is $clVersion but this commit computes to $version.
  Update either the changelog or app/res/version.h.in so they match, then
  commit the correction and re-run.
"@
}
Ok "CHANGELOG.md top entry matches ($version)"

if (Select-String -Path $Bbcode -Pattern ([regex]::Escape($version)) -Quiet) {
    Ok "Nexus bbcode mentions $version"
} else {
    Warn "NexusMods_Mod_Description.bbcode does not mention $version - update its version/What's New before uploading"
}

# --- build ---

if (-not $SkipBuild) {
    Step 'Build (Release)'
    Get-Process NIFDiff -ErrorAction SilentlyContinue | Stop-Process -Force
    & cmake -S $Root -B $BuildDir | Out-Null
    if ($LASTEXITCODE -ne 0) { Die 'cmake configure failed' }
    & cmake --build $BuildDir --config Release --target NIFDiff
    if ($LASTEXITCODE -ne 0) { Die 'build failed' }
    Ok 'built'
} else {
    Warn 'skipping build (-SkipBuild)'
}

$exe = Join-Path $BuildDir 'bin\Release\NIFDiff.exe'
if (-not (Test-Path $exe)) { Die "no Release exe at $exe" }

# The exe must actually carry the version we are about to tag: catches a
# stale build, a dirty-tree stamp, or a generator that skipped the version step.
$fv = (Get-Item $exe).VersionInfo.FileVersion
if ($fv -ne $version) { Die "exe reports FileVersion '$fv' but the release is '$version' (stale build? run without -SkipBuild)" }
Ok "exe FileVersion $fv"

# --- package ---

Step 'Package'
$stage = Join-Path $DistDir "NIFDiff-$version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'shaders') | Out-Null

Copy-Item $exe $stage
# Editable shader sources: the app prefers these over its embedded bytecode,
# and shaders.ini is the (commented-out) binding manifest template.
$shaderSrc = Join-Path $BuildDir 'bin\Release\shaders'
Copy-Item (Join-Path $shaderSrc '*') (Join-Path $stage 'shaders') -Recurse
foreach ($doc in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md')) {
    Copy-Item (Join-Path $Root $doc) $stage
}
Copy-Item $Changelog $stage

$zip = Join-Path $DistDir "NIFDiff-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

$zipMB = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Ok "dist\NIFDiff-$version.zip ($zipMB MB)"
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    Info ("   {0,-40} {1,8:N0} B" -f $_.FullName.Substring($stage.Length + 1), $_.Length)
}

# --- tag + push ---

if ($DryRun) {
    Step 'Dry run - stopping before tag/push'
    Info "Would tag : $tag"
    Info "Would push: origin master + $tag"
    Info "Zip is ready for inspection at dist\NIFDiff-$version.zip"
    exit 0
}

Step 'Tag + push'
$notes = "NIFDiff $version"
Git-Out tag -a $tag -m $notes | Out-Null
Ok "tagged $tag"

Git-Out push origin $branch | Out-Null
Ok "pushed $branch"
Git-Out push origin $tag | Out-Null
Ok "pushed $tag"

Step 'Done'
Info "Upload to Nexus : dist\NIFDiff-$version.zip"
Info "Nexus version   : $version"
Info "Description     : NexusMods_Mod_Description.bbcode"
Info ""
Info "Optional - publish a GitHub release for the same artifact:"
Info "  gh release create $tag `"$zip`" --title `"NIFDiff $version`" --notes-file CHANGELOG.md"
