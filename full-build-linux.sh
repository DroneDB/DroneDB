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

# ----------------------------------------------------------------------
# Optional: build the Untwine binary if vendor/untwine/CMakeLists.txt
# exists. Failures here NEVER abort the script; DroneDB transparently
# falls back to PDAL writers.copc when untwine is missing.
# ----------------------------------------------------------------------
UNTWINE_SRC="../vendor/untwine"
if [ -f "${UNTWINE_SRC}/CMakeLists.txt" ]; then
    echo "Building optional Untwine COPC accelerator..."
    (
        set +e
        mkdir -p untwine
        cd untwine
        cmake "${UNTWINE_SRC}" -DCMAKE_TOOLCHAIN_FILE="../../vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release \
            && cmake --build . -- -j"$CPU_CORES"
        rc=$?
        if [ $rc -eq 0 ]; then
            UNTWINE_BIN=$(find . -maxdepth 4 -type f -name untwine -executable | head -n1)
            if [ -n "$UNTWINE_BIN" ]; then
                cp -f "$UNTWINE_BIN" ../untwine
                echo "  ✓ untwine copied next to ddbcmd"
            else
                echo "  - untwine binary not found in build output"
            fi
        else
            echo "WARNING: untwine build failed (non-blocking, PDAL fallback will be used)"
        fi
    )
fi
