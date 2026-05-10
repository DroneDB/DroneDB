#!/bin/bash
# Builds the optional Untwine COPC accelerator binary for DroneDB.
#
# Usage:
#   scripts/build-untwine.sh [BUILD_DIR] [BUILD_TYPE] [VENDOR_DIR]
#
# Defaults:
#   BUILD_DIR  = build
#   BUILD_TYPE = Release
#   VENDOR_DIR = vendor/untwine
#
# This script is intentionally tolerant of missing sources: when
# vendor/untwine is not present (or its submodule is not initialised) it
# prints an informational message and exits 0, since DroneDB falls back
# to PDAL writers.copc at runtime when untwine is missing.
#
# When the build succeeds the produced `untwine` binary is copied next to
# the main DroneDB binaries inside BUILD_DIR so that it is picked up by
# both packaging scripts (build-debian-package.sh) and runtime discovery
# (untwine_runner.cpp::findUntwineBinary -> getExeFolder()).
#
# Exit codes:
#   0  success, OR vendor/untwine not present (caller treats as soft skip)
#   1  CMake configure / build / copy failure (caller decides if fatal)

set -u

BUILD_DIR="${1:-build}"
BUILD_TYPE="${2:-Release}"

# Resolve repo root (one level above this script's directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${3:-${REPO_ROOT}/vendor/untwine}"

# Make BUILD_DIR absolute when relative
case "${BUILD_DIR}" in
    /*) ;;
    *) BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" ;;
esac

CPU_CORES="$(nproc 2>/dev/null || echo 4)"

echo "========================================"
echo "Untwine (optional COPC accelerator)"
echo "========================================"
echo "  Source : ${VENDOR_DIR}"
echo "  Build  : ${BUILD_DIR}/untwine-build"
echo "  Config : ${BUILD_TYPE}"

if [ ! -f "${VENDOR_DIR}/CMakeLists.txt" ]; then
    if [ -d "${VENDOR_DIR}" ]; then
        echo "INFO: vendor/untwine directory exists but CMakeLists.txt is missing."
        echo "      The git submodule is probably not initialised. Run:"
        echo "        git submodule update --init vendor/untwine"
    else
        echo "INFO: vendor/untwine not present, skipping optional Untwine build (PDAL fallback will be used)."
    fi
    exit 0
fi

if [ ! -d "${BUILD_DIR}" ]; then
    echo "ERROR: BUILD_DIR '${BUILD_DIR}' does not exist. Build DroneDB first." >&2
    exit 1
fi

UNTWINE_BUILD="${BUILD_DIR}/untwine-build"
mkdir -p "${UNTWINE_BUILD}"

VCPKG_TOOLCHAIN="${REPO_ROOT}/vcpkg/scripts/buildsystems/vcpkg.cmake"

(
    cd "${UNTWINE_BUILD}"
    echo ""
    echo "Configuring Untwine (${BUILD_TYPE})..."
    cmake "${VENDOR_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_TOOLCHAIN}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DBUILD_TESTING=OFF
) || { echo "ERROR: cmake configure failed for untwine" >&2; exit 1; }

(
    cd "${UNTWINE_BUILD}"
    echo ""
    echo "Building Untwine (${BUILD_TYPE})..."
    cmake --build . -- -j"${CPU_CORES}"
) || { echo "ERROR: cmake build failed for untwine" >&2; exit 1; }

# Locate the produced binary (path varies depending on Untwine layout)
UNTWINE_BIN="$(find "${UNTWINE_BUILD}" -maxdepth 5 -type f -name untwine -executable 2>/dev/null | head -n1)"
if [ -z "${UNTWINE_BIN}" ]; then
    echo "ERROR: untwine binary not found in ${UNTWINE_BUILD} after build" >&2
    exit 1
fi

cp -f "${UNTWINE_BIN}" "${BUILD_DIR}/untwine"
chmod +x "${BUILD_DIR}/untwine"
echo ""
echo "SUCCESS: untwine copied to ${BUILD_DIR}/untwine"
exit 0
