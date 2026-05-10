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
# Optional: build the Untwine binary if vendor/untwine/CMakeLists.txt
# exists. Failures here NEVER abort the script; DroneDB transparently
# falls back to PDAL writers.copc when untwine is missing.
#
# Untwine is built with the same BUILD_TYPE as DroneDB so that shared
# vcpkg dependencies link against a matching CRT / standard library.
# ----------------------------------------------------------------------
UNTWINE_SRC="../vendor/untwine"
if [ -f "${UNTWINE_SRC}/CMakeLists.txt" ]; then
    echo "Building optional Untwine COPC accelerator (${BUILD_TYPE}, matching DroneDB)..."
    (
        set +e
        mkdir -p untwine-build
        cd untwine-build
        cmake "${UNTWINE_SRC}" -DCMAKE_TOOLCHAIN_FILE="../../vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
            && cmake --build . -- -j"$CPU_CORES"
        rc=$?
        if [ $rc -eq 0 ]; then
            UNTWINE_BIN=$(find . -maxdepth 4 -type f -name untwine -executable | head -n1)
            if [ -n "$UNTWINE_BIN" ]; then
                cp -f "$UNTWINE_BIN" ..
                echo "  ✓ untwine copied next to ddbcmd"
            else
                echo "  - untwine binary not found in build output"
            fi
        else
            echo "WARNING: untwine build failed (non-blocking, PDAL fallback will be used)"
        fi
    )
else
    # Check whether the directory is an uninitialised git submodule
    if [ -d "${UNTWINE_SRC}" ]; then
        echo "INFO: vendor/untwine directory exists but CMakeLists.txt is missing."
        echo "      The git submodule is probably not initialised. Run:"
        echo "        git submodule update --init vendor/untwine"
        echo "      then re-run this build script to enable the Untwine COPC backend."
        echo "      DroneDB will use the PDAL writers.copc fallback in the meantime."
    else
        echo "INFO: vendor/untwine not present, skipping optional Untwine build (PDAL fallback will be used)."
    fi
fi
