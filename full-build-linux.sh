#!/bin/bash
set -e

# Detect CPU cores
CPU_CORES=$(nproc)

# Create build directory
mkdir -p build

# Change to build directory
cd build

# Set VCPKG_ROOT environment variable
export VCPKG_ROOT="../vcpkg"

# Run CMake configure step
cmake .. -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_TESTING=ON

# Build using detected CPU cores
cmake --build . -- -j"$CPU_CORES"
