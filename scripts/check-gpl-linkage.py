#!/usr/bin/env python3
"""GPL isolation guard: libddb and the ddb CLI must not contain or load Exiv2, bm3d or libbm3d, and
the bm3d helper must stay free of Qt.

Exiv2 (GPL-2.0-or-later) is used only by the separate `metadata-util` executable, and bm3d (with
the Nexus, VCGLib and corto code it is derived from) only by the separate `bm3d` executable, a
static program that loads no library of its own. libddb talks to both as child processes, so a
build whose libddb.so or ddbcmd carries their code again would link GPL code into the library.
Qt is the other way round: bm3d decodes textures with plain image codec libraries and does not
use Qt, so Qt code in bm3d (or in libddb/ddbcmd) would bring back a dependency, and with it the
display libraries (X11, xcb, GL, dbus) the Debian package had to declare for it, that nothing
else needs. This check looks at the files that ship:

  symbols   `nm -C --defined-only` lists every symbol, not only the exported ones. The version
            script hides most of libddb's exports, so `nm -D` would be blind to linked-in code
            that is merely hidden. Fails on a defined symbol in Exiv2::, bm3d::, vcg::, crt:: or
            a Qt class (Q[A-Z]...::). ddbcmd embeds its objects, so it gets the same check.
            bm3d is checked for Qt classes only: the other code is its own.
  needed    `readelf -d` NEEDED entries. libddb and ddbcmd fail on libbm3d, libexiv2 or libQt5;
            bm3d fails on Qt and on the display libraries Qt pulled in.
  control   A check that finds nothing proves nothing if its pattern no longer matches what it
            looks for (a renamed namespace would make every run pass). So the symbols of the
            `bm3d` binary, which does contain that code, must match the pattern used on libddb,
            for each of bm3d::, vcg:: and crt:: separately; a family with no symbol fails the
            check. `--self-test` runs the patterns and the control against made-up `nm` output
            (no build needed), so the control itself is tested.

The binaries need their symbol tables for this: a stripped file would pass without proof, so it
is reported as an error instead.

Linux ELF only. The Windows counterpart is scripts/check-gpl-linkage.ps1, which checks the DLLs
the same files import (`dumpbin /dependents`) and has no symbol check.

Usage:
    scripts/check-gpl-linkage.py [BUILD_DIR]       BUILD_DIR defaults to ./build
    scripts/check-gpl-linkage.py --self-test       no build directory needed

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
# The namespaces of the code bm3d is made of. The pattern needs the space before the name: libddb
# has a namespace of its own for the code that starts bm3d (ddb::bm3d), which must not count as
# bm3d code.
BM3D_NAMESPACES = ("bm3d", "vcg", "crt")
EXIV2_SYMBOLS = re.compile(r"Exiv2::")
BM3D_SYMBOLS = re.compile(r" (" + "|".join(BM3D_NAMESPACES) + r")::")
QT_SYMBOLS = re.compile(r" Q[A-Z][A-Za-z]+::")
NEEDED_LINE = re.compile(r"\(NEEDED\).*\[(.+)\]")


@dataclass(frozen=True)
class Rule:
    """What the files of one group must not contain: NEEDED entries and defined symbols."""
    files: tuple[str, ...]
    needed: re.Pattern[str]
    symbols: dict[str, re.Pattern[str]]
    failure: str
    # Pattern whose every family (see positive_control) must be found in these files.
    control: re.Pattern[str] | None = None


RULES = (
    Rule(files=("libddb.so", "ddbcmd"),
         needed=re.compile(r"libbm3d|libexiv2|libQt5"),
         symbols={"Exiv2": EXIV2_SYMBOLS, "bm3d/vcglib/corto": BM3D_SYMBOLS, "Qt": QT_SYMBOLS},
         failure="GPL code is linked into libddb or ddbcmd; Exiv2 belongs to metadata-util, and "
                 "bm3d with its vcglib and corto code to the bm3d program"),
    # Qt is statically linked on Linux when it is used at all, so the symbol check is the one
    # that proves its absence; the NEEDED check also catches the display libraries it loads.
    Rule(files=("bm3d",),
         needed=re.compile(r"^(?:libQt\d|libX11|libxcb|libxkbcommon|libGL\.|libEGL\.|libdbus)"),
         symbols={"Qt": QT_SYMBOLS},
         failure="Qt is back in bm3d, which decodes textures with image codec libraries only",
         control=BM3D_SYMBOLS),
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


def positive_control(symbols: list[str], pattern: re.Pattern[str], name: str) -> list[str]:
    """Returns a message for each family (the first group of `pattern`) with no symbol in `symbols`.

    `pattern` is the one the libddb check uses, so a change that stops it matching the code it is
    meant to find fails here, in the binary that contains that code, instead of passing on
    libddb because it finds nothing."""
    found = {m.group(1) for s in symbols if (m := pattern.search(s))}
    return [f"{name}: no defined {family}:: symbol matches the pattern that looks for GPL code "
            f"in libddb, so that check would prove nothing"
            for family in BM3D_NAMESPACES if family not in found]


def check_binary(binary: Path, rule: Rule) -> list[str]:
    """Returns one message per violation of `rule` found in `binary`."""
    violations = []

    for library in needed_libraries(binary):
        if rule.needed.search(library):
            violations.append(f"{binary.name}: NEEDED entry {library}")

    symbols = defined_symbols(binary)
    if rule.control is not None:
        violations += positive_control(symbols, rule.control, binary.name)
    for family, pattern in rule.symbols.items():
        hits = [s for s in symbols if pattern.search(s)]
        if hits:
            sample = "; ".join(s.split(maxsplit=2)[-1][:100] for s in hits[:SAMPLE_LIMIT])
            violations.append(f"{binary.name}: {len(hits)} defined {family} symbols, e.g. {sample}")

    return violations


def self_test() -> int:
    """Checks the patterns and the control against made-up nm lines; returns an exit code."""
    code = {"bm3d": "0000000000001000 T bm3d::Builder::run()",
            "vcg": "0000000000002000 W vcg::tri::Allocator<MyMesh>::AddVertices()",
            "crt": "0000000000003000 T crt::Encoder::encodeMesh()"}
    failures = []

    def expect(label: str, condition: bool) -> None:
        if not condition:
            failures.append(label)

    for family, line in code.items():
        expect(f"{family}:: symbol is matched", BM3D_SYMBOLS.search(line) is not None)
    expect("libddb's own ddb::bm3d runner is not bm3d code",
           BM3D_SYMBOLS.search("0000000000004000 T ddb::bm3d::findBm3dBinary()") is None)
    expect("the old nx:: namespace is not matched", BM3D_SYMBOLS.search(" T nx::NexusBuilder") is None)
    expect("a Qt class is matched", QT_SYMBOLS.search("0000000000005000 T QString::QString()"))
    expect("an Exiv2 symbol is matched", EXIV2_SYMBOLS.search("0000000000006000 T Exiv2::Image::readMetadata()"))

    everything = list(code.values()) + ["0000000000007000 T main"]
    expect("a binary with all three families passes",
           positive_control(everything, BM3D_SYMBOLS, "bm3d") == [])
    for missing in code:
        without = [line for family, line in code.items() if family != missing]
        messages = positive_control(without, BM3D_SYMBOLS, "bm3d")
        expect(f"a binary without {missing}:: fails the control",
               len(messages) == 1 and f"{missing}::" in messages[0])
    expect("a binary with no matching symbol fails three times",
           len(positive_control(["0000000000007000 T main"], BM3D_SYMBOLS, "bm3d")) == 3)
    expect("a pattern that no longer matches fails the control",
           len(positive_control(everything, re.compile(r" (nx|vcg|crt)::"), "bm3d")) == 1)

    for label in failures:
        print(f"FAIL: self-test: {label}")
    if not failures:
        print("PASS: self-test")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", nargs="?", type=Path, default=ROOT / "build",
                        help="directory holding libddb.so, ddbcmd and bm3d (default: build)")
    parser.add_argument("--self-test", action="store_true",
                        help="test the patterns and the positive control on made-up symbols, "
                             "without a build directory")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

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
