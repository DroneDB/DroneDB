#!/usr/bin/env bash
# Collects the Corresponding Source of the copyleft library that vcpkg builds into the helper
# executables a release ships: the exact Exiv2 archive linked into metadata-util (GPL-2.0-or-later).
# The tag holds the sources of DroneDB and its helpers but not this one, which vcpkg downloads
# while it builds, so a release attaches it. The other vcpkg libraries the helpers use (the image
# codecs of bm3d) have permissive licences and need no source offer.
#
# Version, URL and SHA512 are read from the vcpkg port files, so the archives are the ones vcpkg
# verified when it built the libraries; each download is checked against the same SHA512 and a
# mismatch fails the script. The port files (portfile.cmake and the patches vcpkg applies to the
# sources) are archived as well: the Corresponding Source is the archive plus those patches.
#
# Usage:
#   scripts/fetch-vcpkg-gpl-sources.sh [VCPKG_DIR] [OUT_DIR]
#     VCPKG_DIR  the vcpkg checkout the build used (default: vcpkg)
#     OUT_DIR    destination folder (default: gpl-sources)
#
# Output in OUT_DIR: exiv2-<version>.tar.gz,
# vcpkg-ports-for-gpl-sources.tar.gz and SOURCES.txt (what each file is and where it came from).
set -euo pipefail

VCPKG_DIR="${1:-vcpkg}"
OUT_DIR="${2:-gpl-sources}"
PORTS="${VCPKG_DIR}/ports"

fail() { echo "error: $*" >&2; exit 1; }

# First capture group of the first line of FILE that matches the POSIX BRE PATTERN.
first_match() {
    local value
    value="$(sed -n "s/${1}/\\1/p" "$2" | head -n 1)"
    [ -n "${value}" ] || fail "no match for '${1}' in $2"
    printf '%s' "${value}"
}

# download <file name> <sha512> <url>...: fetches into OUT_DIR from the first URL that works and
# verifies the SHA512 of the vcpkg port. A mismatch is fatal.
download() {
    local name="$1" sha512="$2" url
    shift 2
    for url in "$@"; do
        if curl -fsSL --retry 3 -o "${OUT_DIR}/${name}" "${url}"; then
            echo "${sha512}  ${OUT_DIR}/${name}" | sha512sum --check --status \
                || fail "${name} from ${url} does not match the SHA512 of the vcpkg port"
            printf '%s\n  from %s\n  sha512 %s\n' "${name}" "${url}" "${sha512}" >> "${OUT_DIR}/SOURCES.txt"
            return 0
        fi
    done
    fail "could not download ${name}"
}

[ -f "${PORTS}/exiv2/portfile.cmake" ] || fail "${PORTS}/exiv2/portfile.cmake not found: is ${VCPKG_DIR} a vcpkg checkout?"

mkdir -p "${OUT_DIR}"
: > "${OUT_DIR}/SOURCES.txt"

# Exiv2: vcpkg_from_github(REPO ... REF "v${VERSION}" SHA512 ...) fetches the GitHub tag archive
exiv2_version="$(first_match '^ *"version": "\([^"]*\)".*' "${PORTS}/exiv2/vcpkg.json")"
exiv2_repo="$(first_match '^ *REPO \([^ ]*\).*' "${PORTS}/exiv2/portfile.cmake")"
exiv2_ref="$(first_match '^ *REF "\([^"]*\)".*' "${PORTS}/exiv2/portfile.cmake")"
exiv2_ref="${exiv2_ref//\$\{VERSION\}/${exiv2_version}}"
exiv2_sha512="$(first_match '^ *SHA512 \([0-9a-f]\{128\}\).*' "${PORTS}/exiv2/portfile.cmake")"
download "exiv2-${exiv2_version}.tar.gz" "${exiv2_sha512}" \
    "https://github.com/${exiv2_repo}/archive/${exiv2_ref}.tar.gz"

# The patches vcpkg applies to that source live in the port directory
tar -czf "${OUT_DIR}/vcpkg-ports-for-gpl-sources.tar.gz" -C "${VCPKG_DIR}" \
    ports/exiv2
printf '%s\n  portfile.cmake and the patches vcpkg applies to the archive above\n' \
    "vcpkg-ports-for-gpl-sources.tar.gz" >> "${OUT_DIR}/SOURCES.txt"

echo "Corresponding source collected in ${OUT_DIR}:"
ls -l "${OUT_DIR}"
