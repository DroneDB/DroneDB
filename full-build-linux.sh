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

# ----------------------------------------------------------------------
# build-lod Gaussian Splat LOD producer (vendor/spark): MANDATORY for Gaussian
# Splat builds. Locally failures NEVER abort the main build; Gaussian Splat
# (RAD) builds stay deferred (BuildDepMissingException) until build-lod is
# available. CI sets DDB_REQUIRE_BUILDLOD to make a missing tool a hard error
# in scripts/build-buildlod.sh.
# ----------------------------------------------------------------------
(
    cd ..
    if [ -x "./scripts/build-buildlod.sh" ]; then
        ./scripts/build-buildlod.sh build "${BUILD_TYPE}" || \
            echo "WARNING: scripts/build-buildlod.sh failed (non-blocking, Gaussian Splat builds deferred until build-lod is available)"
    elif [ -f "./scripts/build-buildlod.sh" ]; then
        bash ./scripts/build-buildlod.sh build "${BUILD_TYPE}" || \
            echo "WARNING: scripts/build-buildlod.sh failed (non-blocking, Gaussian Splat builds deferred until build-lod is available)"
    fi
    if [ ! -x "build/build-lod" ]; then
        echo ""
        echo "************************************************************************"
        echo "* WARNING: build-lod was NOT built/found in build/                    *"
        echo "* gsplat tests will be SKIPPED when running ddbtest without            *"
        echo "* DDB_REQUIRE_BUILDLOD. Install Rust or run scripts/build-buildlod.sh   *"
        echo "************************************************************************"
        echo ""
    fi
)

# ----------------------------------------------------------------------
# Optional: download the OpenDroneMap Obj2Tiles binary (OGC 3D Tiles generator).
# Delegated to scripts/download-obj2tiles.sh so the same logic is shared with
# CI workflows and packaging. Failures NEVER abort the main build; DroneDB skips
# 3D Tiles generation (Nexus still produced) when Obj2Tiles is missing.
# ----------------------------------------------------------------------
(
    cd ..
    if [ -x "./scripts/download-obj2tiles.sh" ]; then
        ./scripts/download-obj2tiles.sh build || \
            echo "WARNING: scripts/download-obj2tiles.sh failed (non-blocking, 3D Tiles generation disabled)"
    elif [ -f "./scripts/download-obj2tiles.sh" ]; then
        bash ./scripts/download-obj2tiles.sh build || \
            echo "WARNING: scripts/download-obj2tiles.sh failed (non-blocking, 3D Tiles generation disabled)"
    fi
)
