#!/bin/bash
set -e

# Detect CPU cores
CPU_CORES=$(nproc)

# Remove the build directory if it exists
rm -rf build

# Create build directory
mkdir build

# Change to build directory
cd build

# Run CMake configure step
cmake .. -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release

# Build using detected CPU cores
cmake --build . -- -j"$CPU_CORES"
