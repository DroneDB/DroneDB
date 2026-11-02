#!/usr/bin/env python3
"""Fails when a Windows executable does not declare itself long path aware.

Windows takes a path of 260 characters or more only in a process whose executable has a manifest
with <longPathAware>true</longPathAware> (and only where the system allows long paths:
LongPathsEnabled=1). The setting belongs to the executable that starts the process, so every
program that opens the files of a dataset needs it: the CLI, the helper programs of
scripts/helper-tools.txt, Untwine and the test binaries (cmake/LongPathAware.cmake). A link that
leaves the manifest out, or a CMake change that stops merging it, is not seen by any run on a
machine with short paths, so this reads the manifest embedded in each file.

The manifest is the XML text stored in the file as its RT_MANIFEST resource (UTF-8, not
compressed). It is found by its <assembly> element and parsed, so a longPathAware inside a string
of the program's code, or one with the wrong namespace or value, does not count. It needs no
Windows tool, so the script runs on any host; the test binaries and tools of a Linux build have no
manifest and fail it, which is the point.

Usage:
    scripts/check-long-path-aware.py FILE...      the executables to check
    scripts/check-long-path-aware.py --self-test  the detection, without any executable

Exit codes: 0 every file declares it, 1 a file does not, 2 a file cannot be read.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

SETTINGS_NAMESPACE = "http://schemas.microsoft.com/SMI/2016/WindowsSettings"
LONG_PATH_AWARE = f"{{{SETTINGS_NAMESPACE}}}longPathAware"
# The root element of an application manifest and its end tag: the text between is the manifest
MANIFEST_PATTERN = re.compile(rb"<assembly\b.*?</assembly\s*>", re.DOTALL)


def declares_long_path_aware(data: bytes) -> bool:
    """True when a manifest embedded in `data` sets longPathAware to true."""
    for match in MANIFEST_PATTERN.finditer(data):
        try:
            root = ET.fromstring(match.group(0))
        except ET.ParseError:
            continue
        if any((element.text or "").strip() == "true" for element in root.iter(LONG_PATH_AWARE)):
            return True
    return False


def check_files(files: list[Path]) -> int:
    status = 0
    for file in files:
        try:
            data = file.read_bytes()
        except OSError as error:
            print(f"error: cannot read {file}: {error}")
            return 2
        if declares_long_path_aware(data):
            print(f"ok: {file} is long path aware")
        else:
            print(f"FAIL: {file} has no manifest with <longPathAware>true</longPathAware>")
            status = 1
    return status


def self_test() -> int:
    def manifest(setting: str, namespace: str = SETTINGS_NAMESPACE) -> bytes:
        return (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n'
            '<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>'
            f'{setting.replace("NS", namespace)}'
            "</windowsSettings></application></assembly>"
        ).encode()

    # The bytes around a manifest in an executable are not text
    def in_executable(content: bytes) -> bytes:
        return b"MZ\x90\x00\x03" + bytes(range(256)) + content + b"\x00\xff\xfe" + b"PE\x00\x00"

    default = b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"></assembly>'
    cases = [
        ("true", True, in_executable(manifest('<longPathAware xmlns="NS">true</longPathAware>'))),
        ("true with spaces", True, in_executable(manifest('<longPathAware xmlns="NS">\n  true\n</longPathAware>'))),
        ("prefixed", True, in_executable(manifest('<ws:longPathAware xmlns:ws="NS">true</ws:longPathAware>'))),
        ("after a manifest without it", True, in_executable(default + manifest('<longPathAware xmlns="NS">true</longPathAware>'))),
        ("false", False, in_executable(manifest('<longPathAware xmlns="NS">false</longPathAware>'))),
        ("another setting", False, in_executable(manifest('<dpiAware xmlns="NS">true</dpiAware>'))),
        ("another namespace", False, in_executable(manifest('<longPathAware xmlns="NS">true</longPathAware>', "urn:other"))),
        ("a manifest without it", False, in_executable(default)),
        ("the name in the code", False, in_executable(b"longPathAware true</longPathAware>")),
        ("a broken manifest", False, in_executable(b"<assembly><longPathAware>true</assembly")),
        ("no manifest", False, in_executable(b"")),
    ]
    failures = 0
    for label, expected, data in cases:
        actual = declares_long_path_aware(data)
        if actual != expected:
            print(f"FAIL: self-test: {label}: {actual}, expected {expected}")
            failures += 1

    with tempfile.TemporaryDirectory() as folder:
        good, bad = Path(folder, "good.exe"), Path(folder, "bad.exe")
        good.write_bytes(cases[0][2])
        bad.write_bytes(cases[7][2])
        for label, files, expected in (
            ("one file", [good], 0),
            ("a file that does not", [good, bad], 1),
            ("a missing file", [good, Path(folder, "missing.exe")], 2),
        ):
            actual = check_files(files)
            if actual != expected:
                print(f"FAIL: self-test: {label}: exit {actual}, expected {expected}")
                failures += 1
    print("FAIL: self-test" if failures else "PASS: self-test")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", type=Path, help="the executables to check")
    parser.add_argument("--self-test", action="store_true", help="check the detection, without any executable")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.files:
        parser.error("name at least one file, or use --self-test")
    return check_files(args.files)


if __name__ == "__main__":
    sys.exit(main())
