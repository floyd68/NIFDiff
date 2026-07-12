<#
.SYNOPSIS
    Generates a Visual Studio 2026 solution/project files for NIFDiff.

.DESCRIPTION
    Creates (if missing) a "build" directory next to this script and runs
    CMake with the "Visual Studio 18 2026" generator to produce
    NIFDiff.sln plus the .vcxproj files for NIFDiff and its dependencies
    (FD2D/ImageCore/DirectXTex/Floar), matching this repo's CMakeLists.txt.

    This script only configures the project (cmake -G ...); it does not
    build it. Open build\NIFDiff.sln in Visual Studio 2026, or build
    from the command line with:
        cmake --build build --config Debug

.PARAMETER Architecture
    Target architecture passed to CMake's -A flag. Defaults to x64.

.PARAMETER Clean
    Deletes an existing build directory before configuring, for a fully
    fresh CMake cache.

.PARAMETER SkipSubmodules
    Skips the automatic "git submodule update --init --recursive" step this
    script otherwise runs first (FD2D/ImageCore/DirectXTex/Floar under
    third_party/ are git submodules - see .gitmodules).

.EXAMPLE
    .\GenerateVS2026.ps1
    Initializes third_party/ submodules if needed, then generates
    build\NIFDiff.sln for x64.

.EXAMPLE
    .\GenerateVS2026.ps1 -Clean
    Wipes any existing build directory, then regenerates from scratch.
#>

[CmdletBinding()]
param(
    [string]$Architecture = "x64",
    [switch]$Clean,
    [switch]$SkipSubmodules
)

$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$buildDir = Join-Path $scriptDir "build"

if (-not $SkipSubmodules)
{
    $fd2dMarker = Join-Path $scriptDir "third_party\FD2D\CMakeLists.txt"
    $floarMarker = Join-Path $scriptDir "third_party\Floar\CMakeLists.txt"
    if (-not (Test-Path $fd2dMarker) -or -not (Test-Path $floarMarker))
    {
        Write-Host "third_party/ submodules look uninitialized - running 'git submodule update --init --recursive'..."
        & git -C $scriptDir submodule update --init --recursive
        if ($LASTEXITCODE -ne 0)
        {
            Write-Error "git submodule update failed with exit code $LASTEXITCODE."
            exit $LASTEXITCODE
        }
    }
}

if ($Clean -and (Test-Path $buildDir))
{
    Write-Host "Removing existing build directory: $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path $buildDir))
{
    Write-Host "Creating build directory: $buildDir"
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

$cmakeArgs = @(
    "-S", $scriptDir,
    "-B", $buildDir,
    "-G", "Visual Studio 18 2026",
    "-A", $Architecture
)

Write-Host "Running: cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0)
{
    Write-Error "CMake configure failed with exit code $exitCode."
    exit $exitCode
}

# Recent CMake/Visual Studio versions may emit the newer XML-based .slnx
# format instead of the classic .sln; detect whichever one was actually
# written rather than hardcoding the extension.
$sln = Get-ChildItem -Path $buildDir -Filter "NIFDiff.sln*" -File |
    Sort-Object Name | Select-Object -First 1

Write-Host ""
if ($sln)
{
    Write-Host "Done. Solution generated at:"
    Write-Host "  $($sln.FullName)"
}
else
{
    Write-Host "Done. CMake reported success, but no NIFDiff.sln/.slnx was found in:"
    Write-Host "  $buildDir"
}
Write-Host ""
Write-Host "Open it in Visual Studio 2026, or build from the command line with:"
Write-Host "  cmake --build `"$buildDir`" --config Debug"
