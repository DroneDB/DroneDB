#!/usr/bin/env python3
"""Checks that every helper program built from a submodule is pinned to a release of its repository.

The helper programs (scripts/helper-tools.txt) that come from a git submodule ship as binaries
whose source is the submodule's commit. A licence notice or source offer names a helper by its
release (THIRD_PARTY_LICENSES.md names the bm3d and metadata-util releases, both GPL), and a
helper reports the version of its version source with --version. Both only hold when the
submodule commit is exactly the commit of the tag v<version> in the helper's own repository:
a submodule moved to a commit between releases, or a version that was not bumped, would ship a
binary whose source is not the release it claims to be.

For each program of helper-tools.txt whose name is the last component of a submodule path in
.gitmodules, the version is read from its version source at the pinned commit: the package version
(package.json) for "package", or the first line of the version file, read from the submodule at the
pinned commit and not from whatever is checked out. The tag is asked of the helper repository
itself (git ls-remote on the submodule's origin), so it is the real tag and not one the local clone
happens to have; an annotated tag is compared by the commit it points to. A program that is not a
submodule is built from this repository and has nothing to pin.

Usage:
    scripts/check-helper-pins.py              (from the repository root)
    scripts/check-helper-pins.py --self-test  (made-up repositories in a temporary folder)

Exit codes: 0 every pin is a release, 1 a pin is not, 2 the check could not run.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent

# The list of helper programs and its parser are those of the package gate (no __pycache__ left
# in scripts/ by the import)
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("check_helper_tools", SCRIPTS_DIR / "check-helper-tools.py")
check_helper_tools = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_helper_tools)

OK, NOT_A_RELEASE, CANNOT_CHECK = 0, 1, 2


class CannotCheck(Exception):
    """The check could not run: a missing submodule, a repository that does not answer."""


@dataclass
class Outcome:
    code: int
    message: str


def git(*args: str, cwd: Path) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise CannotCheck(f"git {' '.join(args)} in {cwd}: {result.stderr.strip()}")
    return result.stdout


def submodule_paths(repo: Path) -> dict[str, str]:
    """The submodule paths of .gitmodules, by their last component."""
    if not (repo / ".gitmodules").is_file():
        return {}
    listed = git("config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$", cwd=repo)
    paths = [line.split(maxsplit=1)[1] for line in listed.splitlines() if line.strip()]
    return {Path(path).name: path for path in paths}


def package_version(repo: Path) -> str:
    """The version of package.json at the commit being checked."""
    try:
        return json.loads(git("show", "HEAD:package.json", cwd=repo))["version"]
    except (KeyError, ValueError) as exc:
        raise CannotCheck(f"package.json has no version: {exc}") from exc


def pinned_version(repo: Path, module: str, pinned: str, source: str) -> str:
    """The version a helper pinned at `pinned` reports: the package's, or its version file's."""
    if source == check_helper_tools.PACKAGE_VERSION:
        return package_version(repo)
    # The version file lives in the submodule: read it at the pinned commit
    relative = Path(source).relative_to(module) if Path(source).is_relative_to(module) else None
    if relative is None:
        raise CannotCheck(f"the version file {source} is not in the submodule {module}")
    lines = git("-C", module, "show", f"{pinned}:{relative.as_posix()}", cwd=repo).splitlines()
    version = lines[0].strip() if lines else ""
    if not version:
        raise CannotCheck(f"{source} is empty at commit {pinned}")
    return version


def tagged_commit(repo: Path, module: str, tag: str) -> str | None:
    """The commit of `tag` on the origin of the submodule, or None when it has no such tag."""
    listed = git("-C", module, "ls-remote", "origin", f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}", cwd=repo)
    refs = {ref: sha for sha, ref in (line.split() for line in listed.splitlines() if line.strip())}
    # A lightweight tag is one line; an annotated tag is two, and the peeled "^{}" line is the commit
    return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")


def check_pin(repo: Path, tool: str, module: str, source: str) -> Outcome:
    if not (repo / module).is_dir() or git("-C", module, "rev-parse", "--show-prefix", cwd=repo).strip():
        raise CannotCheck(f"{module} is not a checked-out submodule (git submodule update --init {module})")
    pinned = git("rev-parse", f"HEAD:{module}", cwd=repo).strip()
    version = pinned_version(repo, module, pinned, source)
    tag = f"v{version}"
    tagged = tagged_commit(repo, module, tag)
    if tagged is None:
        return Outcome(NOT_A_RELEASE, f"{tool}: {module} reports version {version}, but its repository has no tag "
                                      f"{tag}: tag the release, or fix the version")
    if tagged != pinned:
        return Outcome(NOT_A_RELEASE, f"{tool}: {module} is at commit {pinned}, but the tag {tag} is commit {tagged}: "
                                      f"point the submodule at the release tag, not between releases")
    return Outcome(OK, f"{tool}: {module} is at {tag} ({pinned})")


def check_all(repo: Path, tools: dict[str, str]) -> int:
    modules = submodule_paths(repo)
    worst = OK
    for tool, source in tools.items():
        module = modules.get(tool)
        if module is None:
            print(f"skip: {tool} is built from this repository")
            continue
        try:
            outcome = check_pin(repo, tool, module, source)
        except CannotCheck as exc:
            outcome = Outcome(CANNOT_CHECK, f"{tool}: {exc}")
        print(("OK: " if outcome.code == OK else "::error::") + outcome.message,
              file=sys.stdout if outcome.code == OK else sys.stderr)
        worst = max(worst, outcome.code)
    return worst


def self_test() -> int:
    """Runs the check on made-up repositories: a helper origin with tags and a superproject."""
    failures = []

    def expect(label: str, got: int, wanted: int) -> None:
        if got != wanted:
            failures.append(f"{label}: exit {got}, expected {wanted}")

    env_keys = ("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL",
                "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0")
    saved = {key: os.environ.get(key) for key in env_keys}
    os.environ.update({"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                       "GIT_COMMITTER_EMAIL": "t@t",
                       # A submodule cloned from a local path needs the file protocol
                       "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.file.allow",
                       "GIT_CONFIG_VALUE_0": "always"})
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def commit(repo: Path, files: dict[str, str], message: str) -> str:
                for name, text in files.items():
                    (repo / name).write_text(text, encoding="utf-8")
                git("add", "-A", cwd=repo)
                git("commit", "-q", "-m", message, cwd=repo)
                return git("rev-parse", "HEAD", cwd=repo).strip()

            helper = root / "helper"
            helper.mkdir()
            git("init", "-q", cwd=helper)
            first = commit(helper, {"VERSION": "1.0.0\n"}, "one")
            git("tag", "v1.0.0", cwd=helper)
            second = commit(helper, {"VERSION": "1.1.0\n"}, "two")
            git("tag", "-a", "v1.1.0", "-m", "annotated", cwd=helper)
            between = commit(helper, {"other": "x"}, "between releases")

            repo = root / "repo"
            repo.mkdir()
            git("init", "-q", cwd=repo)
            commit(repo, {"package.json": json.dumps({"version": "2.0.0"})}, "package")
            git("submodule", "add", "-q", str(helper), "vendor/helper", cwd=repo)

            def pin(sha: str) -> None:
                git("-C", "vendor/helper", "checkout", "-q", sha, cwd=repo)
                git("add", "vendor/helper", cwd=repo)
                git("commit", "-q", "-m", f"pin {sha}", cwd=repo)

            file_source = {"helper": "vendor/helper/VERSION"}
            pin(first)
            expect("lightweight release tag", check_all(repo, file_source), OK)
            pin(second)
            expect("annotated release tag", check_all(repo, file_source), OK)
            # The version file is read at the pinned commit: the checkout of v1.0.0 would say 1.0.0,
            # whose tag is not the pinned commit
            git("-C", "vendor/helper", "checkout", "-q", first, cwd=repo)
            expect("version read at the pinned commit", check_all(repo, file_source), OK)
            pin(between)
            expect("commit between releases", check_all(repo, file_source), NOT_A_RELEASE)

            package_source = {"helper": check_helper_tools.PACKAGE_VERSION}
            expect("package version without a tag", check_all(repo, package_source), NOT_A_RELEASE)
            git("tag", "v2.0.0", between, cwd=helper)
            expect("package version tagged", check_all(repo, package_source), OK)

            expect("program built from this repository", check_all(repo, {"other": "package"}), OK)
            expect("version file outside the submodule", check_all(repo, {"helper": "VERSION"}), CANNOT_CHECK)
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    for failure in failures:
        print(f"FAIL: self-test: {failure}")
    if failures:
        return NOT_A_RELEASE
    print("PASS: self-test")
    return OK


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the check on made-up repositories")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        return check_all(SCRIPTS_DIR.parent, check_helper_tools.helper_tools())
    except CannotCheck as exc:
        print(f"error: {exc}", file=sys.stderr)
        return CANNOT_CHECK


if __name__ == "__main__":
    sys.exit(main())
