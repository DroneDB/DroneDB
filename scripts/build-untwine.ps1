[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [string]$BuildDir = "build",

    [Parameter(Mandatory=$false)]
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',

    [Parameter(Mandatory=$false)]
    [ValidateSet('Ninja', 'VisualStudio')]
    [string]$Builder = 'Ninja',

    [Parameter(Mandatory=$false)]
    [string]$VcpkgRoot = "",

    [Parameter(Mandatory=$false)]
    [string]$VcpkgTriplet = "x64-windows-release",

    [Parameter(Mandatory=$false)]
    [int]$Jobs = [System.Environment]::ProcessorCount,

    [Parameter(Mandatory=$false)]
    [string]$VendorDir = ""
)

<#
.SYNOPSIS
    Builds the optional Untwine COPC accelerator binary for DroneDB.

.DESCRIPTION
    Configures and builds the Untwine source tree (expected at vendor/untwine
    relative to the repository root, unless -VendorDir is specified) using the
    same vcpkg toolchain and dependencies as the main DroneDB build, then
    copies the resulting `untwine.exe` next to the main DroneDB binaries in
    $BuildDir.

    This script is intentionally tolerant: it never throws on failure. The
    caller (CI workflow, full-build-win.ps1, packaging script, ...) decides
    whether to treat a missing untwine.exe as fatal. DroneDB itself falls
    back to PDAL writers.copc at runtime when untwine.exe is absent.

.PARAMETER BuildDir
    Directory containing the main DroneDB build (default: "build"). The
    Untwine sub-build is created at "$BuildDir/untwine" and the resulting
    binary is copied to "$BuildDir/untwine.exe".

.PARAMETER Config
    CMake configuration to use. Must match the configuration used for the
    main DroneDB build so that shared vcpkg dependencies (pdalcpp, gdal,
    proj, ...) link against the matching CRT.

.PARAMETER Builder
    CMake generator: "Ninja" or "VisualStudio". Defaults to Ninja.

.PARAMETER VcpkgRoot
    Path to the vcpkg root. If empty, defaults to "<repo-root>/vcpkg".

.PARAMETER VcpkgTriplet
    vcpkg triplet used to locate the installed dependencies under
    "$BuildDir/vcpkg_installed/<triplet>". Defaults to x64-windows-release.

.PARAMETER VendorDir
    Path to the Untwine source tree. Defaults to "<repo-root>/vendor/untwine".

.EXAMPLE
    .\scripts\build-untwine.ps1

.EXAMPLE
    .\scripts\build-untwine.ps1 -BuildDir build -Config Release -Builder Ninja
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($VendorDir)) {
    $VendorDir = Join-Path $repoRoot "vendor\untwine"
}
if ([string]::IsNullOrEmpty($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $repoRoot "vcpkg"
}

# Resolve absolute path for $BuildDir if it is relative
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

Write-Host "========================================" -ForegroundColor Magenta
Write-Host "Untwine (optional COPC accelerator)" -ForegroundColor Magenta
Write-Host "========================================" -ForegroundColor Magenta
Write-Host "  Source : $VendorDir"
Write-Host "  Build  : $BuildDir\untwine"
Write-Host "  Config : $Config"
Write-Host "  Builder: $Builder"
Write-Host "  Vcpkg  : $VcpkgRoot ($VcpkgTriplet)"

if (-not (Test-Path (Join-Path $VendorDir "CMakeLists.txt"))) {
    if (Test-Path $VendorDir) {
        Write-Host "INFO: vendor/untwine directory exists but CMakeLists.txt is missing." -ForegroundColor Yellow
        Write-Host "      The git submodule is probably not initialised. Run:" -ForegroundColor Yellow
        Write-Host "        git submodule update --init vendor/untwine" -ForegroundColor Cyan
    } else {
        Write-Host "INFO: vendor/untwine not present, skipping optional Untwine build (PDAL fallback will be used)." -ForegroundColor Gray
    }
    exit 0
}

if (-not (Test-Path $BuildDir)) {
    Write-Host "ERROR: BuildDir '$BuildDir' does not exist. Build DroneDB first." -ForegroundColor Red
    exit 1
}

$untwineBuildDir = Join-Path $BuildDir "untwine"
if (-not (Test-Path $untwineBuildDir)) {
    New-Item -ItemType Directory -Path $untwineBuildDir | Out-Null
}

# vcpkg installed dir is shared with the main build. The triplet used by
# the main build is auto-detected: we look for the subdirectory that
# actually contains PDALConfig.cmake (release-only triplets like
# x64-windows-release are preferred on CI; local builds typically use
# x64-windows). Falls back to $VcpkgTriplet when no match is found.
$vcpkgInstalled = Join-Path $BuildDir "vcpkg_installed"
$prefixPath = $null
if (Test-Path $vcpkgInstalled) {
    $candidates = @($VcpkgTriplet, "x64-windows-release", "x64-windows") | Select-Object -Unique
    foreach ($triplet in $candidates) {
        $tripletDir = Join-Path $vcpkgInstalled $triplet
        if (Test-Path (Join-Path $tripletDir "share\pdal\PDALConfig.cmake")) {
            $prefixPath = $tripletDir
            Write-Host "  Found PDAL via triplet: $triplet" -ForegroundColor Green
            break
        }
    }
}
if (-not $prefixPath) {
    $prefixPath = Join-Path $vcpkgInstalled $VcpkgTriplet
    Write-Host "  WARNING: PDALConfig.cmake not found under $vcpkgInstalled. Falling back to $prefixPath" -ForegroundColor Yellow
}

$cmakeArgs = @(
    $VendorDir,
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_MANIFEST_MODE=OFF",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalled",
    "-DCMAKE_PREFIX_PATH=$prefixPath",
    "-DPDAL_DIR=$prefixPath\share\pdal",
    "-DBUILD_TESTING=OFF"
)

function Get-VSGenerator {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsMajor = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property catalog_productLineVersion `
            -format value 2>$null
        $map = @{
            '2019' = 'Visual Studio 16 2019'
            '2022' = 'Visual Studio 17 2022'
            '2025' = 'Visual Studio 18 2025'
        }
        if ($vsMajor -and $map.ContainsKey($vsMajor)) { return $map[$vsMajor] }
    }
    return 'Visual Studio 17 2022'
}

function Enable-MsvcToolchain {
    # The Ninja generator does not embed a compiler choice: CMake probes the
    # PATH to find one. GitHub's windows-* runner images ship MinGW GCC under
    # C:\mingw64\bin, which CMake will happily select. The resulting GCC-ABI
    # objects then fail to link against the MSVC-built vcpkg PDAL with hundreds
    # of "undefined reference to pdal::..." errors. Ensure the MSVC toolchain
    # (cl.exe + INCLUDE/LIB) is active before configuring.
    #
    # No-op when the caller (e.g. full-build-win.ps1) has already imported the
    # Visual Studio environment: cl.exe is then already discoverable on PATH.
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host "  MSVC toolchain already active (cl.exe on PATH)." -ForegroundColor Green
        return
    }

    $vcvars = $null
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath -format value 2>$null
        if ($vsPath) {
            $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { $vcvars = $candidate }
        }
    }
    if (-not $vcvars) {
        Write-Host "  WARNING: vcvars64.bat not found via vswhere; Ninja may select a non-MSVC compiler (e.g. MinGW gcc)." -ForegroundColor Yellow
        return
    }

    Write-Host "  Importing MSVC environment: $vcvars" -ForegroundColor Cyan
    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^(.*?)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2] -Force
        }
    }
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host "  MSVC toolchain activated (cl.exe now on PATH)." -ForegroundColor Green
    } else {
        Write-Host "  WARNING: cl.exe still not found after importing vcvars64.bat." -ForegroundColor Yellow
    }
}

if ($Builder -eq 'Ninja') {
    # On Windows the Ninja generator must run under an MSVC environment so it
    # does not fall back to a stray MinGW gcc on the runner PATH (which would
    # produce objects that cannot link against the MSVC-built vcpkg PDAL).
    if ($env:OS -eq 'Windows_NT') {
        Enable-MsvcToolchain

        # Defensive self-heal: a previous CI run may have configured (and cached)
        # build/untwine with a non-MSVC compiler. CMake caches the compiler in
        # CMakeCache.txt, so reusing such a cache would keep producing unlinkable
        # GCC objects even with cl.exe now on PATH. Wipe it for a clean reconfigure.
        $cacheFile = Join-Path $untwineBuildDir "CMakeCache.txt"
        if (Test-Path $cacheFile) {
            $cachedCxx = $null
            foreach ($line in (Get-Content $cacheFile)) {
                if ($line -match '^CMAKE_CXX_COMPILER:[^=]*=(.*)$') { $cachedCxx = $matches[1]; break }
            }
            if ($cachedCxx -and $cachedCxx -notmatch '(?i)cl\.exe$') {
                Write-Host "  Existing untwine cache uses non-MSVC compiler '$cachedCxx'. Wiping for a clean MSVC reconfigure." -ForegroundColor Yellow
                Remove-Item -Recurse -Force $untwineBuildDir
                New-Item -ItemType Directory -Path $untwineBuildDir | Out-Null
            }
        }
    }
    $cmakeArgs += @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
} else {
    $cmakeArgs += @("-G", (Get-VSGenerator), "-A", "x64")
}

Push-Location $untwineBuildDir
try {
    Write-Host "`nConfiguring Untwine ($Config)..." -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: cmake configure failed for untwine (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    Write-Host "`nBuilding Untwine ($Config)..." -ForegroundColor Cyan
    if ($Builder -eq 'Ninja') {
        & cmake --build . --config $Config -- "-j$Jobs"
    } else {
        & cmake --build . --config $Config -- "/m:$Jobs"
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: cmake build failed for untwine (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

# Locate produced binary anywhere in the sub-build tree
$untwineBin = Get-ChildItem -Path $untwineBuildDir -Filter "untwine.exe" -Recurse `
    -ErrorAction SilentlyContinue | Select-Object -First 1

if ($untwineBin) {
    $dest = Join-Path $BuildDir "untwine.exe"
    Copy-Item $untwineBin.FullName $dest -Force
    Write-Host "`nSUCCESS: untwine.exe copied to $dest" -ForegroundColor Green
    exit 0
} else {
    Write-Host "`nERROR: untwine.exe not found in $untwineBuildDir after build" -ForegroundColor Red
    exit 1
}
