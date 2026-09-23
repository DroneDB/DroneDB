#!/bin/bash
# Builds the build-lod Gaussian Splat LOD producer binary for DroneDB
# (mandatory for Gaussian Splat builds, see DDB_REQUIRE_BUILDLOD below).
#
# Usage:
#   scripts/build-buildlod.sh [BUILD_DIR] [BUILD_TYPE] [VENDOR_DIR]
#
# Defaults:
#   BUILD_DIR  = build
#   BUILD_TYPE = Release
#   VENDOR_DIR = vendor/spark
#
# Compiles Spark's `build-lod` Rust CLI (vendor/spark/rust) with cargo and copies the
# resulting `build-lod` binary next to the main DroneDB binaries inside BUILD_DIR, where it
# is picked up by both packaging (build-debian-package.sh) and runtime discovery
# (buildlod_runner.cpp::findBuildLodBinary -> getExeFolder()).
#
# The GPU feature (wgpu) is disabled (--no-default-features): it is only used by the optional
# --cluster-sh path, which DroneDB does not use. Without it build-lod is a self-contained,
# pure-Rust binary with no system-library dependencies.
#
# This script is intentionally tolerant of missing sources/toolchain LOCALLY: when
# vendor/spark is not present (submodule not initialised) or cargo is unavailable it prints
# an informational message and exits 0. In that case Gaussian Splat (RAD) builds are
# deferred: buildGsplat throws BuildDepMissingException until build-lod is available
# (no delivery artifact is produced).
# Set DDB_REQUIRE_BUILDLOD (done by CI) to turn a missing source/toolchain into a hard
# error instead, so gsplat builds cannot silently ship without the mandatory LOD producer.
#
# Exit codes:
#   0  success, OR vendor/spark / cargo not present and DDB_REQUIRE_BUILDLOD unset (soft skip)
#   1  cargo build / copy failure, or missing source/toolchain with DDB_REQUIRE_BUILDLOD set

set -u

BUILD_DIR="${1:-build}"
BUILD_TYPE="${2:-Release}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${3:-${REPO_ROOT}/vendor/spark}"

# Make BUILD_DIR absolute when relative
case "${BUILD_DIR}" in
    /*) ;;
    *) BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" ;;
esac

MANIFEST="${VENDOR_DIR}/rust/Cargo.toml"

echo "========================================"
echo "build-lod (Gaussian Splat LOD producer - mandatory for gsplat builds)"
echo "========================================"
echo "  Source : ${VENDOR_DIR}"
echo "  Build  : ${BUILD_DIR}/build-lod"
echo "  Config : ${BUILD_TYPE}"

if [ ! -f "${MANIFEST}" ]; then
    if [ -n "${DDB_REQUIRE_BUILDLOD:-}" ]; then
        echo "ERROR: vendor/spark or rust/Cargo.toml missing but DDB_REQUIRE_BUILDLOD is set." >&2
        echo "       build-lod is a MANDATORY dependency of Gaussian Splat builds." >&2
        echo "       Run: git submodule update --init vendor/spark" >&2
        exit 1
    fi
    if [ -d "${VENDOR_DIR}" ]; then
        echo "INFO: vendor/spark exists but rust/Cargo.toml is missing."
        echo "      The git submodule is probably not initialised. Run:"
        echo "        git submodule update --init vendor/spark"
    else
        echo "INFO: vendor/spark not present, skipping build-lod build (Gaussian Splat builds deferred until build-lod is available)."
    fi
    exit 0
fi

if ! command -v cargo >/dev/null 2>&1; then
    if [ -n "${DDB_REQUIRE_BUILDLOD:-}" ]; then
        echo "ERROR: cargo (Rust toolchain) not found on PATH but DDB_REQUIRE_BUILDLOD is set." >&2
        echo "       build-lod is a MANDATORY dependency of Gaussian Splat builds." >&2
        echo "       Install Rust from https://rustup.rs/." >&2
        exit 1
    fi
    echo "INFO: cargo (Rust toolchain) not found on PATH; skipping build-lod build."
    echo "      Install Rust from https://rustup.rs/ to enable Gaussian Splat LOD builds."
    exit 0
fi

if [ ! -d "${BUILD_DIR}" ]; then
    echo "ERROR: BUILD_DIR '${BUILD_DIR}' does not exist. Build DroneDB first." >&2
    exit 1
fi

CARGO_PROFILE_FLAGS=""
CARGO_TARGET_SUBDIR="debug"
if [ "${BUILD_TYPE}" != "Debug" ]; then
    CARGO_PROFILE_FLAGS="--release"
    CARGO_TARGET_SUBDIR="release"
fi

echo ""
echo "Building build-lod (${BUILD_TYPE}, --no-default-features)..."
cargo build \
    ${CARGO_PROFILE_FLAGS} \
    --locked \
    --package build-lod \
    --no-default-features \
    --manifest-path "${MANIFEST}" \
    || { echo "ERROR: cargo build failed for build-lod" >&2; exit 1; }

# cargo writes to <workspace>/target/{profile}/build-lod by default.
BUILDLOD_BIN="${VENDOR_DIR}/rust/target/${CARGO_TARGET_SUBDIR}/build-lod"
if [ ! -x "${BUILDLOD_BIN}" ]; then
    BUILDLOD_BIN="$(find "${VENDOR_DIR}/rust/target" -maxdepth 4 -type f -name build-lod -perm -u+x 2>/dev/null | head -n1)"
fi

if [ -z "${BUILDLOD_BIN}" ] || [ ! -x "${BUILDLOD_BIN}" ]; then
    echo "ERROR: build-lod binary not found under ${VENDOR_DIR}/rust/target after build" >&2
    exit 1
fi

cp -f "${BUILDLOD_BIN}" "${BUILD_DIR}/build-lod"
chmod +x "${BUILD_DIR}/build-lod"
echo ""
echo "SUCCESS: build-lod copied to ${BUILD_DIR}/build-lod"
exit 0
