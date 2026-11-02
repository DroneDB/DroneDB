#!/usr/bin/env python3
"""Copies the licence notice of every vcpkg port that ends up in a shipped binary into a folder.

Most vcpkg ports are linked into libddb, ddbcmd and the helper executables (statically on Linux,
as DLLs next to them on Windows), and most of their licences (BSD, MIT, zlib, Apache, the GPL of
Exiv2 ...) require the notice to travel with binary distributions. Both packages ship the folder
this script fills: /usr/share/doc/ddb/third-party in the Debian package, third-party/ in the
Windows zip.

The ports are the ones installed for the triplet (<installed>/vcpkg/info/<port>_<version>_<triplet>.list,
which lists the files each port installed). A port is left out when:

  * it installs nothing but build metadata in share/ (CMake files, vcpkg's records): vcpkg's
    CMake helpers, meta ports, and ports that are empty on a platform, such as libiconv on Linux,
    which uses the C library's, or
  * it is in BUILD_ONLY_PORTS: tools that run while the libraries are built, and the test
    framework, none of which is linked into a shipped binary.

Any other port must have installed share/<port>/copyright, or the script fails: a notice that
goes missing would otherwise only be noticed by a reader of the package.

Output: <port>.LICENSE per port and index.txt (port, version, file, SPDX licence vcpkg records).

Usage:
    scripts/collect-vcpkg-notices.py --installed build/vcpkg_installed --output DIR [--triplet T]

Without --triplet, the only triplet installed is used. Exit codes: 0 ok, 1 failure.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

INDEX_NAME = "index.txt"
UNRECORDED_LICENCE = "not recorded: see the file"

# Ports that run during the build or the tests only
BUILD_ONLY_PORTS = {
    "gtest": "test framework, linked into the test executables only",
    "pdal-dimbuilder": "generates a PDAL header while PDAL is built",
    "pkgconf": "pkg-config, run by the builds of other ports",
}

# vcpkg's own build helpers (vcpkg-cmake, vcpkg-tool-meson ...): CMake scripts and build tools
BUILD_HELPER_PREFIX = "vcpkg-"


# What vcpkg keeps in share/ for the build: CMake package files and wrappers, the port's notice,
# usage text and vcpkg's own records. Anything else (libraries, headers, tools, data) may ship.
SHARE_BUILD_FILES = {"copyright", "usage", "vcpkg_abi_info.txt"}


def is_build_metadata(path: str, triplet: str) -> bool:
    if not path.startswith(f"{triplet}/share/"):
        return False
    name = path.rsplit("/", 1)[-1]
    return name.endswith(".cmake") or name in SHARE_BUILD_FILES or name.startswith("vcpkg")


@dataclass
class Port:
    name: str
    version: str
    files: list[str]  # installed files, relative to <installed>, without the folder entries

    def notice(self, triplet: str) -> str | None:
        copyright_file = f"{triplet}/share/{self.name}/copyright"
        return copyright_file if copyright_file in self.files else None

    def ships_files(self, triplet: str) -> bool:
        return any(not is_build_metadata(f, triplet) for f in self.files)


def installed_triplets(installed: Path) -> list[str]:
    info = installed / "vcpkg" / "info"
    return sorted({p.stem.rsplit("_", 1)[1] for p in info.glob("*_*_*.list")})


def installed_ports(installed: Path, triplet: str) -> list[Port]:
    ports = []
    for listing in sorted((installed / "vcpkg" / "info").glob(f"*_{triplet}.list")):
        # Port names hold no '_', triplets neither: the version is what lies between
        name, rest = listing.stem.split("_", 1)
        version = rest[: -(len(triplet) + 1)]
        files = [line.strip() for line in listing.read_text(encoding="utf-8").splitlines()]
        ports.append(Port(name, version, [f for f in files if f and not f.endswith("/")]))
    return ports


def spdx_licence(installed: Path, triplet: str, port: str) -> str:
    """The licence expression vcpkg recorded for the port, or a pointer to the notice."""
    path = installed / triplet / "share" / port / "vcpkg.spdx.json"
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return UNRECORDED_LICENCE
    for package in document.get("packages", []):
        if package.get("SPDXID") == "SPDXRef-port":
            licence = package.get("licenseConcluded")
            # vcpkg writes these when the port declares no licence
            if licence and licence not in ("NOASSERTION", "LicenseRef-vcpkg-null"):
                return licence
    return UNRECORDED_LICENCE


def exclusion(port: Port, triplet: str) -> str | None:
    """Why the port ships nothing, or None when its notice must ship."""
    if port.name.startswith(BUILD_HELPER_PREFIX):
        return "vcpkg build helper"
    if port.name in BUILD_ONLY_PORTS:
        return BUILD_ONLY_PORTS[port.name]
    if not port.ships_files(triplet):
        return "installs only build metadata"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--installed", type=Path, required=True, help="the vcpkg_installed folder of the build")
    parser.add_argument("--output", type=Path, required=True, help="folder to fill (created; must be empty)")
    parser.add_argument("--triplet", help="vcpkg triplet of the shipped binaries (default: the only one installed)")
    args = parser.parse_args()

    triplets = installed_triplets(args.installed)
    triplet = args.triplet
    if not triplet:
        if len(triplets) != 1:
            print(f"ERROR: {args.installed} holds the triplets {triplets or 'none'}: pass --triplet", file=sys.stderr)
            return 1
        triplet = triplets[0]
    elif triplet not in triplets:
        print(f"ERROR: triplet {triplet} is not installed in {args.installed} ({triplets or 'none'})", file=sys.stderr)
        return 1

    if args.output.exists() and any(args.output.iterdir()):
        print(f"ERROR: {args.output} is not empty: stale notices would ship", file=sys.stderr)
        return 1

    shipped, skipped, problems = [], [], []
    for port in installed_ports(args.installed, triplet):
        reason = exclusion(port, triplet)
        if reason:
            skipped.append((port.name, reason))
        elif (notice := port.notice(triplet)) is None:
            problems.append(f"{port.name} installs files but no share/{port.name}/copyright")
        else:
            shipped.append((port, notice))

    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        return 1
    if not shipped:
        print(f"ERROR: no port with a notice is installed for {triplet} in {args.installed}", file=sys.stderr)
        return 1

    args.output.mkdir(parents=True, exist_ok=True)
    index = [
        f"Licence notices of the vcpkg libraries in these binaries ({triplet}): one <port>.LICENSE per port.",
        "",
        f"{'port':<24} {'version':<16} {'file':<32} licence (as vcpkg records it)",
    ]
    for port, notice in shipped:
        file_name = f"{port.name}.LICENSE"
        shutil.copyfile(args.installed / notice, args.output / file_name)
        licence = spdx_licence(args.installed, triplet, port.name)
        index.append(f"{port.name:<24} {port.version:<16} {file_name:<32} {licence}")
    (args.output / INDEX_NAME).write_text("\n".join(index) + "\n", encoding="utf-8")

    print(f"Collected {len(shipped)} vcpkg licence notices ({triplet}) in {args.output}")
    for name, reason in skipped:
        print(f"  skipped {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
