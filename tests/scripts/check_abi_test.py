# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Self-test of scripts/check-abi.py: the semver rule of `bump` and the `tag` check.

Every case builds a throw-away git repository (package.json, the symbol manifest, ddb.h) with a
release commit tagged v1.2.3, changes it and runs `check-abi.py bump` against the tag. Run by
ctest (abi.CheckAbiSemver) or directly: python3 tests/scripts/check_abi_test.py
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent.parent / "scripts" / "check-abi.py"
BASE = ("DDBAdd", "DDBGet", "DDBInit")


def manifest(symbols):
    return "# libddb symbol manifest\n" + "".join(f"{s}\n" for s in sorted(symbols))


def header(symbols):
    return '#include "ddb_version.h"\n' + "".join(f"DDB_DLL int {s}(void);\n" for s in sorted(symbols))


class CheckAbi(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        (self.repo / "src" / "include").mkdir(parents=True)
        self.git("init", "-q")
        self.write("1.2.3", BASE)
        self.commit("release", tag="v1.2.3")

    def git(self, *args):
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                   GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t")
        subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, env=env)

    def write(self, version, symbols, with_manifest=True):
        (self.repo / "package.json").write_text(json.dumps({"name": "ddb", "version": version}))
        (self.repo / "src/include/ddb.h").write_text(header(symbols))
        manifest_file = self.repo / "src/include/ddb.symbols"
        if with_manifest:
            manifest_file.write_text(manifest(symbols))
        elif manifest_file.exists():
            manifest_file.unlink()

    def commit(self, message, tag=None):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        if tag:
            self.git("tag", tag)

    def run_check(self, *args):
        result = subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo), *args],
                                capture_output=True, text=True, check=False)
        return result.returncode, result.stdout

    def bump(self, version, symbols, base="v1.2.3"):
        self.write(version, symbols)
        return self.run_check("bump", "--base", base)

    def assertPasses(self, version, symbols):
        code, out = self.bump(version, symbols)
        self.assertEqual(code, 0, out)

    def assertFails(self, version, symbols, mentions):
        code, out = self.bump(version, symbols)
        self.assertEqual(code, 1, out)
        self.assertIn(mentions, out)

    def test_unchanged_exports_pass_with_any_version_that_does_not_go_down(self):
        for version in ("1.2.3", "1.2.4", "1.3.0", "2.0.0"):
            with self.subTest(version=version):
                self.assertPasses(version, BASE)

    def test_a_version_that_goes_down_fails(self):
        for version in ("1.2.2", "1.1.9", "0.9.0"):
            with self.subTest(version=version):
                self.assertFails(version, BASE, "lower than 1.2.3")

    def test_an_added_export_needs_a_minor_or_major_bump(self):
        added = BASE + ("DDBNew",)
        for version in ("1.2.3", "1.2.4", "1.2.99"):
            with self.subTest(version=version):
                self.assertFails(version, added, "without a MINOR or MAJOR bump")
        for version in ("1.3.0", "1.4.1", "2.0.0"):
            with self.subTest(version=version):
                self.assertPasses(version, added)

    def test_a_removed_export_needs_a_major_bump(self):
        removed = BASE[:-1]
        for version in ("1.2.3", "1.2.4", "1.3.0", "1.99.0"):
            with self.subTest(version=version):
                self.assertFails(version, removed, "without a MAJOR bump")
        for version in ("2.0.0", "3.1.4"):
            with self.subTest(version=version):
                self.assertPasses(version, removed)

    def test_a_major_bump_resets_the_minor(self):
        # (2, 0) is above (1, 2): the added export has its bump
        self.assertPasses("2.0.0", BASE + ("DDBNew",))

    def test_replaced_export_needs_a_major_bump(self):
        replaced = BASE[:-1] + ("DDBOther",)
        self.assertFails("1.9.0", replaced, "without a MAJOR bump")
        self.assertPasses("2.0.0", replaced)

    def test_a_base_without_manifest_is_read_from_its_header(self):
        self.write("1.0.0", BASE, with_manifest=False)
        self.commit("old release without manifest", tag="v1.0.0")
        code, out = self.bump("1.0.1", BASE[:-1], base="v1.0.0")
        self.assertEqual(code, 1, out)
        self.assertIn("without a MAJOR bump", out)
        code, out = self.bump("2.0.0", BASE[:-1], base="v1.0.0")
        self.assertEqual(code, 0, out)
        code, out = self.bump("1.0.1", BASE + ("DDBNew",), base="v1.0.0")
        self.assertEqual(code, 1, out)
        self.assertIn("DDBNew", out)

    def test_the_default_base_is_the_previous_release_tag(self):
        self.write("1.3.0", BASE + ("DDBNew",))
        self.commit("next release", tag="v1.3.0")
        # HEAD is the release v1.3.0: its base is v1.2.3, not itself
        code, out = self.run_check("bump")
        self.assertEqual(code, 0, out)
        self.assertIn("v1.2.3", out)

    def test_an_unknown_base_is_an_input_error(self):
        code, out = self.run_check("bump", "--base", "v9.9.9")
        self.assertEqual(code, 2, out)

    def test_tag_equals_the_package_version(self):
        self.assertEqual(self.run_check("tag", "--tag", "v1.2.3")[0], 0)
        self.assertEqual(self.run_check("tag", "--tag", "refs/tags/v1.2.3")[0], 0)
        self.assertEqual(self.run_check("tag", "--tag", "v1.2.4")[0], 1)
        self.assertEqual(self.run_check("tag", "--tag", "1.2.3")[0], 1)


if __name__ == "__main__":
    unittest.main()
