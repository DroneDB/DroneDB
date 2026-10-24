#!/usr/bin/env python3
"""clang-format and clang-tidy on the lines a change touches only: no mass reformat.

The change is "git diff <merge-base of BASE and HEAD>" (committed and uncommitted changes),
restricted to src/, apps/ and tests/.

    format  clang-format-diff with the repository's .clang-format. Fails (exit 1) when a changed
            line is not formatted as .clang-format says, and prints the command that fixes it.
    tidy    clang-tidy-diff with the repository's .clang-tidy and BUILD_DIR/compile_commands.json,
            on the changed lines of .c/.cpp files (headers have no compile command of their own).
            Report-only: the findings are printed (and annotated in GitHub Actions) but the exit
            code is 0, unless --strict. The checks have not been tuned against the codebase yet,
            and a touched line often carries an older pattern the change did not introduce; the
            step becomes failing once the findings on real PRs show the check set is right.

Usage:
    scripts/lint-changed-lines.py format [--base REV] [--report-only]
    scripts/lint-changed-lines.py tidy [--base REV] [--build-dir DIR] [--strict]

--base defaults to origin/master. An unknown or all-zero base (the "before" of a new branch or a
force push) falls back to HEAD~1, so the commit itself is still linted.

Tools: the newest clang-format-diff / clang-tidy-diff script found on PATH (also as
clang-format-diff-N, clang-tidy-diff-N.py) or under /usr/lib/llvm-N/share/clang and
/usr/share/clang/clang-format-N, run with the clang-format-N / clang-tidy-N binary of the same
version. Override with the CLANG_FORMAT_DIFF, CLANG_FORMAT, CLANG_TIDY_DIFF and CLANG_TIDY
environment variables. Exit codes: 0 ok (or tidy findings without --strict), 1 findings,
2 missing tool, bad base or missing compile_commands.json.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATHS = ("src", "apps", "tests")
FORMAT_REGEX = r"(src|apps|tests)/.*\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl)"
TIDY_REGEX = r"(src|apps|tests)/.*\.(c|cc|cpp|cxx)"
TIDY_FINDING = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+): (?P<level>warning|error): "
                          r"(?P<message>.*?)(?: \[(?P<check>[^\]]+)\])?$")
IN_CI = bool(os.environ.get("GITHUB_ACTIONS"))


def fail(message: str, code: int = 2) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(code)


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    if result.returncode != 0:
        fail(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def resolve_base(base: str) -> str:
    """The merge base of BASE and HEAD, falling back to HEAD~1 for a missing base."""
    def exists(rev: str) -> bool:
        return subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"], cwd=ROOT,
                              stdout=subprocess.DEVNULL, check=False).returncode == 0

    if not base or set(base) == {"0"} or not exists(base):
        print(f"note: base '{base}' is not a known commit; linting the last commit (HEAD~1..HEAD)")
        base = "HEAD~1"
        if not exists(base):
            fail("HEAD~1 does not exist: fetch more history (actions/checkout fetch-depth: 0)")
    return git("merge-base", base, "HEAD").strip()


def changed_lines_diff(merge_base: str) -> str:
    return git("diff", "-U0", "--no-color", "--no-ext-diff", "--diff-filter=ACMR", merge_base, "--", *PATHS)


def version_of(path: str) -> int:
    """Major version in a tool path (clang-format-diff-18, llvm-18/...), 0 when unversioned."""
    numbers = re.findall(r"(?:-|llvm-)(\d+)", path)
    return int(numbers[-1]) if numbers else 0


def find_script(env_name: str, name: str, patterns: list[str]) -> tuple[str, int]:
    override = os.environ.get(env_name)
    if override:
        return override, version_of(override)
    found = [p for pattern in patterns for p in glob.glob(pattern) if os.path.isfile(p)]
    on_path = shutil.which(name)
    if on_path:
        found.append(on_path)
    if not found:
        fail(f"{name} not found; install clang-format/clang-tidy (apt: clang-format clang-tidy) or set {env_name}")
    best = max(found, key=version_of)
    return best, version_of(best)


def find_binary(env_name: str, name: str, version: int) -> str:
    override = os.environ.get(env_name)
    if override:
        return override
    candidates = [f"{name}-{version}", f"/usr/lib/llvm-{version}/bin/{name}"] if version else []
    for candidate in candidates + [name]:
        path = shutil.which(candidate)
        if path:
            return path
    fail(f"{name} not found; install it or set {env_name}")
    return ""


def run_script(script: str, args: list[str], diff: str) -> subprocess.CompletedProcess[str]:
    # The diff scripts are Python; run them with this interpreter so a missing shebang target or
    # a non-executable file (llvm-N/share/clang/*.py) does not matter.
    return subprocess.run([sys.executable, script, *args], cwd=ROOT, input=diff, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, check=False)


def lint_format(merge_base: str, report_only: bool) -> int:
    diff = changed_lines_diff(merge_base)
    if not diff.strip():
        print("clang-format: no changed C/C++ lines in src/, apps/ or tests/.")
        return 0
    script, version = find_script("CLANG_FORMAT_DIFF", "clang-format-diff", [
        "/usr/bin/clang-format-diff-*", "/usr/lib/llvm-*/share/clang/clang-format-diff.py",
        "/usr/share/clang/clang-format-*/clang-format-diff.py"])
    binary = find_binary("CLANG_FORMAT", "clang-format", version)
    print(f"clang-format: {binary} via {script}, changes since {merge_base[:12]}")
    result = run_script(script, ["-p1", "-regex", FORMAT_REGEX, "-binary", binary], diff)
    if not result.stdout.strip():
        print("clang-format: changed lines are formatted.")
        return 0
    print(result.stdout)
    fix = (f"git diff -U0 --no-color {merge_base[:12]} -- {' '.join(PATHS)} | "
           f"{Path(script).name} -p1 -i -regex '{FORMAT_REGEX}'")
    level = "warning" if report_only else "error"
    print(f"{level}: changed lines are not formatted as .clang-format says. Fix them with:\n  {fix}",
          file=sys.stderr)
    if IN_CI:
        print(f"::{level} title=clang-format::Changed lines are not formatted; see the diff in the log. "
              f"Fix: {fix}")
    return 0 if report_only else 1


def lint_tidy(merge_base: str, build_dir: Path, strict: bool) -> int:
    if not (build_dir / "compile_commands.json").is_file():
        fail(f"{build_dir}/compile_commands.json not found: configure the build first "
             "(CMAKE_EXPORT_COMPILE_COMMANDS is ON in CMakeLists.txt)")
    diff = changed_lines_diff(merge_base)
    if not diff.strip():
        print("clang-tidy: no changed C/C++ lines in src/, apps/ or tests/.")
        return 0
    script, version = find_script("CLANG_TIDY_DIFF", "clang-tidy-diff.py", [
        "/usr/bin/clang-tidy-diff-*.py", "/usr/lib/llvm-*/share/clang/clang-tidy-diff.py"])
    binary = find_binary("CLANG_TIDY", "clang-tidy", version)
    print(f"clang-tidy: {binary} via {script}, changes since {merge_base[:12]}", flush=True)
    result = run_script(script, ["-p1", "-path", str(build_dir), "-regex", TIDY_REGEX, "-quiet",
                                 "-j", str(os.cpu_count() or 2), "-clang-tidy-binary", binary], diff)
    print(result.stdout)

    findings = set()
    for line in result.stdout.splitlines():
        match = TIDY_FINDING.match(line)
        if not match:
            continue
        path = os.path.relpath(match.group("path"), ROOT) if os.path.isabs(match.group("path")) else match.group("path")
        findings.add((path, int(match.group("line")), int(match.group("col")), match.group("level"),
                      match.group("message"), match.group("check") or "clang-tidy"))
    for path, line, col, level, message, check in sorted(findings):
        if IN_CI:
            kind = "error" if strict else "warning"
            print(f"::{kind} file={path},line={line},col={col},title={check}::{message}")

    if result.returncode != 0 and not findings:
        print(f"warning: clang-tidy-diff exited with {result.returncode} (see the output above)", file=sys.stderr)
        return 1 if strict else 0
    print(f"clang-tidy: {len(findings)} finding(s) on changed lines"
          + ("" if strict or not findings else " (report-only, not failing)") + ".")
    return 1 if strict and findings else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("format", "tidy"))
    parser.add_argument("--base", default="origin/master", help="revision the change is compared with")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build",
                        help="build directory with compile_commands.json (tidy; default: build)")
    parser.add_argument("--strict", action="store_true", help="tidy: fail on findings")
    parser.add_argument("--report-only", action="store_true", help="format: print the diff, do not fail")
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    merge_base = resolve_base(args.base)
    if args.mode == "format":
        return lint_format(merge_base, args.report_only)
    return lint_tidy(merge_base, args.build_dir.resolve(), args.strict)


if __name__ == "__main__":
    sys.exit(main())
