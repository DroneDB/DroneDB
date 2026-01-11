[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildType = 'Debug',

    [Parameter(Mandatory=$false)]
    [switch]$Clean,

    [Parameter(Mandatory=$false)]
    [switch]$SkipTests,

    [Parameter(Mandatory=$false)]
    [int]$Jobs = [System.Environment]::ProcessorCount
)

# Enable strict error handling
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Store original location
$originalLocation = Get-Location

function Write-Info {
    param([string]$Message)
    Write-Host "INFO: $Message" -ForegroundColor Cyan
}

function Write-Success {
    param([string]$Message)
    Write-Host "SUCCESS: $Message" -ForegroundColor Green
}

function Write-ErrorMsg {
    param([string]$Message)
    Write-Host "ERROR: $Message" -ForegroundColor Red
}

function Find-VisualStudio {
    Write-Info "Searching for Visual Studio installation..."

    # Method 1: Use vswhere (official Microsoft tool) - preferred method
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        Write-Host "  Using vswhere.exe to locate Visual Studio..." -ForegroundColor Gray

        # Find latest VS with C++ workload
        $vsInstallPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath `
            -format value 2>$null

        if ($vsInstallPath -and (Test-Path $vsInstallPath)) {
            $vcvarsPath = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvarsPath) {
                $vsVersion = & $vswhere -latest -property displayName 2>$null
                Write-Success "Found Visual Studio: $vsVersion"
                Write-Host "  Location: $vsInstallPath" -ForegroundColor Gray
                return $vcvarsPath
            }
        }
    }

    # Method 2: Search in common base directories dynamically
    Write-Host "  Searching Visual Studio directories..." -ForegroundColor Gray
    $vsBasePaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )

    foreach ($basePath in $vsBasePaths) {
        if (-not (Test-Path $basePath)) { continue }

        # Find all version directories (2019, 2022, 2025, 18, etc.)
        $versionDirs = Get-ChildItem -Path $basePath -Directory -ErrorAction SilentlyContinue

        foreach ($versionDir in $versionDirs) {
            # Find all edition directories (Professional, Community, Enterprise, Preview, etc.)
            $editionDirs = Get-ChildItem -Path $versionDir.FullName -Directory -ErrorAction SilentlyContinue

            foreach ($editionDir in $editionDirs) {
                $vcvarsPath = Join-Path $editionDir.FullName "VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path $vcvarsPath) {
                    Write-Success "Found Visual Studio: $($versionDir.Name) $($editionDir.Name)"
                    Write-Host "  Location: $($editionDir.FullName)" -ForegroundColor Gray
                    return $vcvarsPath
                }
            }
        }
    }

    # Method 3: Try environment variables as last resort
    if ($env:VSINSTALLDIR) {
        Write-Host "  Checking VSINSTALLDIR environment variable..." -ForegroundColor Gray
        $vcvarsPath = Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvarsPath) {
            Write-Success "Found Visual Studio via VSINSTALLDIR"
            Write-Host "  Location: $env:VSINSTALLDIR" -ForegroundColor Gray
            return $vcvarsPath
        }
    }

    return $null
}

function Test-Prerequisites {
    Write-Info "Checking prerequisites..."

    # Check CMake
    try {
        $cmakeVersion = cmake --version 2>&1 | Select-Object -First 1
        Write-Success "CMake found: $cmakeVersion"
    } catch {
        Write-ErrorMsg "CMake not found. Please install CMake and add it to PATH."
        return $false
    }

    # Check Python
    try {
        $pythonVersion = python --version 2>&1
        Write-Success "Python found: $pythonVersion"
    } catch {
        Write-ErrorMsg "Python not found. Please install Python 3 and add it to PATH."
        return $false
    }

    # Check Visual Studio
    $vcvarsPath = Find-VisualStudio
    if (-not $vcvarsPath) {
        Write-ErrorMsg "Visual Studio not found. Please install Visual Studio 2019 or later."
        return $false
    }

    return $vcvarsPath
}

function Initialize-BuildEnvironment {
    param([string]$VcvarsPath)

    Write-Info "Setting up Visual Studio build environment..."

    # Import Visual Studio environment variables
    cmd /c "`"$VcvarsPath`" && set" | ForEach-Object {
        if ($_ -match "^(.*?)=(.*)$") {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2] -Force
        }
    }

    # Verify ninja is available
    try {
        $ninjaVersion = ninja --version 2>&1
        Write-Success "Ninja build system found: v$ninjaVersion"
    } catch {
        Write-ErrorMsg "Ninja not found after setting up Visual Studio environment."
        throw "Build tools not properly configured."
    }
}

try {
    Write-Host "`n========================================" -ForegroundColor Magenta
    Write-Host "DroneDB Windows Build Script" -ForegroundColor Magenta
    Write-Host "========================================`n" -ForegroundColor Magenta

    Write-Info "Build Configuration:"
    Write-Host "  - Build Type: $BuildType" -ForegroundColor Yellow
    Write-Host "  - Parallel Jobs: $Jobs" -ForegroundColor Yellow
    Write-Host "  - Testing: $(if ($SkipTests) { 'Disabled' } else { 'Enabled' })" -ForegroundColor Yellow
    Write-Host "  - Clean Build: $(if ($Clean) { 'Yes' } else { 'No' })`n" -ForegroundColor Yellow

    # Check prerequisites
    $vcvarsPath = Test-Prerequisites
    if (-not $vcvarsPath) {
        exit 1
    }

    # Create build directory if it doesn't exist
    $buildDir = Join-Path $PSScriptRoot "build"
    if (-Not (Test-Path $buildDir)) {
        Write-Info "Creating build directory..."
        New-Item -ItemType Directory -Path $buildDir | Out-Null
        Write-Success "Build directory created."
    }

    # Clean build if requested
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Info "Cleaning build directory..."
        Remove-Item -Path (Join-Path $buildDir "CMakeCache.txt") -Force -ErrorAction SilentlyContinue
        Remove-Item -Path (Join-Path $buildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -Path (Join-Path $buildDir "*.dll") -Force -ErrorAction SilentlyContinue
        Remove-Item -Path (Join-Path $buildDir "*.exe") -Force -ErrorAction SilentlyContinue
        Write-Success "Build directory cleaned."
    }

    # Change to build directory
    Set-Location $buildDir
    Write-Info "Working directory: $buildDir"

    # Set VCPKG_ROOT environment variable
    $vcpkgRoot = Join-Path $PSScriptRoot "vcpkg"
    $env:VCPKG_ROOT = $vcpkgRoot
    Write-Info "VCPKG_ROOT: $vcpkgRoot"

    # Initialize build environment
    Initialize-BuildEnvironment -VcvarsPath $vcvarsPath

    # Configure with CMake
    Write-Info "Configuring CMake..."
    $cmakeArgs = @(
        "..",
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_TOOLCHAIN_FILE=..\vcpkg\scripts\buildsystems\vcpkg.cmake"
    )

    if (-not $SkipTests) {
        $cmakeArgs += "-DCMAKE_BUILD_TESTING=ON"
    }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-ErrorMsg "CMake configuration failed!"
        throw "CMake configuration failed with exit code $LASTEXITCODE"
    }
    Write-Success "CMake configuration completed."

    # Build
    Write-Info "Building DroneDB ($BuildType)..."
    Write-Host "Using $Jobs parallel jobs`n" -ForegroundColor Yellow

    & cmake --build . --config $BuildType -- "-j$Jobs"
    if ($LASTEXITCODE -ne 0) {
        Write-ErrorMsg "Build failed!"
        throw "Build failed with exit code $LASTEXITCODE"
    }

    Write-Success "Build completed successfully!"

    # Display build artifacts
    Write-Host "`n========================================" -ForegroundColor Magenta
    Write-Host "Build Artifacts:" -ForegroundColor Magenta
    Write-Host "========================================" -ForegroundColor Magenta

    $artifacts = Get-ChildItem -Path $buildDir -Include "ddbcmd.exe", "ddb.dll", "ddbtest.exe" -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.DirectoryName -notlike "*vcpkg*" -and $_.DirectoryName -notlike "*CMakeFiles*" } |
        Select-Object -First 3

    foreach ($artifact in $artifacts) {
        $sizeKB = [math]::Round($artifact.Length / 1KB, 2)
        Write-Host "  ✓ $($artifact.Name) ($sizeKB KB)" -ForegroundColor Green
        Write-Host "    Location: $($artifact.Directory)" -ForegroundColor Gray
    }

    # Test the executable
    Write-Host "`n========================================" -ForegroundColor Magenta
    Write-Host "Verification:" -ForegroundColor Magenta
    Write-Host "========================================" -ForegroundColor Magenta

    $ddbcmdPath = Join-Path $buildDir "ddbcmd.exe"
    if (Test-Path $ddbcmdPath) {
        Write-Info "Testing ddbcmd.exe..."
        $testOutput = & $ddbcmdPath 2>&1 | Select-Object -First 3
        if ($testOutput) {
            Write-Success "ddbcmd.exe is working correctly!"
            Write-Host $testOutput -ForegroundColor Gray
        }
    }

    Write-Host "`n========================================" -ForegroundColor Magenta
    Write-Success "BUILD COMPLETED SUCCESSFULLY!"
    Write-Host "========================================`n" -ForegroundColor Magenta

    exit 0

} catch {
    Write-Host "`n========================================" -ForegroundColor Red
    Write-ErrorMsg "BUILD FAILED!"
    Write-Host "========================================" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor Gray
    exit 1
} finally {
    # Return to original location
    Set-Location $originalLocation
}
