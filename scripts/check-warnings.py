#!/usr/bin/env python3
"""Compiler warning ratchet: a change may not add a warning to our code.

The baseline (cmake/warnings-baseline.txt) has one line per file and warning flag:

    <path from the repository root> <flag> <count>

<count> is the number of distinct locations (line:column) of that flag in that file. Line
numbers are not stored, so an edit that only moves code does not change the baseline, while a
new warning raises a count (or adds a line) and fails the check. A header included by many
translation units counts each location once.

Only our code counts: src/, apps/, tests/ and the generated sources of the build directory
(<build>/generated/, i.e. version.cpp). vendor/, vcpkg and system headers are ignored; the CMake
targets also include third-party directories as SYSTEM, so most of their warnings never appear.

The check also fails when the total drops by more than --max-drop warnings or --max-drop-percent
percent without a baseline update: a large unexplained drop usually means the log does not cover
a full build (an incremental build, a failed target, another compiler), not that warnings were
fixed. Smaller drops pass with a reminder to run --update, so the ratchet keeps the gain.

The baseline is measured with the compiler of the CI Linux job (GCC 13 on ubuntu-24.04, recorded
in its header). Another compiler or major version reports another set: GCC 15 misses some
-Wmaybe-uninitialized warnings that GCC 13 gives at -O3, so a local build with a newer GCC can
pass and CI fail. --compile checks with the baseline's compiler whatever compiler the build
directory uses: it compiles every translation unit of the build directory's
compile_commands.json with that compiler (GNU 13: gcc-13 and g++-13, which must be installed),
with the same flags, and discards the objects. The build directory itself is not changed and
needs no rebuild of its dependencies; it must be configured, and its generated sources built.
When --build-dir or --build points to a build with another compiler, the check notes it.
GCC/Clang logs ("file:line:col: warning: ... [-Wflag]") and MSVC logs
("file(line,col): warning C4244: ...") are both parsed, but CI ratchets the Linux build only:
Windows warnings are reported in its build log, not ratcheted, until a Windows baseline exists.

ccache replays the compiler's stderr on a cache hit, so a build through ccache still prints every
warning; an incremental build does not (unchanged objects are not compiled at all). Feed the log
of a complete build: a fresh build directory, or --build, which rebuilds with --clean-first.

Usage:
    scripts/check-warnings.py <build-log>               compare a full build log with the baseline
    scripts/check-warnings.py --build [<build-dir>]     clean-rebuild <build-dir> (default: build)
    scripts/check-warnings.py --compile [<build-dir>]   compile <build-dir> with the CI compiler
    scripts/check-warnings.py --update <build-log>      write the log's warnings to the baseline
    scripts/check-warnings.py --summary <build-log>     print the counts per flag, compare nothing

Build with LC_ALL=C so the compiler does not translate "warning:".
Exit codes: 0 ok, 1 new warnings or an unexplained drop, 2 usage error or failed build.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import json
import math
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = Path(__file__).resolve().relative_to(ROOT).as_posix()
DEFAULT_BASELINE = ROOT / "cmake" / "warnings-baseline.txt"
DEFAULT_BUILD_DIR = ROOT / "build"
PROJECT_DIRS = ("src/", "apps/", "tests/")
GENERATED_PREFIX = "<build>/generated/"
THIRD_PARTY_PARTS = {"vendor", "vcpkg", "vcpkg_installed", "_deps"}

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# GCC/Clang: path:line[:col]: warning: message [-Wflag] (clang may print [-Werror,-Wflag])
GNU_WARNING = re.compile(
    r"^(?P<path>[^:\s][^:]*?|[A-Za-z]:[\\/][^:]*?):(?P<line>\d+)(?::(?P<col>\d+))?:"
    r" warning: (?P<message>.*?)(?: \[(?P<flag>-W[^\]]*)\])?\s*$"
)
# MSVC/MSBuild: [N>]path(line[,col]): warning C1234: message [project]
MSVC_WARNING = re.compile(
    r"^\s*(?:\d+>)?(?P<path>.+?)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:"
    r" warning (?P<flag>[A-Z]+\d+)\s*:"
)
BASELINE_COMPILER = re.compile(r"^#\s*Compiler:\s*(?P<id>\S+)\s+(?P<major>\d+)", re.IGNORECASE)

Location = tuple[str, str]  # (line, column)


def project_key(raw_path: str, build_dir: Path | None = None) -> str | None:
    """Maps a path from the log to its baseline key, or None when it is not our code."""
    path = raw_path.strip().replace("\\", "/")
    root = ROOT.as_posix()
    windows = re.match(r"^[A-Za-z]:/", path) is not None
    if not posixpath.isabs(path) and not windows:
        path = posixpath.join(root, path)
    path = posixpath.normpath(path)
    # A build directory outside the repository: only its generated sources are ours
    build = build_dir.resolve().as_posix() if build_dir else None
    if build and not build.startswith(root + "/") and path.startswith(build + "/generated/"):
        return GENERATED_PREFIX + path[len(build + "/generated/"):]
    if windows:
        if not path.lower().startswith(root.lower() + "/"):
            return None
    elif not path.startswith(root + "/"):
        return None
    rel = path[len(root) + 1:]
    if rel.startswith(PROJECT_DIRS):
        return rel
    parts = rel.split("/")
    if THIRD_PARTY_PARTS.intersection(parts):
        return None
    # <build-dir>/generated/<file>: any build directory name, so local and CI keys agree
    if "generated" in parts[1:-1]:
        index = parts.index("generated", 1)
        return GENERATED_PREFIX + "/".join(parts[index + 1:])
    return None


def parse_log(text: str, build_dir: Path | None = None) -> dict[tuple[str, str], set[Location]]:
    """Returns {(path, flag): {(line, column)}} for the warnings of our code in a build log."""
    found: dict[tuple[str, str], set[Location]] = collections.defaultdict(set)
    for raw_line in text.splitlines():
        line = ANSI_ESCAPE.sub("", raw_line)
        if "warning" not in line:
            continue
        match = GNU_WARNING.match(line)
        if match:
            flag = match.group("flag") or "-W(no-flag)"
            flag = flag.split(",")[-1]  # [-Werror,-Wfoo] -> -Wfoo
        else:
            match = MSVC_WARNING.match(line)
            if not match:
                continue
            flag = match.group("flag")
        key = project_key(match.group("path"), build_dir)
        if key is None:
            continue
        found[(key, flag)].add((match.group("line"), match.group("col") or "0"))
    return found


def counts_of(found: dict[tuple[str, str], set[Location]]) -> dict[tuple[str, str], int]:
    return {key: len(locations) for key, locations in found.items()}


def read_baseline(path: Path) -> tuple[dict[tuple[str, str], int], str | None]:
    counts: dict[tuple[str, str], int] = {}
    compiler = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            match = BASELINE_COMPILER.match(stripped)
            if match:
                compiler = f"{match.group('id')} {match.group('major')}"
            continue
        fields = stripped.split()
        if len(fields) != 3 or not fields[2].isdigit():
            raise SystemExit(f"error: {path}:{number}: expected '<path> <flag> <count>'")
        counts[(fields[0], fields[1])] = int(fields[2])
    return counts, compiler


def recorded_compiler(path: Path) -> str | None:
    """'<id> <major>' recorded in the header of the baseline, if any."""
    return read_baseline(path)[1] if path.is_file() else None


def compiler_commands(compiler: str) -> tuple[str, str]:
    """The C and C++ compiler commands of '<id> <major>' as distributions install them."""
    compiler_id, major = compiler.split()
    names = {"GNU": ("gcc", "g++"), "Clang": ("clang", "clang++")}
    if compiler_id not in names:
        raise SystemExit(f"error: no compiler commands known for {compiler}; pass --cc and --cxx")
    return f"{names[compiler_id][0]}-{major}", f"{names[compiler_id][1]}-{major}"


def build_compiler(build_dir: Path) -> str | None:
    """'<CMAKE_CXX_COMPILER_ID> <major>' of a configured CMake build directory, if any."""
    for cmake_file in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        text = cmake_file.read_text(encoding="utf-8", errors="replace")
        compiler_id = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text)
        version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "(\d+)', text)
        if compiler_id and version:
            return f"{compiler_id.group(1)} {version.group(1)}"
    return None


def per_flag_summary(counts: dict[tuple[str, str], int]) -> str:
    per_flag: collections.Counter[str] = collections.Counter()
    for (_, flag), count in counts.items():
        per_flag[flag] += count
    lines = [f"{sum(counts.values())} unique warnings in {len(counts)} file/flag entries:"]
    lines += [f"  {flag:<32} {count}" for flag, count in sorted(per_flag.items(), key=lambda i: (-i[1], i[0]))]
    return "\n".join(lines)


def display(path: Path) -> str:
    """path relative to the repository when it is inside it."""
    resolved = path.resolve()
    return resolved.relative_to(ROOT).as_posix() if resolved.is_relative_to(ROOT) else str(path)


def write_baseline(path: Path, counts: dict[tuple[str, str], int], compiler: str | None) -> None:
    header = [
        f"# Warning baseline for {SCRIPT}. Do not edit by hand: after removing",
        f"# warnings, run {SCRIPT} --update <log of a full build> and commit this file.",
        f"# Compiler: {compiler or 'unknown'} (the CI build_ubuntu job measures with this compiler)",
        "# Format: <path> <flag> <number of distinct line:column locations>",
    ]
    body = [f"{p} {flag} {count}" for (p, flag), count in sorted(counts.items())]
    path.write_text("\n".join(header + body) + "\n", encoding="utf-8")


def run_build(build_dir: Path) -> str:
    """Rebuilds every target of build_dir from scratch and returns the build output."""
    if not (build_dir / "CMakeCache.txt").is_file():
        raise SystemExit(f"error: {build_dir} is not a configured CMake build directory")
    env = dict(os.environ, LC_ALL="C")
    command = ["cmake", "--build", str(build_dir), "--clean-first", "--parallel", str(os.cpu_count() or 2)]
    print("Running: " + " ".join(command), flush=True)
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", check=False)
    log = build_dir / "warnings-build.log"
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        print("\n".join(result.stdout.splitlines()[-50:]), file=sys.stderr)
        print(f"error: the build failed (full log: {log})", file=sys.stderr)
        sys.exit(2)
    print(f"Build log: {log}")
    return result.stdout


def compile_unit(entry: dict, cc: str, cxx: str, env: dict[str, str]) -> tuple[str, int]:
    """Compiles one compile_commands.json entry with cc or cxx, the object discarded."""
    args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    is_c = Path(entry["file"]).suffix == ".c"
    args = [cc if is_c else cxx] + args[1:]
    if "-o" in args:
        args[args.index("-o") + 1] = os.devnull
    result = subprocess.run(args, cwd=entry["directory"], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", check=False)
    output = result.stdout
    if result.returncode != 0:
        output += f"error: {entry['file']}: the compiler exited with {result.returncode}\n"
    return output, result.returncode


def run_compile(build_dir: Path, cc: str, cxx: str) -> str:
    """Compiles every translation unit of build_dir with cc and cxx and returns their output."""
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(f"error: no {database}: configure {build_dir} with CMake first")
    for command in (cc, cxx):
        if not shutil.which(command):
            raise SystemExit(f"error: {command} not found (on Ubuntu: apt install {command})")
    entries = json.loads(database.read_text(encoding="utf-8"))
    env = dict(os.environ, LC_ALL="C")
    jobs = os.cpu_count() or 2
    print(f"Compiling {len(entries)} translation units of {display(build_dir)} with {cc} and {cxx} "
          f"({jobs} jobs, objects discarded)", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda entry: compile_unit(entry, cc, cxx, env), entries))
    # Each unit's output whole and in the order of the database: no interleaved diagnostics
    text = "".join(output for output, _ in results)
    log = build_dir / f"warnings-{Path(cxx).name}.log"
    log.write_text(text, encoding="utf-8")
    failed = sum(1 for _, code in results if code != 0)
    if failed:
        errors = [line for line in text.splitlines() if " error: " in line or line.startswith("error: ")]
        print("\n".join(errors[:50]), file=sys.stderr)
        print(f"error: {failed} translation units do not compile with {cxx} (full log: {log})", file=sys.stderr)
        sys.exit(2)
    print(f"Compile log: {log}")
    return text


def describe(entries: list[tuple[tuple[str, str], int, int]]) -> list[str]:
    return [f"  {p} {flag} {before} -> {after}" for (p, flag), before, after in entries]


def compare(baseline: dict[tuple[str, str], int], found: dict[tuple[str, str], set[Location]],
            max_drop: int, max_drop_percent: float, update_hint: str) -> int:
    current = counts_of(found)
    keys = sorted(set(baseline) | set(current))
    added = [(k, baseline.get(k, 0), current.get(k, 0)) for k in keys if current.get(k, 0) > baseline.get(k, 0)]
    removed = [(k, baseline.get(k, 0), current.get(k, 0)) for k in keys if current.get(k, 0) < baseline.get(k, 0)]
    total_before, total_now = sum(baseline.values()), sum(current.values())
    allowed_drop = min(max_drop, math.floor(total_before * max_drop_percent / 100))
    in_ci = bool(os.environ.get("GITHUB_ACTIONS"))
    status = 0

    if added:
        print("error: new warnings (<path> <flag> <baseline> -> <now>):", file=sys.stderr)
        print("\n".join(describe(added)), file=sys.stderr)
        print("Locations of those flags in those files (the new ones are among them):", file=sys.stderr)
        for (p, flag), before, after in added:
            for line, col in sorted(found[(p, flag)], key=lambda loc: (int(loc[0]), int(loc[1]))):
                print(f"  {p}:{line}:{col}: {flag}", file=sys.stderr)
                if in_ci and before == 0:  # a new file/flag entry: every location is new
                    print(f"::error file={p},line={line},col={col},title=New compiler warning::{flag}")
            if in_ci and before > 0:
                print(f"::error file={p},title=New compiler warning::{flag}: {before} -> {after} locations")
        print("Fix them; the full messages are in the build log.", file=sys.stderr)
        status = 1

    if total_before - total_now > allowed_drop:
        print(f"error: {total_before - total_now} fewer warnings than the baseline ({total_before} -> "
              f"{total_now}), more than the {allowed_drop} allowed without a baseline update.", file=sys.stderr)
        print("Check that the log covers a full build with the baseline's compiler (a fresh build "
              "directory or --build). If the warnings were really fixed: " + update_hint, file=sys.stderr)
        if removed:
            print("Changed entries (<path> <flag> <baseline> -> <now>):", file=sys.stderr)
            print("\n".join(describe(removed)), file=sys.stderr)
        status = 1
    elif removed:
        print("Fewer warnings than the baseline (<path> <flag> <baseline> -> <now>):")
        print("\n".join(describe(removed)))
        print(update_hint)
        if in_ci:
            print("::warning title=Warning baseline::Warnings were removed. " + update_hint)

    if status == 0:
        print(f"No new warnings: {total_now} unique warnings in {len(current)} file/flag entries "
              f"(baseline {total_before}).")
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("log", nargs="?", type=Path, help="log of a full build")
    source.add_argument("--build", nargs="?", const=DEFAULT_BUILD_DIR, type=Path, metavar="BUILD_DIR",
                        help="clean-rebuild BUILD_DIR (default: build) and check its output")
    source.add_argument("--compile", nargs="?", const=DEFAULT_BUILD_DIR, type=Path, metavar="BUILD_DIR",
                        help="compile every translation unit of BUILD_DIR (default: build) with the "
                             "baseline's compiler, objects discarded, and check its output")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--update", action="store_true", help="write the warnings to the baseline")
    mode.add_argument("--summary", action="store_true", help="print the counts per flag only")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--build-dir", type=Path,
                        help="CMake build directory of the log: its compiler is checked against the "
                             "baseline and recorded by --update (default: build)")
    parser.add_argument("--cc", help="C compiler of --compile (default: the baseline's, e.g. gcc-13)")
    parser.add_argument("--cxx", help="C++ compiler of --compile (default: the baseline's, e.g. g++-13)")
    parser.add_argument("--max-drop", type=int, default=50,
                        help="largest drop of the total accepted without a baseline update (default 50)")
    parser.add_argument("--max-drop-percent", type=float, default=10.0,
                        help="same, as a percentage of the baseline total (default 10); the smaller wins")
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep stdout and stderr in order in CI logs

    build_dir = args.build or args.compile or args.build_dir or DEFAULT_BUILD_DIR
    if args.compile:
        compiler = recorded_compiler(args.baseline)
        if args.cc or args.cxx:
            if not (args.cc and args.cxx):
                raise SystemExit("error: pass both --cc and --cxx")
            cc, cxx, compiler = args.cc, args.cxx, None  # their id and major are not known
        elif compiler:
            cc, cxx = compiler_commands(compiler)
        else:
            raise SystemExit(f"error: {display(args.baseline)} records no compiler; pass --cc and --cxx")
        text = run_compile(build_dir, cc, cxx)
    elif args.build:
        text = run_build(build_dir)
        compiler = build_compiler(build_dir)
    else:
        text = args.log.read_text(encoding="utf-8", errors="replace")
        compiler = build_compiler(build_dir)
    found = parse_log(text, build_dir)
    counts = counts_of(found)
    print(per_flag_summary(counts))

    if args.summary:
        return 0
    if args.update:
        write_baseline(args.baseline, counts, compiler)
        print(f"Baseline updated: {display(args.baseline)} ({compiler or 'unknown compiler'}).")
        return 0

    if not args.baseline.is_file():
        print(f"error: no baseline at {args.baseline}; create it with --update", file=sys.stderr)
        return 2
    baseline, baseline_compiler = read_baseline(args.baseline)
    if compiler and baseline_compiler and compiler != baseline_compiler:
        print(f"note: the baseline was measured with {baseline_compiler}, this build uses {compiler}; another "
              "compiler may report other warnings. CI (the build_ubuntu job) is the reference.")
    if not compiler:
        print("note: compiler of the log unknown, compiler check skipped")
    update_hint = (f"Run {SCRIPT} --update <build log> --build-dir <its build dir> and commit "
                   f"{display(args.baseline)}, so the ratchet keeps the gain.")
    return compare(baseline, found, args.max_drop, args.max_drop_percent, update_hint)


if __name__ == "__main__":
    sys.exit(main())
