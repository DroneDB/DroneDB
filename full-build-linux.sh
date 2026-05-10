#!/bin/bash
set -e

# Detect CPU cores
CPU_CORES=$(nproc)

# Build type: defaults to Release; override with first argument, e.g.:
#   ./full-build-linux.sh Debug
BUILD_TYPE="${1:-Release}"

# Create build directory
mkdir -p build

# Change to build directory
cd build

# Set VCPKG_ROOT environment variable
export VCPKG_ROOT="../vcpkg"

# Run CMake configure step
cmake .. -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DCMAKE_BUILD_TESTING=ON

# Build using detected CPU cores
cmake --build . -- -j"$CPU_CORES"

# ----------------------------------------------------------------------
# Optional: build the Untwine COPC accelerator (vendor/untwine).
# Delegated to scripts/build-untwine.sh so the same logic is shared with
# CI workflows and packaging scripts. Failures NEVER abort the main
# build; DroneDB transparently falls back to PDAL writers.copc when
# untwine is missing.
# ----------------------------------------------------------------------
(
    cd ..
    if [ -x "./scripts/build-untwine.sh" ]; then
        ./scripts/build-untwine.sh build "${BUILD_TYPE}" || \
            echo "WARNING: scripts/build-untwine.sh failed (non-blocking, PDAL fallback will be used)"
    elif [ -f "./scripts/build-untwine.sh" ]; then
        bash ./scripts/build-untwine.sh build "${BUILD_TYPE}" || \
            echo "WARNING: scripts/build-untwine.sh failed (non-blocking, PDAL fallback will be used)"
    fi
)
