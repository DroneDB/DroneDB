<#
.SYNOPSIS
    Builds the LibreDWG dwg2dxf converter (dwg2dxf.exe) from source on Windows and
    places it next to the DroneDB executable.

.DESCRIPTION
    DroneDB converts DWG drawings to DXF with LibreDWG's dwg2dxf, discovered at
    runtime via cad.cpp::findDwg2DxfBinary (next to ddbcmd.exe, then PATH).
    dwg2dxf is GPL-3.0-or-later and invoked ONLY as a separate subprocess (mere
    aggregation); it does not affect DroneDB's MPL-2.0 license. See
    THIRD_PARTY_LICENSES.md.

    This script runs scripts/build-libredwg.sh inside an MSYS2 MINGW64 shell: the
    same signed GNU tarball, signature check and static build as on Linux. It is
    meant for local development; CI cross-compiles dwg2dxf.exe on Linux
    (build-libredwg.sh <dir> "" x86_64-w64-mingw32) and the Windows job uses that
    artifact.

    MSYS2 packages needed (pacman -S):
      mingw-w64-x86_64-toolchain make mingw-w64-x86_64-pkgconf gnupg curl tar xz

.PARAMETER TargetDir
    Destination folder for dwg2dxf.exe. Default: build

.PARAMETER Version
    LibreDWG release. Default: $env:LIBREDWG_VERSION or value in scripts/libredwg-version

.PARAMETER Msys2Root
    MSYS2 installation. Default: $env:MSYS2_ROOT or C:\msys64

.NOTES
    Exit code 0 on success, when the binary is already present at the requested
    version (idempotent), or when MSYS2 is not installed (soft skip: DWG support is
    optional). Non-zero on build failure (caller decides whether it is fatal).
#>
param(
    [string]$TargetDir = "build",
    [string]$Version = $(
        if ($env:LIBREDWG_VERSION) {
            $env:LIBREDWG_VERSION
        } else {
            $versionFile = Join-Path $PSScriptRoot "libredwg-version"
            if (Test-Path $versionFile) {
                (Get-Content $versionFile -ErrorAction SilentlyContinue).Trim()
            } else {
                "0.14"
            }
        }
    ),
    [string]$Msys2Root = $(if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" })
)

$ErrorActionPreference = 'Stop'

$dest = Join-Path $TargetDir "dwg2dxf.exe"

Write-Host "========================================"
Write-Host "LibreDWG dwg2dxf (optional DWG support)"
Write-Host "========================================"
Write-Host "  Target : $dest"
Write-Host "  Version: $Version"

if (Test-Path $dest) {
    $versionOutput = & $dest --version 2>&1 | Out-String
    if ($versionOutput -match 'dwg2dxf\s+([0-9][0-9.]*)' -and $Matches[1] -eq $Version) {
        Write-Host "  dwg2dxf $Version already present, skipping build."
        exit 0
    }
}

$bash = Join-Path $Msys2Root "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    Write-Host "  MSYS2 not found at $Msys2Root (set -Msys2Root or MSYS2_ROOT), skipping (soft)." -ForegroundColor Yellow
    Write-Host "  DWG support stays disabled until dwg2dxf.exe is next to ddbcmd.exe (or DDB_DWG2DXF_PATH is set)."
    exit 0
}

New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
$targetAbs = (Resolve-Path $TargetDir).Path

# MINGW64 environment: native Windows toolchain, uname reports MINGW64_NT-*,
# which build-libredwg.sh treats as a native Windows build (dwg2dxf.exe, -all-static).
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"
$env:LIBREDWG_VERSION = $Version
$command = "./build-libredwg.sh `"`$(cygpath -u '$targetAbs')`" '$Version'"

Push-Location $PSScriptRoot
try {
    & $bash -lc $command
    if ($LASTEXITCODE -ne 0) { throw "build-libredwg.sh failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

if (-not (Test-Path $dest)) { throw "dwg2dxf.exe was not produced in $TargetDir" }
Write-Host "  dwg2dxf.exe $Version installed to $dest" -ForegroundColor Green
