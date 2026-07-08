<#
.SYNOPSIS
    Downloads the precompiled OpenDroneMap Obj2Tiles binary for Windows and places
    it next to the DroneDB executable.

.DESCRIPTION
    Obj2Tiles generates OGC 3D Tiles (tileset.json + b3dm) from OBJ models. DroneDB
    discovers it at runtime via obj2tiles_runner.cpp::findObj2TilesBinary ->
    getExeFolder(), so copying Obj2Tiles.exe next to ddbcmd.exe is all that is
    needed. Obj2Tiles is AGPL-3.0 and invoked ONLY as a separate subprocess (mere
    aggregation); it does not affect DroneDB's MPL-2.0 license. See
    THIRD_PARTY_LICENSES.md.

.PARAMETER TargetDir
    Destination folder for Obj2Tiles.exe. Default: build

.PARAMETER Version
    Obj2Tiles release tag. Default: $env:OBJ2TILES_VERSION or value in scripts/obj2tiles-version

.NOTES
    Exit code 0 on success or when the binary is already present (idempotent).
    Non-zero on download/extract failure (caller decides whether it is fatal).
#>
param(
    [string]$TargetDir = "build",
    [string]$Version = $(
        if ($env:OBJ2TILES_VERSION) {
            $env:OBJ2TILES_VERSION
        } else {
            $versionFile = Join-Path $PSScriptRoot "obj2tiles-version"
            if (Test-Path $versionFile) {
                (Get-Content $versionFile -ErrorAction SilentlyContinue).Trim()
            } else {
                "v1.5.0"
            }
        }
    )
)

$ErrorActionPreference = 'Stop'

$bin = "Obj2Tiles.exe"
$dest = Join-Path $TargetDir $bin

Write-Host "========================================"
Write-Host "Obj2Tiles (optional OGC 3D Tiles generator)"
Write-Host "========================================"
Write-Host "  Target : $dest"
Write-Host "  Version: $Version"

New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null

# Strip leading 'v' from version tag for comparison with binary output.
$requestedVersion = $Version -replace '^v', ''

# Query installed version from the binary itself using --version.
$needsUpdate = $false
if (Test-Path $dest) {
    $versionOutput = & $dest --version 2>&1 | Out-String
    if ($versionOutput -match 'Obj2Tiles\s+(\d+\.\d+\.\d+)') {
        $installedVersion = $Matches[1]
        if ($installedVersion -eq $requestedVersion) {
            Write-Host "  Obj2Tiles $Version already present, skipping download."
            exit 0
        }
        Write-Host "  Obj2Tiles present but version mismatch (installed: $installedVersion, requested: $Version). Upgrading..."
    } else {
        Write-Host "  Obj2Tiles present but version unreadable. Replacing..."
    }
    $needsUpdate = $true
}

$url = "https://github.com/OpenDroneMap/Obj2Tiles/releases/download/$Version/Obj2Tiles-Win64.zip"
$tmp = Join-Path $env:TEMP ("obj2tiles-" + [Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

try {
    $zip = Join-Path $tmp "obj2tiles.zip"
    Write-Host "  Downloading $url ..."

    $attempt = 0
    while ($true) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
            break
        } catch {
            $attempt++
            if ($attempt -ge 3) { throw }
            Write-Host "  Download failed (attempt $attempt), retrying in 5s..."
            Start-Sleep -Seconds 5
        }
    }

    Expand-Archive -Path $zip -DestinationPath $tmp -Force

    $src = Join-Path $tmp $bin
    if (!(Test-Path $src)) {
        throw "$bin not found in the downloaded archive."
    }

    Copy-Item $src $dest -Force
    Write-Host "  Obj2Tiles $Version installed to $dest"

    # AGPL-3.0 compliance: fetch the upstream LICENSE.md and place it next to the binary.
    $licenseDest = Join-Path $TargetDir "Obj2Tiles.LICENSE.md"
    $licenseUrl = "https://raw.githubusercontent.com/OpenDroneMap/Obj2Tiles/$Version/LICENSE.md"
    try {
        Invoke-WebRequest -Uri $licenseUrl -OutFile $licenseDest -UseBasicParsing
        Write-Host "  Obj2Tiles.LICENSE.md staged to $licenseDest"
    } catch {
        Write-Host "  WARNING: could not fetch Obj2Tiles LICENSE.md from $licenseUrl" -ForegroundColor Yellow
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
