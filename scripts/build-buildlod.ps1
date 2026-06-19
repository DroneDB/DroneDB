[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [string]$BuildDir = "build",

    [Parameter(Mandatory=$false)]
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',

    [Parameter(Mandatory=$false)]
    [string]$VendorDir = ""
)

<#
.SYNOPSIS
    Builds the optional build-lod Gaussian Splat LOD producer binary for DroneDB.

.DESCRIPTION
    Compiles Spark's `build-lod` Rust CLI (expected at vendor/spark/rust relative to the
    repository root, unless -VendorDir is specified) with cargo, then copies the resulting
    `build-lod.exe` next to the main DroneDB binaries in $BuildDir.

    The GPU feature (wgpu) is intentionally disabled (--no-default-features): it is only used
    by the optional --cluster-sh path, which DroneDB does not use. Without it build-lod is a
    self-contained, pure-Rust binary with no system-library dependencies.

    This script is intentionally tolerant: it never throws on failure. The caller (CI workflow,
    full-build-win.ps1, packaging script) decides whether to treat a missing build-lod.exe as
    fatal. DroneDB serves the plain model.spz (no LOD streaming) when build-lod is absent.

.PARAMETER BuildDir
    Directory containing the main DroneDB build (default: "build"). The resulting binary is
    copied to "$BuildDir/build-lod.exe".

.PARAMETER Config
    Cargo profile to use: 'Release' (and the *Rel* configs) map to `--release`; 'Debug' builds
    the unoptimized profile. Defaults to Release.

.PARAMETER VendorDir
    Path to the Spark source tree (the one containing rust/Cargo.toml). Defaults to
    "<repo-root>/vendor/spark".

.EXAMPLE
    .\scripts\build-buildlod.ps1

.EXAMPLE
    .\scripts\build-buildlod.ps1 -BuildDir build -Config Release
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($VendorDir)) {
    $VendorDir = Join-Path $repoRoot "vendor\spark"
}

# Resolve absolute path for $BuildDir if it is relative
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

$manifest = Join-Path $VendorDir "rust\Cargo.toml"

Write-Host "========================================" -ForegroundColor Magenta
Write-Host "build-lod (optional Gaussian Splat LOD producer)" -ForegroundColor Magenta
Write-Host "========================================" -ForegroundColor Magenta
Write-Host "  Source : $VendorDir"
Write-Host "  Build  : $BuildDir\build-lod.exe"
Write-Host "  Config : $Config"

if (-not (Test-Path $manifest)) {
    if (Test-Path $VendorDir) {
        Write-Host "INFO: vendor/spark exists but rust/Cargo.toml is missing." -ForegroundColor Yellow
        Write-Host "      The git submodule is probably not initialised. Run:" -ForegroundColor Yellow
        Write-Host "        git submodule update --init vendor/spark" -ForegroundColor Cyan
    } else {
        Write-Host "INFO: vendor/spark not present, skipping optional build-lod build (model.spz served without LOD)." -ForegroundColor Gray
    }
    exit 0
}

$cargo = Get-Command cargo -ErrorAction SilentlyContinue
if (-not $cargo) {
    Write-Host "INFO: cargo (Rust toolchain) not found on PATH; skipping optional build-lod build." -ForegroundColor Yellow
    Write-Host "      Install Rust from https://rustup.rs/ to enable Gaussian Splat LOD streaming." -ForegroundColor Cyan
    exit 0
}

if (-not (Test-Path $BuildDir)) {
    Write-Host "ERROR: BuildDir '$BuildDir' does not exist. Build DroneDB first." -ForegroundColor Red
    exit 1
}

$cargoArgs = @(
    "build",
    "--package", "build-lod",
    "--no-default-features",
    "--manifest-path", $manifest
)

$profile = "debug"
if ($Config -in @('Release', 'RelWithDebInfo', 'MinSizeRel')) {
    $cargoArgs += "--release"
    $profile = "release"
}

Write-Host "`nBuilding build-lod ($profile, --no-default-features)..." -ForegroundColor Cyan
& cargo @cargoArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: cargo build failed for build-lod (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

# cargo writes to <workspace>/target/{profile}/build-lod.exe by default.
$candidate = Join-Path $VendorDir "rust\target\$profile\build-lod.exe"
$buildLodBin = $null
if (Test-Path $candidate) {
    $buildLodBin = Get-Item $candidate
} else {
    $buildLodBin = Get-ChildItem -Path (Join-Path $VendorDir "rust\target") -Filter "build-lod.exe" -Recurse `
        -ErrorAction SilentlyContinue | Select-Object -First 1
}

if ($buildLodBin) {
    $dest = Join-Path $BuildDir "build-lod.exe"
    Copy-Item $buildLodBin.FullName $dest -Force
    Write-Host "`nSUCCESS: build-lod.exe copied to $dest" -ForegroundColor Green
    exit 0
} else {
    Write-Host "`nERROR: build-lod.exe not found under $VendorDir\rust\target after build" -ForegroundColor Red
    exit 1
}
