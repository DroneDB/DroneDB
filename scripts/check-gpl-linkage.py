#!/usr/bin/env python3
"""GPL isolation guard: libddb and the ddb CLI must not contain or load Exiv2, Nexus or libnxs, and
the Nexus helper must stay free of Qt.

Exiv2 (GPL-2.0-or-later) is used only by the separate `metadata-util` executable, and Nexus, vcglib
and corto only by the separate `ddb-nexus` executable (with its libnxs). libddb talks to both as
child processes, so a build whose libddb.so or ddbcmd carries their code again would link GPL code
into the library. Qt is the other way round: ddb-nexus decodes textures with plain image codec
libraries and no longer uses Qt, so Qt code in ddb-nexus or libnxs (or in libddb/ddbcmd) would
bring back a dependency, and with it the display libraries (X11, xcb, GL, dbus) the Debian package
had to declare for it, that nothing else needs. This check looks at the files that ship:

  symbols   `nm -C --defined-only` lists every symbol, not only the exported ones. The version
            script hides most of libddb's exports, so `nm -D` would be blind to linked-in code
            that is merely hidden. Fails on a defined symbol in Exiv2::, nx::, vcg::, crt:: or a
            Qt class (Q[A-Z]...::). ddbcmd embeds its objects, so it gets the same check.
            ddb-nexus and libnxs are checked for Qt classes only: Nexus code is theirs.
  needed    `readelf -d` NEEDED entries. libddb and ddbcmd fail on libnxs, libexiv2 or libQt5;
            ddb-nexus and libnxs fail on Qt and on the display libraries Qt pulled in.

The binaries need their symbol tables for this: a stripped file would pass without proof, so it
is reported as an error instead.

Linux ELF only. The Windows counterpart is scripts/check-gpl-linkage.ps1, which checks the DLLs
the same files import (`dumpbin /dependents`) and has no symbol check.

Usage:
    scripts/check-gpl-linkage.py [BUILD_DIR]       BUILD_DIR defaults to ./build

Exit codes: 0 no violation found, 1 violation, 2 usage or tooling error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLE_LIMIT = 3

# What a symbol check can see, by the library it came from. The patterns match the demangled
# name nm prints after the address and the type letter.
EXIV2_SYMBOLS = re.compile(r"Exiv2::")
NEXUS_SYMBOLS = re.compile(r" (?:nx|vcg|crt)::")
QT_SYMBOLS = re.compile(r" Q[A-Z][A-Za-z]+::")
NEEDED_LINE = re.compile(r"\(NEEDED\).*\[(.+)\]")


@dataclass(frozen=True)
class Rule:
    """What the files of one group must not contain: NEEDED entries and defined symbols."""
    files: tuple[str, ...]
    needed: re.Pattern[str]
    symbols: dict[str, re.Pattern[str]]
    failure: str


RULES = (
    Rule(files=("libddb.so", "ddbcmd"),
         needed=re.compile(r"libnxs|libexiv2|libQt5"),
         symbols={"Exiv2": EXIV2_SYMBOLS, "Nexus/vcglib/corto": NEXUS_SYMBOLS, "Qt": QT_SYMBOLS},
         failure="GPL code is linked into libddb or ddbcmd; Exiv2 belongs to metadata-util and "
                 "Nexus, vcglib and corto to ddb-nexus"),
    # Qt is statically linked on Linux when it is used at all, so the symbol check is the one
    # that proves its absence; the NEEDED check also catches the display libraries it loads.
    Rule(files=("ddb-nexus", "libnxs.so"),
         needed=re.compile(r"^(?:libQt\d|libX11|libxcb|libxkbcommon|libGL\.|libEGL\.|libdbus)"),
         symbols={"Qt": QT_SYMBOLS},
         failure="Qt is back in ddb-nexus or libnxs, which decode textures with image codec "
                 "libraries only"),
)


class ToolError(Exception):
    """A tool the check depends on is missing or failed."""


def run_tool(*args: str) -> str:
    try:
        result = subprocess.run(args, capture_output=True, text=True, errors="replace", check=False)
    except FileNotFoundError as exc:
        raise ToolError(f"{args[0]} not found: install binutils") from exc
    if result.returncode != 0:
        raise ToolError(f"{' '.join(args)} failed: {result.stderr.strip() or result.returncode}")
    return result.stdout


def needed_libraries(binary: Path) -> list[str]:
    return [m.group(1) for line in run_tool("readelf", "-d", str(binary)).splitlines()
            if (m := NEEDED_LINE.search(line))]


def defined_symbols(binary: Path) -> list[str]:
    symbols = run_tool("nm", "-C", "--defined-only", str(binary)).splitlines()
    if not symbols:
        raise ToolError(f"{binary} has no symbol table (stripped?): the symbol check would pass "
                        "without proving anything")
    return symbols


def check_binary(binary: Path, rule: Rule) -> list[str]:
    """Returns one message per violation of `rule` found in `binary`."""
    violations = []

    for library in needed_libraries(binary):
        if rule.needed.search(library):
            violations.append(f"{binary.name}: NEEDED entry {library}")

    symbols = defined_symbols(binary)
    for family, pattern in rule.symbols.items():
        hits = [s for s in symbols if pattern.search(s)]
        if hits:
            sample = "; ".join(s.split(maxsplit=2)[-1][:100] for s in hits[:SAMPLE_LIMIT])
            violations.append(f"{binary.name}: {len(hits)} defined {family} symbols, e.g. {sample}")

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", nargs="?", type=Path, default=ROOT / "build",
                        help="directory holding libddb.so, ddbcmd, ddb-nexus and libnxs.so "
                             "(default: build)")
    args = parser.parse_args()

    failed: list[str] = []
    try:
        for rule in RULES:
            violations: list[str] = []
            for name in rule.files:
                binary = args.build_dir / name
                if not binary.is_file():
                    raise ToolError(f"{binary} does not exist")
                violations += check_binary(binary, rule)
            for violation in violations:
                print(f"FAIL: {violation}")
            if violations:
                failed.append(rule.failure)
            else:
                print(f"PASS: {', '.join(rule.files)}")
    except ToolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    for failure in failed:
        print(f"FAIL: {failure}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
