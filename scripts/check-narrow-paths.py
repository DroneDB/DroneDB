#!/usr/bin/env python3
"""Narrow path conversion ratchet: a change may not add a raw std::string <-> fs::path conversion,
or an ANSI Win32 call, to libddb or the CLI.

Inside libddb a std::string path is UTF-8 (src/include/mio.h, "Path encoding"). It becomes an
fs::path only through io::fromUtf8, and a path becomes a string only through io::toUtf8 or
io::toGenericUtf8. MSVC converts fs::path(std::string), path.string() and path.generic_string()
with the ANSI code page (or the CRT locale), so on Windows they mangle a non-ASCII path, while the
Linux build behaves the same either way. This check counts what a text search can see:

  string      .string() and .generic_string() calls (also through ->). io::Path::string() is
              UTF-8, but a text search cannot tell the receivers apart: those calls are in the
              baseline.
  path-ctor   fs::path(<expression>) and std::filesystem::path(<expression>), other than
              fs::path() and fs::path("literal"). Copies of a path are in the baseline.
  ansi-win32  Win32 functions that take ANSI strings: the *A names, and the TCHAR names that
              resolve to them in this build (no UNICODE define).

Implicit conversions (fs::exists(str), dir / str, std::ifstream(str), path == str) are invisible
to a text search; reviews and the Windows tests with non-ASCII paths (Utf8Paths.*) cover them.

Checked: src/library, src/include and apps (.cpp, .h, .hpp). Comments and string literals are
ignored. src/library/path_encoding.cpp implements the conversions and is not checked.

The baseline (cmake/narrow-paths-baseline.txt) has one line per file and check:

    <path from the repository root> <check> <count>

A count that grows, or a new line, fails. A count that drops passes with a reminder to run
--update, so the ratchet keeps the gain.

Usage:
    scripts/check-narrow-paths.py                  compare the sources with the baseline
    scripts/check-narrow-paths.py --update         write the current counts to the baseline
    scripts/check-narrow-paths.py --list [PATH..]  print every occurrence (of the given files)

Exit codes: 0 ok, 1 a count grew, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = Path(__file__).resolve().relative_to(ROOT).as_posix()
DEFAULT_BASELINE = ROOT / "cmake" / "narrow-paths-baseline.txt"
SOURCE_DIRS = ("src/library", "src/include", "apps")
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp"}
EXCLUDED = {"src/library/path_encoding.cpp"}

ANSI_WIN32 = (
    # *A functions that take or return file names, paths or text
    "CopyFileA|CreateDirectoryA|CreateFileA|CreateProcessA|DeleteFileA|FindFirstFileA|"
    "FindNextFileA|FormatMessageA|GetCurrentDirectoryA|GetEnvironmentVariableA|"
    "GetFileAttributesA|GetFileAttributesExA|GetFullPathNameA|GetLongPathNameA|"
    "GetModuleFileNameA|GetModuleHandleA|GetModuleHandleExA|GetShortPathNameA|GetTempPathA|"
    "LoadLibraryA|LoadLibraryExA|MoveFileA|MoveFileExA|ReadConsoleA|RemoveDirectoryA|"
    "ReplaceFileA|SetCurrentDirectoryA|SetEnvironmentVariableA|SetFileAttributesA|ShellExecuteA|"
    # TCHAR names: the A variants without UNICODE
    "CopyFile|CreateDirectory|CreateFile|CreateProcess|DeleteFile|FindFirstFile|FindNextFile|"
    "FormatMessage|GetCurrentDirectory|GetEnvironmentVariable|GetFileAttributes|"
    "GetFileAttributesEx|GetFullPathName|GetModuleFileName|GetModuleHandle|GetModuleHandleEx|"
    "GetTempPath|LoadLibrary|LoadLibraryEx|MoveFile|MoveFileEx|RemoveDirectory|"
    "SetCurrentDirectory|SetEnvironmentVariable|SetFileAttributes|ShellExecute"
)

CHECKS: dict[str, re.Pattern[str]] = {
    "string": re.compile(r"(?:\.|->)(?:generic_)?string\s*\(\s*\)"),
    "path-ctor": re.compile(r"\b(?:fs|std::filesystem)::path\s*\((?!\s*\))"),
    "ansi-win32": re.compile(r"(?<![\w:])(?:::)?(?:" + ANSI_WIN32 + r")\s*\("),
}

# fs::path("cog"): a literal is ASCII here, and strip_code empties it to ""
LITERAL_ARGUMENT = re.compile(r'\s*(?:u8|L|u|U)?""\s*\)')

Key = tuple[str, str]  # (path, check)


def strip_code(text: str) -> str:
    """text with comments removed and string/character literals emptied ("", ''), so the
    checks only see code. Newlines are kept, so line numbers do not change."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif c == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            # raw string R"delim( ... )delim"
            open_paren = text.find("(", i + 2)
            delimiter = text[i + 2:open_paren]
            end = text.find(")" + delimiter + '"', open_paren)
            end = n if end < 0 else end + len(delimiter) + 2
            out.append('""' + "\n" * text.count("\n", i, end))
            i = end
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            # string or character literal ('\'' handled; 1'000 digit separators are skipped)
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def occurrences(code: str) -> list[tuple[str, int, str]]:
    """[(check, line, source line)] of the stripped code of one file."""
    found = []
    lines = code.split("\n")
    for number, line in enumerate(lines, 1):
        for check, pattern in CHECKS.items():
            for match in pattern.finditer(line):
                if check == "path-ctor" and LITERAL_ARGUMENT.match(line, match.end()):
                    continue
                found.append((check, number, line.strip()))
    return found


def source_files(only: list[str] | None = None) -> list[Path]:
    files = []
    for directory in SOURCE_DIRS:
        for path in sorted((ROOT / directory).rglob("*")):
            rel = path.relative_to(ROOT).as_posix()
            if path.suffix in SOURCE_SUFFIXES and path.is_file() and rel not in EXCLUDED:
                if only is None or rel in only:
                    files.append(path)
    return files


def scan(only: list[str] | None = None) -> dict[Path, list[tuple[str, int, str]]]:
    result = {}
    for path in source_files(only):
        code = strip_code(path.read_text(encoding="utf-8", errors="replace"))
        found = occurrences(code)
        if found:
            result[path] = found
    return result


def counts_of(found: dict[Path, list[tuple[str, int, str]]]) -> dict[Key, int]:
    counts: dict[Key, int] = {}
    for path, items in found.items():
        rel = path.relative_to(ROOT).as_posix()
        for check, _, _ in items:
            counts[(rel, check)] = counts.get((rel, check), 0) + 1
    return counts


def read_baseline(path: Path) -> dict[Key, int]:
    counts: dict[Key, int] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if len(fields) != 3 or fields[1] not in CHECKS or not fields[2].isdigit():
            raise SystemExit(f"error: {path}:{number}: expected '<path> <check> <count>'")
        counts[(fields[0], fields[1])] = int(fields[2])
    return counts


def write_baseline(path: Path, counts: dict[Key, int]) -> None:
    header = [
        f"# Narrow path conversion baseline for {SCRIPT} (UTF-8 paths, src/include/mio.h).",
        f"# Do not edit by hand: after removing conversions, run {SCRIPT} --update and commit this file.",
        "# Format: <path> <check> <count>   (checks: " + ", ".join(CHECKS) + ")",
    ]
    body = [f"{p} {check} {count}" for (p, check), count in sorted(counts.items())]
    path.write_text("\n".join(header + body) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--update", action="store_true", help="write the current counts to the baseline")
    group.add_argument("--list", nargs="*", metavar="PATH", help="print every occurrence")
    args = parser.parse_args()

    if args.list is not None:
        found = scan(args.list or None)
        for path, items in found.items():
            rel = path.relative_to(ROOT).as_posix()
            for check, number, line in items:
                print(f"{rel}:{number}: [{check}] {line}")
        return 0

    current = counts_of(scan())
    if args.update:
        write_baseline(args.baseline, current)
        print(f"Wrote {sum(current.values())} occurrences in {len(current)} file/check entries "
              f"to {args.baseline.relative_to(ROOT).as_posix()}")
        return 0

    if not args.baseline.is_file():
        print(f"error: {args.baseline} does not exist (run {SCRIPT} --update)", file=sys.stderr)
        return 2
    baseline = read_baseline(args.baseline)

    grown = sorted(k for k in current if current[k] > baseline.get(k, 0))
    dropped = sorted(k for k in baseline if current.get(k, 0) < baseline[k])
    for rel, check in grown:
        print(f"error: {rel}: {check} {baseline.get((rel, check), 0)} -> {current[(rel, check)]}")
    if grown:
        print(f"\n{len(grown)} file/check entries grew. Convert std::string paths with io::fromUtf8 and "
              "paths with io::toUtf8 / io::toGenericUtf8, and call the W variant of a Win32 function "
              f"(src/include/mio.h, \"Path encoding\"). '{SCRIPT} --list <path>' shows the "
              "occurrences; when a new one is right (a copy of an fs::path, io::Path::string()), "
              f"run '{SCRIPT} --update' and commit the baseline.")
        return 1
    total_current, total_baseline = sum(current.values()), sum(baseline.values())
    if dropped:
        print(f"OK: {total_current} occurrences, {total_baseline - total_current} fewer than the baseline "
              f"({len(dropped)} entries dropped): run '{SCRIPT} --update' to keep the gain.")
    else:
        print(f"OK: {total_current} occurrences, same as the baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
