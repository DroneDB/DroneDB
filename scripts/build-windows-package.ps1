#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Creates a Windows distribution package for DroneDB

.DESCRIPTION
    This script packages DroneDB binaries and dependencies into a distributable ZIP file.
    It assumes the project has already been built in the build directory.

    This script is designed to be run as part of the CI pipeline, but can also be
    executed manually for local package creation.

.PARAMETER BuildDir
    Path to the build directory containing compiled binaries (default: "build")

.PARAMETER Version
    Version string for the package. If not specified, reads from package.json

.PARAMETER OutputDir
    Directory where the ZIP file will be created. If not specified, uses build/dist

.EXAMPLE
    .\scripts\build-windows-package.ps1

.EXAMPLE
    .\scripts\build-windows-package.ps1 -BuildDir "build" -Version "1.2.4"

.NOTES
    Author: DroneDB Team
    This script follows the same pattern as build-debian-package.sh for consistency
#>

param(
    [Parameter(HelpMessage = "Path to the build directory")]
    [string]$BuildDir = "build",

    [Parameter(HelpMessage = "Version string for the package")]
    [string]$Version = "",

    [Parameter(HelpMessage = "Output directory for the ZIP file")]
    [string]$OutputDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Banner
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  DroneDB Windows Package Builder" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Determine version from package.json if not specified
if ([string]::IsNullOrEmpty($Version)) {
    Write-Host "Reading version from package.json..." -ForegroundColor Yellow
    try {
        $packageJson = Get-Content "package.json" -Raw | ConvertFrom-Json
        $Version = $packageJson.version
        Write-Host "  Version: $Version" -ForegroundColor Green
    }
    catch {
        Write-Error "Failed to read version from package.json: $_"
        exit 1
    }
}
else {
    Write-Host "Using specified version: $Version" -ForegroundColor Yellow
}

# Validate build directory exists
if (-not (Test-Path $BuildDir)) {
    Write-Error "Build directory not found: $BuildDir"
    Write-Host "Please build the project first using:" -ForegroundColor Yellow
    Write-Host "  cmake --build $BuildDir --config Release" -ForegroundColor Yellow
    exit 1
}

Write-Host "Build directory: $BuildDir" -ForegroundColor Yellow

# Determine output directory
if ([string]::IsNullOrEmpty($OutputDir)) {
    $OutputDir = Join-Path $BuildDir "dist"
}

# Create directories
Write-Host ""
Write-Host "Preparing directories..." -ForegroundColor Cyan
$stagingDir = Join-Path $BuildDir "package_staging"
if (Test-Path $stagingDir) {
    Write-Host "  Cleaning existing staging directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $stagingDir
}
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Write-Host "  ✓ Staging directory: $stagingDir" -ForegroundColor Green
Write-Host "  ✓ Output directory: $OutputDir" -ForegroundColor Green

# Define required files for verification
$requiredFiles = @(
    "ddbcmd.exe",
    "ddb.dll",
    "nxs.dll",
    "proj.db",
    "timezone21.bin",
    "sensor_data.sqlite",
    "curl-ca-bundle.crt"
)

# Verify required files exist
Write-Host ""
Write-Host "Verifying required files..." -ForegroundColor Cyan
$missingFiles = @()
foreach ($file in $requiredFiles) {
    $path = Join-Path $BuildDir $file
    if (Test-Path $path) {
        Write-Host "  ✓ $file" -ForegroundColor Green
    }
    else {
        Write-Host "  ✗ $file (MISSING)" -ForegroundColor Red
        $missingFiles += $file
    }
}

if ($missingFiles.Count -gt 0) {
    Write-Error "Required files are missing. Please ensure the build completed successfully."
    exit 1
}

# Copy executable
Write-Host ""
Write-Host "Copying executable..." -ForegroundColor Cyan
$exePath = Join-Path $BuildDir "ddbcmd.exe"
Copy-Item $exePath $stagingDir -Force
Write-Host "  ✓ ddbcmd.exe" -ForegroundColor Green

# Copy all DLL files
Write-Host ""
Write-Host "Copying DLL files..." -ForegroundColor Cyan
$dllFiles = Get-ChildItem -Path $BuildDir -Filter "*.dll" -File
$dllCount = 0
foreach ($dll in $dllFiles) {
    Copy-Item $dll.FullName $stagingDir -Force
    $dllCount++
}
Write-Host "  ✓ Copied $dllCount DLL files" -ForegroundColor Green

# Copy support files
Write-Host ""
Write-Host "Copying support files..." -ForegroundColor Cyan
$supportFiles = @(
    "proj.db",
    "sensor_data.sqlite",
    "timezone21.bin",
    "curl-ca-bundle.crt"
)

# Include ddb.bat if it exists (Windows helper script)
if (Test-Path (Join-Path $BuildDir "ddb.bat")) {
    $supportFiles += "ddb.bat"
}

$copiedSupport = 0
foreach ($file in $supportFiles) {
    $sourcePath = Join-Path $BuildDir $file
    if (Test-Path $sourcePath) {
        Copy-Item $sourcePath $stagingDir -Force
        Write-Host "  ✓ $file" -ForegroundColor Green
        $copiedSupport++
    }
    else {
        Write-Host "  - $file (not found, skipping)" -ForegroundColor Yellow
    }
}

# Copy directories (zoneinfo, plugins)
Write-Host ""
Write-Host "Copying directories..." -ForegroundColor Cyan
$directories = @("zoneinfo", "plugins")
$copiedDirs = 0
foreach ($dir in $directories) {
    $sourcePath = Join-Path $BuildDir $dir
    if (Test-Path $sourcePath -PathType Container) {
        Copy-Item -Recurse $sourcePath $stagingDir -Force
        $fileCount = (Get-ChildItem -Recurse $sourcePath -File).Count
        Write-Host "  ✓ $dir/ ($fileCount files)" -ForegroundColor Green
        $copiedDirs++
    }
    else {
        Write-Host "  - $dir/ (not found, skipping)" -ForegroundColor Yellow
    }
}

# Count total files staged
$totalFiles = (Get-ChildItem -Recurse $stagingDir -File).Count
Write-Host ""
Write-Host "Staging complete:" -ForegroundColor Yellow
Write-Host "  Total files: $totalFiles" -ForegroundColor Yellow

# Create ZIP package
$zipName = "ddb-$Version-Windows.zip"
$zipPath = Join-Path $OutputDir $zipName

Write-Host ""
Write-Host "Creating ZIP package..." -ForegroundColor Cyan
if (Test-Path $zipPath) {
    Write-Host "  Removing existing package..." -ForegroundColor Yellow
    Remove-Item $zipPath -Force
}

try {
    # Use Compress-Archive with optimal compression
    Compress-Archive -Path "$stagingDir\*" -DestinationPath $zipPath -CompressionLevel Optimal -Force
    Write-Host "  ✓ Package created successfully" -ForegroundColor Green
}
catch {
    Write-Error "Failed to create ZIP package: $_"
    exit 1
}

# Display final information
$zipSize = (Get-Item $zipPath).Length / 1MB
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Package Creation Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Package details:" -ForegroundColor Yellow
Write-Host "  File: $zipPath" -ForegroundColor White
Write-Host "  Size: $([Math]::Round($zipSize, 2)) MB" -ForegroundColor White
Write-Host "  Files: $totalFiles" -ForegroundColor White
Write-Host "  Version: $Version" -ForegroundColor White
Write-Host ""

# Optional: List contents for verification (commented out by default)
# Write-Host "Package contents:" -ForegroundColor Yellow
# Get-ChildItem -Recurse $stagingDir -File | Select-Object -ExpandProperty Name | ForEach-Object { Write-Host "  - $_" }

Write-Host "Package is ready for distribution!" -ForegroundColor Green
Write-Host ""

# Note: We intentionally keep the staging directory for inspection
# To clean it up, uncomment the following line:
# Remove-Item -Recurse -Force $stagingDir

exit 0
