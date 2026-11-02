#!/usr/bin/env bash
# Builds the LibreDWG dwg2dxf converter (DWG -> DXF) and places it next to the
# DroneDB executable so runtime discovery (cad.cpp::findDwg2DxfBinary ->
# tool_locator: exe folder, then PATH) finds it without any extra configuration.
#
# dwg2dxf is GPL-3.0-or-later and invoked ONLY as a separate subprocess (mere
# aggregation, same model as Obj2Tiles), so it does not affect DroneDB's
# MPL-2.0 license. See THIRD_PARTY_LICENSES.md. The release tarball is
# verified against the LibreDWG maintainer's GPG key before it is built.
#
# Usage:
#   scripts/build-libredwg.sh [TARGET_DIR] [VERSION] [HOST]
#     TARGET_DIR  destination folder for the binary (default: build)
#     VERSION     LibreDWG release (default: $LIBREDWG_VERSION or scripts/libredwg-version)
#     HOST        empty = native build; x86_64-w64-mingw32 = Windows dwg2dxf.exe,
#                 cross-compiled with mingw-w64 from the same signed tarball
#                 (one fully static .exe: only KERNEL32/ADVAPI32/msvcrt imports)
#
# Environment:
#   LIBREDWG_SOURCE_DIR  when set, the verified tarball + .sig are copied there
#                        (release builds attach them: GPL-3.0 Corresponding Source)
#
# Exit codes:
#   0  success, OR the binary is already present at the requested version
#      (idempotent), OR the platform is unsupported (soft skip - caller treats
#      DWG support as optional)
#   non-zero on download/verify/build failure (caller decides whether it is fatal)
set -euo pipefail

TARGET_DIR="${1:-build}"
_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_DEFAULT_VERSION="$(cat "${_SCRIPT_DIR}/libredwg-version" 2>/dev/null | tr -d '[:space:]' || echo '0.14')"
VERSION="${2:-${LIBREDWG_VERSION:-${_DEFAULT_VERSION}}}"
HOST="${3:-}"

# Reini Urban <reini.urban@gmail.com>, LibreDWG maintainer (signs GNU releases).
SIGNING_KEY="38A4167B0DB69E49C5F7216CB8C28866AB27A7A2"
BIN="dwg2dxf"
EXE="${BIN}"
WINDOWS=false
CONFIGURE_HOST=()
if [ -n "${HOST}" ]; then
    case "${HOST}" in
        *-w64-mingw32) WINDOWS=true ;;
        *) echo "  ERROR: unsupported HOST '${HOST}' (only *-w64-mingw32)."; exit 1 ;;
    esac
    CONFIGURE_HOST=(--host="${HOST}")
fi

case "$(uname -s)" in
    Linux|Darwin) ;;  # native, or the build host of a Windows cross-compile
    # Native Windows build inside an MSYS2 MINGW64 shell (scripts/build-libredwg.ps1).
    MINGW*|MSYS*) WINDOWS=true ;;
    *) echo "  Unsupported OS '$(uname -s)', skipping (soft)."; exit 0 ;;
esac
${WINDOWS} && EXE="${BIN}.exe"
DEST="${TARGET_DIR}/${EXE}"

echo "========================================"
echo "LibreDWG dwg2dxf (optional DWG support)"
echo "========================================"
echo "  Target : ${DEST}"
echo "  Version: ${VERSION}"

mkdir -p "${TARGET_DIR}"
# The version check runs the binary, so it is skipped only when cross-compiling.
if [ -z "${HOST}" ] && [ -x "${DEST}" ]; then
    INSTALLED="$("${DEST}" --version 2>/dev/null | sed -n 's/.*dwg2dxf[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' | head -n 1 || true)"
    if [ "${INSTALLED}" = "${VERSION}" ]; then
        echo "  dwg2dxf ${VERSION} already present, skipping build."
        exit 0
    fi
    echo "  dwg2dxf present (${INSTALLED:-unknown version}), rebuilding ${VERSION}..."
fi

TOOLS=(curl gpg tar make cc pkg-config)
[ -n "${HOST}" ] && TOOLS+=("${HOST}-gcc" "${HOST}-strip")
for tool in "${TOOLS[@]}"; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "  ERROR: '${tool}' is required to build LibreDWG."; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
TARBALL="libredwg-${VERSION}.tar.xz"
URL="https://ftp.gnu.org/gnu/libredwg/${TARBALL}"

echo "  Downloading ${URL} ..."
curl -fsSL --retry 3 --retry-delay 5 -o "${TMP}/${TARBALL}" "${URL}"
curl -fsSL --retry 3 --retry-delay 5 -o "${TMP}/${TARBALL}.sig" "${URL}.sig"

# Verify against the pinned maintainer key only (isolated keyring, the key is
# fetched from the GNU keyring and must match the pinned fingerprint).
export GNUPGHOME="${TMP}/gnupg"; mkdir -m 700 "${GNUPGHOME}"
curl -fsSL --retry 3 -o "${TMP}/gnu-keyring.gpg" https://ftp.gnu.org/gnu/gnu-keyring.gpg
gpg --batch --quiet --import "${TMP}/gnu-keyring.gpg" 2>/dev/null || true
STATUS="$(gpg --batch --status-fd 1 --verify "${TMP}/${TARBALL}.sig" "${TMP}/${TARBALL}" 2>/dev/null || true)"
if ! printf '%s\n' "${STATUS}" | grep -q "^\[GNUPG:\] VALIDSIG ${SIGNING_KEY} "; then
    echo "  ERROR: ${TARBALL} is not signed by ${SIGNING_KEY}."
    exit 1
fi
echo "  Signature OK (${SIGNING_KEY})"

# GPL-3.0 section 6: releases that ship dwg2dxf must also offer its Corresponding
# Source. CI sets LIBREDWG_SOURCE_DIR and attaches these files to the release.
if [ -n "${LIBREDWG_SOURCE_DIR:-}" ]; then
    mkdir -p "${LIBREDWG_SOURCE_DIR}"
    cp "${TMP}/${TARBALL}" "${TMP}/${TARBALL}.sig" "${LIBREDWG_SOURCE_DIR}/"
    echo "  Source kept in ${LIBREDWG_SOURCE_DIR}/${TARBALL}"
fi

tar -xJf "${TMP}/${TARBALL}" -C "${TMP}"
cd "${TMP}/libredwg-${VERSION}"
# Static, tools only: one self-contained binary, nothing to install system-wide.
./configure "${CONFIGURE_HOST[@]}" --disable-shared --enable-static --disable-bindings --disable-docs \
    --disable-python --enable-release --prefix="${TMP}/prefix" >"${TMP}/configure.log" 2>&1 \
    || { tail -40 "${TMP}/configure.log"; exit 1; }
# Windows: libtool drops a plain -static for programs; -all-static also folds in
# the mingw stack-protector runtime (otherwise dwg2dxf.exe needs libssp-0.dll).
MAKE_LDFLAGS=()
${WINDOWS} && MAKE_LDFLAGS=(LDFLAGS=-all-static)
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" "${MAKE_LDFLAGS[@]}" >"${TMP}/make.log" 2>&1 \
    || { tail -40 "${TMP}/make.log"; exit 1; }
make install "${MAKE_LDFLAGS[@]}" >"${TMP}/install.log" 2>&1 || { tail -40 "${TMP}/install.log"; exit 1; }

cd - >/dev/null
cp "${TMP}/prefix/bin/${EXE}" "${DEST}"
"${HOST:+${HOST}-}strip" "${DEST}" 2>/dev/null || true
chmod +x "${DEST}"
cp "${TMP}/libredwg-${VERSION}/COPYING" "${TARGET_DIR}/${BIN}.LICENSE.txt"
echo "  ${EXE} ${VERSION} installed to ${DEST}"
[ -z "${HOST}" ] && "${DEST}" --version | head -n 1
OBJDUMP="${HOST:+${HOST}-}objdump"
if ${WINDOWS} && command -v "${OBJDUMP}" >/dev/null 2>&1; then
    # A Windows build must not need DLLs we do not ship.
    EXTRA="$("${OBJDUMP}" -p "${DEST}" | sed -n 's/.*DLL Name: //p' | grep -viE '^(kernel32|advapi32|msvcrt|user32|ws2_32)\.dll$' || true)"
    [ -z "${EXTRA}" ] || { echo "  ERROR: ${EXE} imports unexpected DLLs: ${EXTRA}"; exit 1; }
fi
