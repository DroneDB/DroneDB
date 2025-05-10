# Detect CPU cores
$cpuCores = [System.Environment]::ProcessorCount

# Remove the build directory if it exists
if (Test-Path -Path "./build") {
    Remove-Item -Recurse -Force "./build"
}

# Create build directory
New-Item -ItemType Directory -Path "./build" | Out-Null

# Change to build directory
Set-Location "./build"

# Run CMake configure step
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

# Build with detected CPU cores
cmake --build . --config Release --target ALL_BUILD -- "/maxcpucount:$cpuCores"
