<#
.SYNOPSIS
    NIFDiff release automation: plan, package, tag and publish a Nexus build.

.DESCRIPTION
    Versions are MAJOR.MINOR.REVISION where MAJOR/MINOR live in
    cmake/Version.cmake and REVISION is the git commit count - so the version
    is a function of the history, not a number anyone maintains by hand.

    The release runs in two phases, because the interesting half (deciding
    what changed and writing it up) is not automatable:

      1. .\Release.ps1 -Plan [-Bump minor|major]
         Reports the last release, every commit since it, and the version the
         next commit will carry. Optionally rewrites cmake/Version.cmake.
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

    # Rewrites cmake/Version.cmake. Commit that edit together with the docs.
    [Parameter(ParameterSetName = 'Plan')]
    [ValidateSet('major', 'minor', 'none')]
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
$VersionCM = Join-Path $Root 'cmake\Version.cmake'
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

function Get-MajorMinor {
    if (-not (Test-Path $VersionCM)) { Die "missing $VersionCM" }
    $t = Get-Content $VersionCM -Raw
    if ($t -notmatch 'NIFDIFF_VERSION_MAJOR\s+(\d+)') { Die "no MAJOR in $VersionCM" }
    $maj = [int]$Matches[1]
    if ($t -notmatch 'NIFDIFF_VERSION_MINOR\s+(\d+)') { Die "no MINOR in $VersionCM" }
    $min = [int]$Matches[1]
    return @{ Major = $maj; Minor = $min }
}

function Set-MajorMinor([int]$Major, [int]$Minor) {
    $t = Get-Content $VersionCM -Raw
    $t = $t -replace 'set\(NIFDIFF_VERSION_MAJOR\s+\d+\)', "set(NIFDIFF_VERSION_MAJOR $Major)"
    $t = $t -replace 'set\(NIFDIFF_VERSION_MINOR\s+\d+\)', "set(NIFDIFF_VERSION_MINOR $Minor)"
    Set-Content $VersionCM $t -NoNewline
}

function Get-CommitCount { return [int](Git-Out rev-list --count HEAD) }

function Get-LastTag {
    $t = & git -C $Root describe --tags --abbrev=0 --match 'v*' 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $t) { return $null }
    return $t.Trim()
}

# --- PLAN ------------------------------------------------------------------

if ($PSCmdlet.ParameterSetName -eq 'Plan') {
    Step 'Release plan'

    $mm = Get-MajorMinor
    $lastTag = Get-LastTag

    if ($Bump -ne 'none') {
        if ($Bump -eq 'major') { $mm.Major++; $mm.Minor = 0 } else { $mm.Minor++ }
        Set-MajorMinor $mm.Major $mm.Minor
        Ok "cmake/Version.cmake -> $($mm.Major).$($mm.Minor)  (commit this with the docs)"
    }

    # The release commit is not made yet, so it will be commit N+1.
    $next = "$($mm.Major).$($mm.Minor).$((Get-CommitCount) + 1)"

    Info ""
    if ($lastTag) {
        Info "Last release : $lastTag"
        $range = "$lastTag..HEAD"
    } else {
        Info "Last release : (none - this is the first)"
        $range = 'HEAD'
    }
    Info "Next version : $next   (assuming exactly ONE release commit from here)"
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
    Info "3. git add -A && git commit   <- exactly one commit, or the revision shifts"
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

$mm      = Get-MajorMinor
$version = "$($mm.Major).$($mm.Minor).$(Get-CommitCount)"
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
  Either the entry is stale, or you made more than one commit since -Plan.
  Fix the '## $clVersion' heading to '## $version' (and re-check the notes),
  amend the release commit, and re-run.
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
