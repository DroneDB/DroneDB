# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Self-test of scripts/check-abi.py: the semver rule of `bump`, the `tag` check and the `exports`
check (the library exports exactly the manifest).

Every `bump` case builds a throw-away git repository (package.json, the symbol manifest, ddb.h)
with a release commit tagged v1.2.3, changes it and runs `check-abi.py bump` against the tag. The
`exports` cases read a shared library built on the spot (ELF, with the C compiler, when there is
one) and a PE file written byte by byte (the Windows checker has no other test here). Run by ctest
(abi.CheckAbiSemver) or directly: python3 tests/scripts/check_abi_test.py
"""
import json
import os
import shutil
import struct
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


def pe_dll(exports):
    """A minimal PE32+ DLL whose export table holds `exports` ({name: is_function}): one .text
    section (executable) and one .rdata section (data) with the export directory; a function name
    points into .text, a data name into .rdata."""
    names = sorted(exports)
    file_align = 0x200
    text_rva, rdata_rva = 0x1000, 0x2000
    directory_rva = rdata_rva + 0x10  # the first bytes of .rdata are the exported variables
    count = len(names)
    functions_rva = directory_rva + 40
    names_rva = functions_rva + 4 * count
    ordinals_rva = names_rva + 4 * count
    strings_rva = ordinals_rva + 2 * count
    dll_name = b"ddb.dll\0"
    strings = bytearray(dll_name)
    name_rvas = []
    for name in names:
        name_rvas.append(strings_rva + len(strings))
        strings += name.encode("ascii") + b"\0"
    functions = b"".join(struct.pack("<I", text_rva if exports[name] else rdata_rva) for name in names)
    name_table = b"".join(struct.pack("<I", rva) for rva in name_rvas)
    ordinals = b"".join(struct.pack("<H", i) for i in range(count))
    directory = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, strings_rva + len(strings) - len(dll_name),
                            1, count, count, functions_rva, names_rva, ordinals_rva)
    rdata = bytearray(0x10) + directory + functions + name_table + ordinals + strings
    export_size = len(rdata) - 0x10
    rdata += bytes(-len(rdata) % file_align)
    text = b"\xc3" + bytes(file_align - 1)

    headers_size = 0x40 + 4 + 20 + 240 + 2 * 40
    headers_size += -headers_size % file_align
    text_raw, rdata_raw = headers_size, headers_size + len(text)
    # The optional header (240 bytes, PE32+): the checker reads the magic, the number of data
    # directories (offset 108) and the first directory, the export table (offset 112)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<II", optional, 112, directory_rva, export_size)
    optional = bytes(optional)
    coff = struct.pack("<HHIIIHH", 0x8664, 2, 0, 0, 0, len(optional), 0x2022)
    sections = struct.pack("<8sIIIIIIHHI", b".text", len(text), text_rva, len(text), text_raw, 0, 0, 0, 0, 0x60000020)
    sections += struct.pack("<8sIIIIIIHHI", b".rdata", len(rdata), rdata_rva, len(rdata), rdata_raw, 0, 0, 0, 0, 0x40000040)
    header = bytearray(0x40)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    image = bytes(header) + b"PE\0\0" + coff + optional + sections
    return image + bytes(headers_size - len(image)) + text + bytes(rdata)


class CheckExports(unittest.TestCase):
    """`check-abi.py exports` over a repository that holds a manifest and the header of BASE."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.repo = self.dir / "repo"
        (self.repo / "src" / "include").mkdir(parents=True)
        (self.repo / "package.json").write_text(json.dumps({"name": "ddb", "version": "2.0.0"}))
        (self.repo / "src/include/ddb.h").write_text(header(BASE))
        (self.repo / "src/include/ddb.symbols").write_text(manifest(BASE))

    def run_exports(self, library):
        result = subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo), "exports",
                                 "--library", str(library)], capture_output=True, text=True, check=False)
        return result.returncode, result.stdout

    def elf(self, functions, variables=()):
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None or shutil.which("nm") is None or not sys.platform.startswith("linux"):
            self.skipTest("needs cc and nm on Linux")
        source = self.dir / "lib.c"
        source.write_text("".join(f"int {name}(void) {{ return 1; }}\n" for name in functions)
                          + "".join(f"int {name} = 1;\n" for name in variables))
        library = self.dir / "libtest.so"
        subprocess.run([compiler, "-shared", "-fPIC", "-o", str(library), str(source)], check=True)
        return library

    def pe(self, exports):
        library = self.dir / "test.dll"
        library.write_bytes(pe_dll(exports))
        return library

    def test_elf_exporting_exactly_the_manifest_passes(self):
        code, out = self.run_exports(self.elf(BASE))
        self.assertEqual(code, 0, out)

    def test_elf_with_a_missing_export_fails(self):
        code, out = self.run_exports(self.elf(BASE[:-1]))
        self.assertEqual(code, 1, out)
        self.assertIn("not exported by", out)
        self.assertIn(BASE[-1], out)

    def test_elf_with_an_extra_ddb_export_fails(self):
        code, out = self.run_exports(self.elf(BASE + ("DDBNew",)))
        self.assertEqual(code, 1, out)
        self.assertIn("DDBNew", out)

    def test_elf_with_any_other_export_fails(self):
        code, out = self.run_exports(self.elf(BASE + ("_ZN3ddb4openEv", "cpr_helper")))
        self.assertEqual(code, 1, out)
        self.assertIn("2 symbols that are not DDB* functions", out)
        self.assertIn("cpr_helper", out)

    def test_elf_with_an_exported_ddb_variable_fails(self):
        code, out = self.run_exports(self.elf(BASE, variables=("DDBState",)))
        self.assertEqual(code, 1, out)
        self.assertIn("not functions", out)

    def test_pe_exporting_exactly_the_manifest_passes(self):
        code, out = self.run_exports(self.pe({name: True for name in BASE}))
        self.assertEqual(code, 0, out)

    def test_pe_with_a_missing_export_fails(self):
        code, out = self.run_exports(self.pe({name: True for name in BASE[:-1]}))
        self.assertEqual(code, 1, out)
        self.assertIn("not exported by", out)

    def test_pe_with_any_other_export_fails(self):
        exports = {name: True for name in BASE}
        exports["??0Database@ddb@@QEAA@XZ"] = True
        exports["PLOG_instance"] = False
        code, out = self.run_exports(self.pe(exports))
        self.assertEqual(code, 1, out)
        self.assertIn("2 symbols that are not DDB* functions", out)
        self.assertIn("PLOG_instance", out)

    def test_pe_with_an_exported_ddb_variable_fails(self):
        exports = {name: True for name in BASE}
        exports["DDBState"] = False
        code, out = self.run_exports(self.pe(exports))
        self.assertEqual(code, 1, out)
        self.assertIn("not functions", out)

    def test_a_file_that_is_no_library_is_an_input_error(self):
        (self.dir / "x.bin").write_bytes(b"not a library")
        code, out = self.run_exports(self.dir / "x.bin")
        self.assertEqual(code, 2, out)


if __name__ == "__main__":
    unittest.main()
