#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""libddb ABI checks (XI-01): the exported symbols, the ABI version and the release version.

The library, its C ABI and the release have ONE version: the "version" of package.json (semver).
The ABI is its MAJOR and MINOR (DDB_ABI_MAJOR/DDB_ABI_MINOR in ddb_version.h, generated from
package.json by CMake and included by ddb.h); the PATCH never changes the ABI.
src/include/ddb.symbols lists every function libddb exports whose name starts with DDB (format
in its header). Three checks keep it, ddb.h and the release in step:

  exports  The DDB* symbols the built library exports equal the manifest. Linux: the defined
           dynamic symbols of libddb.so (nm -D --defined-only); Windows: the export table of
           ddb.dll, read from the PE file. Also: the manifest is well formed (names, byte order,
           no duplicates), ddb.h includes ddb_version.h and defines no version macro of its own,
           and every DDB_DLL declaration of ddb.h is in the manifest. An exported DDB* symbol
           that is not a function (a variable) fails too.
  bump     Semver against a base commit (default: the previous release tag v*.*.*): the version
           never goes down, a removed export needs a higher MAJOR, an added export a higher
           MAJOR or MINOR (a PATCH bump alone does not do). The manifest lists names only: a
           changed signature or meaning is invisible to this check, so its MAJOR bump is up to
           the author and the review. A base without a manifest (releases before 2.0, e.g.
           v1.12.2) is read from its ddb.h: the DDB_DLL declarations.
  tag      A release tag vX.Y.Z equals the "version" of package.json.

Usage:
    scripts/check-abi.py exports --library build/libddb.so     (or build/ddb.dll)
    scripts/check-abi.py bump [--base <git ref>]
    scripts/check-abi.py tag --tag v2.0.0                      (or refs/tags/v2.0.0)
Paths are relative to the repository root (--repo, default: the parent of scripts/).
Exit codes: 0 ok, 1 check failed, 2 usage or input error. Python standard library only.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

MANIFEST = "src/include/ddb.symbols"
HEADER = "src/include/ddb.h"
VERSION_TEMPLATE = "src/library/ddb_version.h.in"
PACKAGE_JSON = "package.json"

SYMBOL_RE = re.compile(r"^DDB[A-Za-z0-9_]+$")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
# The version macros of ddb_version.h: ddb.h must not define them itself
VERSION_MACRO_RE = re.compile(r"^\s*#\s*define\s+DDB_(?:ABI|VERSION)_(?:MAJOR|MINOR|PATCH|STRING)\b", re.M)
VERSION_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"ddb_version\.h"', re.M)
# A declaration "DDB_DLL <type> DDBName(", as tests/c_abi_coverage.cmake reads it
DECLARATION_RE = re.compile(r"DDB_DLL[^;(]*?[ *](DDB[A-Za-z0-9_]+)\s*\(")
RELEASE_TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)$")


class InputError(Exception):
    """A file or a git object that cannot be read or parsed (exit code 2)."""


def report(message):
    # GitHub Actions turns ::error:: lines into annotations
    prefix = "::error::" if os.environ.get("GITHUB_ACTIONS") == "true" else "error: "
    print(prefix + message)


# --- ddb.h and the manifest -------------------------------------------------------------------

def package_version(text, where):
    """(major, minor, patch) from the "version" of a package.json text."""
    try:
        version = json.loads(text)["version"]
    except (ValueError, KeyError, TypeError) as e:
        raise InputError(f"{where} has no \"version\": {e}") from e
    match = VERSION_RE.match(str(version))
    if match is None:
        raise InputError(f"{where}: version '{version}' is not MAJOR.MINOR.PATCH")
    return tuple(int(group) for group in match.groups())


def format_version(version):
    return ".".join(str(part) for part in version)


def header_declarations(text):
    return set(DECLARATION_RE.findall(text))


def parse_manifest(text, where):
    """(symbols in file order, format errors). Comment lines ('#') carry no data."""
    symbols, errors = [], []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not SYMBOL_RE.match(line) or line != raw.rstrip("\r\n"):
            errors.append(f"{where}:{number}: '{raw}' is not a DDB* symbol name alone on its line")
            continue
        symbols.append(line)
    if not symbols:
        errors.append(f"{where}: no symbol")
    duplicates = sorted({s for s in symbols if symbols.count(s) > 1})
    if duplicates:
        errors.append(f"{where}: duplicate symbols: {', '.join(duplicates)}")
    if symbols != sorted(symbols):
        errors.append(f"{where}: symbols are not sorted in byte order (LC_ALL=C sort)")
    return symbols, errors


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError as e:
        raise InputError(f"cannot read {path}: {e.strerror}") from e


# --- exported symbols of the library ----------------------------------------------------------

def elf_exports(path):
    """{name: is_function} for the defined dynamic symbols of an ELF shared library (nm -D)."""
    try:
        result = subprocess.run(["nm", "-D", "--defined-only", "--format=posix", str(path)],
                                capture_output=True, text=True, check=False)
    except FileNotFoundError as e:
        raise InputError("nm (binutils) is required to read an ELF library") from e
    if result.returncode != 0:
        raise InputError(f"nm failed on {path}: {result.stderr.strip()}")
    exports = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        name = fields[0].split("@", 1)[0]  # versioned names: DDBAdd@@VERS
        exports[name] = fields[1] in ("T", "W", "i")  # text, weak or indirect function
    return exports


def pe_exports(path):
    """{name: is_function} for the export table of a PE (Windows DLL) file. A name is a function
    when its address lies in an executable section; forwarders count as functions."""
    data = Path(path).read_bytes()

    def u16(offset):
        return struct.unpack_from("<H", data, offset)[0]

    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]

    try:
        pe = u32(0x3C)
        if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
            raise InputError(f"{path} is not a PE file")
        coff = pe + 4
        section_count, optional_size = u16(coff + 2), u16(coff + 16)
        optional = coff + 20
        magic = u16(optional)
        if magic not in (0x10B, 0x20B):
            raise InputError(f"{path}: unknown optional header magic 0x{magic:x}")
        directories = optional + (96 if magic == 0x10B else 112)
        if u32(directories - 4) < 1:  # NumberOfRvaAndSizes
            return {}
        export_rva, export_size = u32(directories), u32(directories + 4)
        if export_rva == 0:
            return {}
        sections = []
        for i in range(section_count):
            header = optional + optional_size + 40 * i
            virtual_size, address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
            characteristics = u32(header + 36)
            sections.append((address, max(virtual_size, raw_size), raw_offset, characteristics))

        def section_of(rva):
            for section in sections:
                if section[0] <= rva < section[0] + section[1]:
                    return section
            raise InputError(f"{path}: address 0x{rva:x} is in no section")

        def offset(rva):
            address, _, raw_offset, _ = section_of(rva)
            return rva - address + raw_offset

        def c_string(rva):
            start = offset(rva)
            return data[start:data.index(b"\0", start)].decode("ascii")

        directory = offset(export_rva)
        function_count, name_count = u32(directory + 20), u32(directory + 24)
        if name_count == 0:
            return {}
        functions, names, ordinals = (offset(u32(directory + o)) for o in (28, 32, 36))
        exports = {}
        for i in range(name_count):
            ordinal = u16(ordinals + 2 * i)
            if ordinal >= function_count:
                raise InputError(f"{path}: export ordinal {ordinal} out of range")
            target = u32(functions + 4 * ordinal)
            forwarder = export_rva <= target < export_rva + export_size
            executable = bool(section_of(target)[3] & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE
            exports[c_string(u32(names + 4 * i))] = forwarder or executable
        return exports
    except (struct.error, ValueError, UnicodeDecodeError) as e:
        raise InputError(f"{path}: malformed PE file ({e})") from e


def library_exports(path):
    path = Path(path)
    try:
        with path.open("rb") as f:
            magic = f.read(4)
    except OSError as e:
        raise InputError(f"cannot read {path}: {e.strerror}") from e
    if magic == b"\x7fELF":
        return elf_exports(path)
    if magic[:2] == b"MZ":
        return pe_exports(path)
    raise InputError(f"{path} is neither an ELF nor a PE library")


# --- checks -----------------------------------------------------------------------------------

def check_exports(repo, library):
    errors = []
    header = read_text(repo / HEADER)
    symbols, format_errors = parse_manifest(read_text(repo / MANIFEST), MANIFEST)
    errors += format_errors
    version = package_version(read_text(repo / PACKAGE_JSON), PACKAGE_JSON)
    if VERSION_INCLUDE_RE.search(header) is None:
        errors.append(f'{HEADER} does not include "ddb_version.h" (the ABI macros)')
    if VERSION_MACRO_RE.search(header):
        errors.append(f"{HEADER} defines a version macro: the version comes from {PACKAGE_JSON} "
                      f"through {VERSION_TEMPLATE}")
    undeclared = sorted(header_declarations(header) - set(symbols))
    if undeclared:
        errors.append(f"declared DDB_DLL in {HEADER} but not in {MANIFEST}: {', '.join(undeclared)}")

    exports = {name: function for name, function in library_exports(library).items()
               if SYMBOL_RE.match(name)}
    variables = sorted(name for name, function in exports.items() if not function)
    if variables:
        errors.append(f"{library} exports DDB* symbols that are not functions: {', '.join(variables)}")
    missing = sorted(set(symbols) - set(exports))
    if missing:
        errors.append(f"in {MANIFEST} but not exported by {library}: {', '.join(missing)}")
    unlisted = sorted(set(exports) - set(symbols))
    if unlisted:
        errors.append(f"exported by {library} but not in {MANIFEST} (add them, and bump "
                      f"the MINOR of {PACKAGE_JSON} if not done since the last release): "
                      f"{', '.join(unlisted)}")
    for error in errors:
        report(error)
    if not errors:
        print(f"{library}: exports the {len(symbols)} DDB* functions of {MANIFEST}, "
              f"ABI {version[0]}.{version[1]} (version {format_version(version)})")
    return not errors


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=False)
    return result.returncode, result.stdout, result.stderr.strip()


def git_show(repo, ref, path):
    code, out, _ = git(repo, "show", f"{ref}:{path}")
    return out if code == 0 else None


def previous_release_tag(repo):
    """The newest release tag reachable from HEAD^: the previous release, also when HEAD is one."""
    code, out, err = git(repo, "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*.[0-9]*.[0-9]*", "HEAD^")
    if code != 0:
        raise InputError(f"no previous release tag (git describe: {err}); fetch the tags "
                         "(fetch-depth: 0) or pass --base")
    return out.strip()


def check_bump(repo, base):
    if base is None:
        base = previous_release_tag(repo)
    code, _, err = git(repo, "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}")
    if code != 0:
        raise InputError(f"unknown git ref '{base}' {err}".strip())

    symbols, format_errors = parse_manifest(read_text(repo / MANIFEST), MANIFEST)
    if format_errors:
        for error in format_errors:
            report(error)
        return False
    current = package_version(read_text(repo / PACKAGE_JSON), PACKAGE_JSON)

    base_package = git_show(repo, base, PACKAGE_JSON)
    if base_package is None:
        raise InputError(f"{base} has no {PACKAGE_JSON}")
    base_version = package_version(base_package, f"{base}:{PACKAGE_JSON}")
    base_manifest = git_show(repo, base, MANIFEST)
    if base_manifest is not None:
        base_symbols, base_errors = parse_manifest(base_manifest, f"{base}:{MANIFEST}")
        if base_errors:
            raise InputError("; ".join(base_errors))
        base_source = MANIFEST
    else:
        base_header = git_show(repo, base, HEADER)
        if base_header is None:
            raise InputError(f"{base} has no {HEADER}")
        base_symbols = header_declarations(base_header)
        base_source = f"{HEADER} (no manifest yet)"

    added = sorted(set(symbols) - set(base_symbols))
    removed = sorted(set(base_symbols) - set(symbols))
    errors = []
    version, base_text = format_version(current), format_version(base_version)
    if current < base_version:
        errors.append(f"version {version} is lower than {base_text} at {base}")
    if removed and current[0] <= base_version[0]:
        errors.append(f"removed since {base} (version {base_text}) without a MAJOR bump of "
                      f"{PACKAGE_JSON} (version {version}): {', '.join(removed)}")
    if added and current[:2] <= base_version[:2]:
        errors.append(f"added since {base} (version {base_text}) without a MINOR or MAJOR bump of "
                      f"{PACKAGE_JSON} (version {version}; a PATCH bump does not change the ABI): "
                      f"{', '.join(added)}")
    for error in errors:
        report(error)
    if not errors:
        change = f"{len(added)} added, {len(removed)} removed" if added or removed else "unchanged"
        print(f"version {version} (ABI {current[0]}.{current[1]}) vs {base} (version {base_text}, "
              f"{base_source}): symbols {change}")
    return not errors


def check_tag(repo, tag):
    name = tag.removeprefix("refs/tags/")
    match = RELEASE_TAG_RE.match(name)
    package_text = format_version(package_version(read_text(repo / PACKAGE_JSON), PACKAGE_JSON))
    if match is None:
        report(f"tag '{name}' is not a release tag vMAJOR.MINOR.PATCH")
        return False
    if match.group(1) != package_text:
        report(f"tag {name} does not match {PACKAGE_JSON} version {package_text}")
        return False
    print(f"tag {name} matches {PACKAGE_JSON} version {package_text}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the parent of scripts/)")
    commands = parser.add_subparsers(dest="command", required=True)
    exports = commands.add_parser("exports", help="exported DDB* symbols == manifest")
    exports.add_argument("--library", type=Path, required=True, help="libddb.so or ddb.dll")
    bump = commands.add_parser("bump", help="manifest change => semver bump of package.json")
    bump.add_argument("--base", help="git ref to compare with (default: the previous release tag)")
    tag = commands.add_parser("tag", help="release tag == package.json version")
    tag.add_argument("--tag", required=True, help="vX.Y.Z or refs/tags/vX.Y.Z")
    args = parser.parse_args()

    try:
        if args.command == "exports":
            ok = check_exports(args.repo, args.library)
        elif args.command == "bump":
            ok = check_bump(args.repo, args.base)
        else:
            ok = check_tag(args.repo, args.tag)
    except InputError as e:
        report(str(e))
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
