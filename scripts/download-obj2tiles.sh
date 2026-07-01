#!/usr/bin/env bash
# Downloads the precompiled OpenDroneMap Obj2Tiles binary and places it next to
# the DroneDB executable so runtime discovery
# (obj2tiles_runner.cpp::findObj2TilesBinary -> getExeFolder()) finds it without
# any extra configuration.
#
# Obj2Tiles generates OGC 3D Tiles (tileset.json + b3dm) from OBJ models. It is
# AGPL-3.0 and invoked ONLY as a separate subprocess (mere aggregation), so it
# does not affect DroneDB's MPL-2.0 license. See THIRD_PARTY_LICENSES.md.
#
# Usage:
#   scripts/download-obj2tiles.sh [TARGET_DIR] [VERSION]
#     TARGET_DIR  destination folder for the binary (default: build)
#     VERSION     Obj2Tiles release tag (default: $OBJ2TILES_VERSION or v1.4.0)
#
# Exit codes:
#   0  success, OR the binary is already present (idempotent), OR the platform is
#      unsupported (soft skip - caller treats 3D Tiles as optional)
#   non-zero on download/extract failure (caller decides whether it is fatal)
set -euo pipefail

TARGET_DIR="${1:-build}"
VERSION="${2:-${OBJ2TILES_VERSION:-v1.4.0}}"

BIN="Obj2Tiles"
DEST="${TARGET_DIR}/${BIN}"

echo "========================================"
echo "Obj2Tiles (optional OGC 3D Tiles generator)"
echo "========================================"
echo "  Target : ${DEST}"
echo "  Version: ${VERSION}"

mkdir -p "${TARGET_DIR}"

if [ -x "${DEST}" ]; then
    echo "  Obj2Tiles already present, skipping download."
    exit 0
fi

# Resolve platform -> OpenDroneMap release asset suffix.
OS="$(uname -s)"
ARCH="$(uname -m)"
case "${OS}" in
    Linux)
        case "${ARCH}" in
            x86_64|amd64)   ASSET="Linux64" ;;
            aarch64|arm64)  ASSET="LinuxArm64" ;;
            *) echo "  Unsupported Linux architecture '${ARCH}', skipping (soft)."; exit 0 ;;
        esac ;;
    Darwin)             ASSET="Osx64" ;;
    *) echo "  Unsupported OS '${OS}', skipping (soft)."; exit 0 ;;
esac

URL="https://github.com/OpenDroneMap/Obj2Tiles/releases/download/${VERSION}/Obj2Tiles-${ASSET}.zip"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "  Downloading ${URL} ..."
curl -fL --retry 3 --retry-delay 5 -o "${TMP}/obj2tiles.zip" "${URL}"

# Extract with unzip when available, otherwise fall back to python3 (present on
# the CI runners and the Docker builder image).
if command -v unzip >/dev/null 2>&1; then
    unzip -o -q "${TMP}/obj2tiles.zip" -d "${TMP}"
elif command -v python3 >/dev/null 2>&1; then
    python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
        "${TMP}/obj2tiles.zip" "${TMP}"
else
    echo "  ERROR: neither 'unzip' nor 'python3' is available to extract the archive."
    exit 1
fi

if [ ! -f "${TMP}/${BIN}" ]; then
    echo "  ERROR: ${BIN} not found in the downloaded archive."
    exit 1
fi

cp "${TMP}/${BIN}" "${DEST}"
chmod +x "${DEST}"
echo "  Obj2Tiles installed to ${DEST}"

# AGPL-3.0 compliance: fetch the upstream LICENSE.md and place it next to the binary.
LICENSE_URL="https://raw.githubusercontent.com/OpenDroneMap/Obj2Tiles/${VERSION}/LICENSE.md"
if curl -fL --retry 3 --retry-delay 5 -o "${TARGET_DIR}/Obj2Tiles.LICENSE.md" "${LICENSE_URL}"; then
    echo "  Obj2Tiles.LICENSE.md staged to ${TARGET_DIR}/Obj2Tiles.LICENSE.md"
else
    echo "  WARNING: could not fetch Obj2Tiles LICENSE.md" >&2
fi
