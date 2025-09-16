# Detect CPU cores
$cpuCores = [System.Environment]::ProcessorCount

# Create build directory if it doesn't exist
if (-Not (Test-Path -Path "./build")) {
    New-Item -ItemType Directory -Path "./build" | Out-Null
}

# Change to build directory
Set-Location "./build"

# Set VCPKG_ROOT environment variable
$env:VCPKG_ROOT = "..\vcpkg"

# Run CMake configure step
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="..\vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_BUILD_TESTING=ON

# Build with detected CPU cores
cmake --build . --config Release --target ALL_BUILD -- "/maxcpucount:$cpuCores"
