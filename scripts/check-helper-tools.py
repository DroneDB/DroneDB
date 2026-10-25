#!/usr/bin/env python3
"""Package gate for the helper executables: metadata-util and ddb-nexus must be in the package, run,
report the version of the package, and come with their licence texts.

libddb starts both tools as child processes (and accepts any helper of a compatible protocol at
run time), so a package that lacks one, ships a stale one or leaves out the licence a GPL helper
must be distributed with is broken in a way no other check sees. This script runs for the Debian
package (the extracted tree) and for the Windows zip (the unpacked folder), so both are held to
the same rules:

  tools     <bin-dir>/metadata-util and <bin-dir>/ddb-nexus (".exe" on Windows) exist, are run with
            --version and print "<tool> <version> ..." where <version> is the package version.
  licences  every file named in scripts/helper-licences.txt is in <licence-dir> and not empty.
  notices   <licence-dir>/third-party, filled by scripts/collect-vcpkg-notices.py, holds its index
            and every notice the index names, not empty, among them those of the vcpkg libraries
            built into the helpers (HELPER_PORTS).

Usage:
    scripts/check-helper-tools.py --bin-dir DIR --licence-dir DIR --version VERSION

Exit codes: 0 ok, 1 a check failed.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

LICENCE_LIST = Path(__file__).resolve().parent / "helper-licences.txt"
TOOLS = ("metadata-util", "ddb-nexus")
THIRD_PARTY_FOLDER = "third-party"
THIRD_PARTY_INDEX = "index.txt"
# vcpkg ports built into the helpers: Exiv2 (GPL, metadata-util) and the image codecs of ddb-nexus
HELPER_PORTS = ("exiv2", "libjpeg-turbo", "libpng", "libwebp", "tiff")
VERSION_TIMEOUT_SECONDS = 60


def licence_names() -> list[str]:
    """File names from helper-licences.txt: '<name> | <path>' lines, '#' comments."""
    names = []
    for line in LICENCE_LIST.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            names.append(line.split("|")[0].strip())
    return names


def check_tool(bin_dir: Path, tool: str, version: str) -> str | None:
    """Returns the problem found with `tool`, or None."""
    path = bin_dir / (tool + (".exe" if sys.platform == "win32" else ""))
    if not path.is_file():
        return f"{path} is missing from the package"
    try:
        result = subprocess.run([str(path), "--version"], capture_output=True, text=True,
                                errors="replace", timeout=VERSION_TIMEOUT_SECONDS, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"{path} cannot run: {exc}"
    lines = result.stdout.splitlines()
    if result.returncode != 0 or not lines:
        return (f"{path} --version failed (exit {result.returncode}): "
                f"{(result.stderr or result.stdout).strip()[:300]}")
    words = lines[0].split()
    if len(words) < 2 or words[0] != tool or words[1] != version:
        return f"{tool} reports '{lines[0].strip()}', expected '{tool} {version}'"
    print(f"  {lines[0].strip()}")
    return None


def check_notices(folder: Path) -> list[str]:
    """The problems found with the vcpkg notices in `folder`."""
    index = folder / THIRD_PARTY_INDEX
    if not index.is_file() or index.stat().st_size == 0:
        return [f"{index} is missing or empty (scripts/collect-vcpkg-notices.py not run?)"]
    # The third column of a row of the index is the file name of the notice
    named = [word for line in index.read_text(encoding="utf-8").splitlines()
             for word in line.split()[2:3] if word.endswith(".LICENSE")]
    problems = [] if named else [f"{index} names no notice"]
    for name in named + [f"{port}.LICENSE" for port in HELPER_PORTS if f"{port}.LICENSE" not in named]:
        path = folder / name
        if not path.is_file() or path.stat().st_size == 0:
            problems.append(f"{path} is missing or empty")
    if not problems:
        print(f"  {len(named)} vcpkg licence notices in {folder}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin-dir", type=Path, required=True, help="folder holding the tools")
    parser.add_argument("--licence-dir", type=Path, required=True, help="folder holding the licence texts")
    parser.add_argument("--version", required=True, help="package version the tools must report")
    args = parser.parse_args()

    problems = [p for tool in TOOLS if (p := check_tool(args.bin_dir, tool, args.version))]

    for name in licence_names():
        path = args.licence_dir / name
        if not path.is_file() or path.stat().st_size == 0:
            problems.append(f"{path} is missing or empty")
    problems += check_notices(args.licence_dir / THIRD_PARTY_FOLDER)

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}")
        return 1

    print(f"PASS: metadata-util and ddb-nexus {args.version}, their licences and the vcpkg notices are in the package")
    return 0


if __name__ == "__main__":
    sys.exit(main())
