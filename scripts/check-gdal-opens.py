#!/usr/bin/env python3
"""GDAL open guard: the host never opens a file of a user with GDAL.

libddb parses the files of users (rasters, vectors, photos) in the raster helper, gdal-util, a
process of its own that is confined and can be killed (apps/gdal-util/PROTOCOL.md). The host (the
Registry, ddbcmd, the tests) asks the helper and opens only what the helper wrote, through
src/include/raster_artefacts.h. Two rules keep that true, and this check enforces both on the
sources:

  raw-open     The calls that open a file by its name (GDALOpen, GDALOpenEx, GDALOpenShared,
               GDALDataset::Open, GDALOpenInfo, GDALIdentifyDriver, GDALIdentifyDriverEx, OGROpen,
               OGROpenShared, OGR_Dr_Open, OGRSFDriverRegistrar::Open, GDALBuildVRT, which opens the
               files it is given by name) and the strings of the network and archive file systems of
               GDAL (/vsicurl, /vsicurl_streaming, /vsis3, /vsigs, /vsiaz, /vsiadls, /vsioss,
               /vsiswift, /vsihdfs, /vsiwebhdfs, /vsizip, /vsitar, /vsi7z, /vsirar and their
               _streaming forms) appear in src/ and apps/ (apps/gdal-util aside: it is the helper)
               only in the files of ALLOWED_FILES, each with its reason. A new
               open goes through openUserRaster / openUserVector (the helper's operations),
               openArtefactRaster / openArtefactVector or reopenWrittenRaster; a file that needs
               a raw one is added to the list with a one-line reason, which a review reads.
  enable-call  enableUserFileOpens( is called only in ENABLE_CALLERS: the start of the helper and
               the one test support file that runs a test body in a child process. Anything else that
               calls it turns a host into a helper, and the gate of openUserRaster, openUserVector
               and identifyUserRaster (gdal_open_policy.h) stops protecting it.

An entry of ALLOWED_FILES or ENABLE_CALLERS that no longer matches anything fails too, so the lists
only hold what is needed. Comments are ignored (and so are string literals, except for the file
system prefixes).

Usage:
    scripts/check-gdal-opens.py              check the sources
    scripts/check-gdal-opens.py --self-test  check the check against built-in fixtures

Exit codes: 0 ok, 1 a rule is broken (or the self-test failed), 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The files that may hold a raw open, with the reason. Everything else goes through the
# functions named above.
ALLOWED_FILES: dict[str, str] = {
    "src/library/gdal_open_policy.cpp":
        "the open policy itself: opens a file of a user with the drivers its extension allows",
    "src/library/raster_artefacts.cpp":
        "opens the artefacts libddb wrote (and the files it just wrote) with the drivers it writes them with",
    "src/library/copc_utils.cpp":
        "reads a COPC file by /vsicurl/ with VSI calls for the node cache; no GDAL driver parses it",
    "src/library/merge_vrt.cpp":
        "GDALBuildVRT over the handles of artefacts it already opened (no file names)",
}

# The files that may call enableUserFileOpens(), with the reason
ENABLE_CALLERS: dict[str, str] = {
    "apps/gdal-util/src/runtime.cpp":
        "the start of gdal-util (main builds the Runtime for --serve and --request)",
    "tests/user_open_child.cpp":
        "the test support that runs a test body in a child process that plays the helper",
}

# Where raw opens are looked for; the helper's own folder is exempt
RAW_OPEN_DIRS = ("src", "apps")
RAW_OPEN_EXEMPT = ("apps/gdal-util/",)
# Where the calls of enableUserFileOpens are looked for
ENABLE_DIRS = ("src", "apps", "tests", "vendor")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
SKIPPED_DIRS = {".git", "node_modules"}

RAW_OPEN = re.compile(r"(?<![\w])(GDALOpen|GDALOpenEx|GDALOpenShared|GDALDataset\s*::\s*Open|GDALOpenInfo|"
                      r"GDALIdentifyDriver|GDALIdentifyDriverEx|OGROpen|OGROpenShared|OGR_Dr_Open|"
                      r"OGRSFDriverRegistrar\s*::\s*Open|GDALBuildVRT)(?![\w])")
# The file systems of GDAL that reach the network or open an archive; a prefix also matches its
# _streaming form
VSI_PREFIX = re.compile(r"/vsi(?:curl|s3|gs|az|adls|oss|swift|hdfs|webhdfs|zip|tar|7z|rar)(?![a-z0-9])")
ENABLE_CALL = re.compile(r"(?<![\w])(?:\w+::)*enableUserFileOpens\s*\(")
DECLARATION = re.compile(r"\bvoid\s+(?:\w+::)*$")

Finding = tuple[str, int, str]  # (file, line, message)


def split_source(text: str) -> tuple[str, list[tuple[int, str]]]:
    """(code, strings): the text with comments removed and string/character literals emptied
    (newlines kept, so line numbers stay), and the contents of the string literals with their line."""
    out: list[str] = []
    strings: list[tuple[int, str]] = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            skipped = text.count("\n", i, end)
            out.append("\n" * skipped)
            line += skipped
            i = end
        elif c == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            if open_paren < 0:
                out.append(c)
                i += 1
                continue
            delimiter = text[i + 2:open_paren]
            close = text.find(")" + delimiter + '"', open_paren)
            end = n if close < 0 else close + len(delimiter) + 2
            strings.append((line, text[open_paren + 1:close if close >= 0 else n]))
            skipped = text.count("\n", i, end)
            out.append('""' + "\n" * skipped)
            line += skipped
            i = end
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if c == '"':
                strings.append((line, text[i + 1:j]))
            out.append(c + c)
            i = j + 1
        else:
            if c == "\n":
                line += 1
            out.append(c)
            i += 1
    return "".join(out), strings


def raw_opens(text: str) -> list[tuple[int, str]]:
    """[(line, what)] of the raw opens in one file."""
    code, strings = split_source(text)
    found = [(number, match.group(1))
             for number, source_line in enumerate(code.split("\n"), 1)
             for match in RAW_OPEN.finditer(source_line)]
    found += [(number, match.group(0))
              for number, content in strings
              for match in VSI_PREFIX.finditer(content)]
    return sorted(found)


def enable_calls(text: str) -> list[int]:
    """The lines that call enableUserFileOpens (its declaration and definition are not calls)."""
    code, _ = split_source(text)
    lines = []
    for number, source_line in enumerate(code.split("\n"), 1):
        for match in ENABLE_CALL.finditer(source_line):
            if not DECLARATION.search(source_line[:match.start()]):
                lines.append(number)
    return lines


def check(files: dict[str, str], allowed: dict[str, str], callers: dict[str, str]) -> list[Finding]:
    """The findings for `files` (path from the repository root -> text)."""
    findings: list[Finding] = []
    seen_allowed: set[str] = set()
    seen_callers: set[str] = set()

    for path, text in sorted(files.items()):
        in_raw_scope = path.split("/")[0] in RAW_OPEN_DIRS and not path.startswith(RAW_OPEN_EXEMPT)
        if in_raw_scope:
            for line, what in raw_opens(text):
                if path in allowed:
                    seen_allowed.add(path)
                else:
                    findings.append((path, line, f"{what}: open files through openUserRaster / openUserVector "
                                                 "(helper operations), openArtefactRaster / openArtefactVector or "
                                                 f"reopenWrittenRaster, or add the file to ALLOWED_FILES of {Path(__file__).name} "
                                                 "with its reason"))
        if path.split("/")[0] in ENABLE_DIRS:
            for line in enable_calls(text):
                if path in callers:
                    seen_callers.add(path)
                else:
                    findings.append((path, line, "enableUserFileOpens() is called here: only the start of gdal-util "
                                                 f"and the test support may ({', '.join(sorted(callers))})"))

    for path in sorted(set(allowed) - seen_allowed):
        findings.append((path, 0, "listed in ALLOWED_FILES but it holds no raw open (or does not exist): "
                                  "remove the entry"))
    for path in sorted(set(callers) - seen_callers):
        findings.append((path, 0, "listed in ENABLE_CALLERS but it does not call enableUserFileOpens(): "
                                  "remove the entry"))
    return findings


def read_tree(root: Path) -> dict[str, str]:
    files: dict[str, str] = {}
    for directory in sorted(set(RAW_OPEN_DIRS) | set(ENABLE_DIRS)):
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            parts = path.relative_to(root).parts
            if path.suffix in SOURCE_SUFFIXES and path.is_file() and not SKIPPED_DIRS.intersection(parts):
                files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    return files


# ------------------------------------------------------------------------------------------ self-test

def self_test() -> int:
    """Runs the check on small made-up trees and compares the findings with what each must give."""
    allowed = {"src/library/policy.cpp": "the policy"}
    callers = {"apps/gdal-util/src/runtime.cpp": "helper start", "tests/support.cpp": "test support"}
    ok_call = "void f() { ddb::gdalopen::enableUserFileOpens(); }\n"
    cases: list[tuple[str, dict[str, str], dict[str, str], dict[str, str], list[tuple[str, int]]]] = [
        ("an open in a file that is not listed",
         {"src/library/a.cpp": "auto d = GDALOpenEx(p, 0, 0, 0, 0);\n"}, {}, {},
         [("src/library/a.cpp", 1)]),
        ("every function of the rule",
         {"src/library/a.cpp": "GDALOpen(p, 0);\nGDALOpenShared(p, 0);\nGDALIdentifyDriver(p, 0);\n"
                               "GDALIdentifyDriverEx(p, 0, 0, 0);\nOGROpen(p, 0, 0);\nOGROpenShared(p, 0, 0);\n"},
         {}, {},
         [("src/library/a.cpp", n) for n in range(1, 7)]),
        ("a listed file may open",
         {"src/library/policy.cpp": "auto d = GDALOpenEx(p, 0, 0, 0, 0);\n"}, allowed, {}, []),
        ("a listed file that holds no open is stale",
         {"src/library/policy.cpp": "int x = 1;\n"}, allowed, {}, [("src/library/policy.cpp", 0)]),
        ("a listed file that is gone is stale", {}, allowed, {}, [("src/library/policy.cpp", 0)]),
        ("comments are ignored",
         {"src/library/a.cpp": "// GDALOpen(p)\n/* GDALOpenEx(\n OGROpen */ int x;\n"
                               "// \"/vsicurl/\"\n"}, {}, {}, []),
        ("a name inside a string is not an open",
         {"src/library/a.cpp": 'const char *s = "GDALOpen failed";\n'}, {}, {}, []),
        ("every other function that opens a file by its name",
         {"src/library/a.cpp": "GDALDataset::Open(p, 0);\nGDALOpenInfo info(p, GA_ReadOnly);\n"
                               "OGR_Dr_Open(d, p, 0);\nOGRSFDriverRegistrar::Open(p);\n"
                               "GDALBuildVRT(o, 2, nullptr, names, opts, &e);\n::GDALOpenEx(p, 0, 0, 0, 0);\n"
                               "GDALDataset :: Open(p, 0);\n"},
         {}, {},
         [("src/library/a.cpp", n) for n in range(1, 8)]),
        ("another identifier that starts or ends with the name is not an open",
         {"src/library/a.cpp": "MyGDALOpen(); GDALOpenEx2(); GDALOpenInfoTag t; GDALBuildVRTOptionsNew(0, 0);\n"
                               "x.GDALDatasetOpen(); OGR_Dr_OpenFoo();\n"},
         {}, {}, []),
        ("the /vsicurl string is an open",
         {"src/library/a.cpp": 'auto p = "/vsicurl/" + url;\n'}, {}, {}, [("src/library/a.cpp", 1)]),
        ("every network and archive file system is an open",
         {"src/library/a.cpp": "".join(f'auto p{i} = "/vsi{name}/x";\n' for i, name in enumerate(
             ["curl", "curl_streaming", "s3", "s3_streaming", "gs", "gs_streaming", "az", "az_streaming", "adls",
              "oss", "oss_streaming", "swift", "swift_streaming", "hdfs", "webhdfs", "zip", "tar", "7z", "rar"]))},
         {}, {},
         [("src/library/a.cpp", n) for n in range(1, 20)]),
        ("the other file systems are not",
         {"src/library/a.cpp": 'auto a = "/vsimem/x"; auto b = "/vsigzip/x"; auto c = "/vsistdout/";\n'
                               'auto d = "/vsizipper/"; auto e = "/vsis3x/";\n'},
         {}, {}, []),
        ("a file system in a comment is not an open",
         {"src/library/a.cpp": "// \"/vsis3/bucket\" and /vsizip/ here\n"}, {}, {}, []),
        ("a raw string with /vsicurl is an open",
         {"src/library/a.cpp": 'auto p = R"x(\n/vsicurl/)x";\n'}, {}, {}, [("src/library/a.cpp", 1)]),
        ("the line of an open after a block comment",
         {"src/library/a.cpp": "/* one\n two */\nGDALOpen(p, 0);\n"}, {}, {}, [("src/library/a.cpp", 3)]),
        ("the folder of the helper is exempt",
         {"apps/gdal-util/src/x.cpp": "GDALOpenEx(p, 0, 0, 0, 0);\n"}, {}, {}, []),
        ("other folders of apps are not",
         {"apps/cmd/x.cpp": "GDALOpenEx(p, 0, 0, 0, 0);\n"}, {}, {}, [("apps/cmd/x.cpp", 1)]),
        ("tests may open (they are not the host)",
         {"tests/unit/x.cpp": "GDALOpenEx(p, 0, 0, 0, 0);\n"}, {}, {}, []),
        ("the enable call of the helper and of the test support",
         {"apps/gdal-util/src/runtime.cpp": ok_call, "tests/support.cpp": ok_call}, {}, callers, []),
        ("an enable call anywhere else",
         {"src/library/a.cpp": ok_call, "apps/gdal-util/src/runtime.cpp": ok_call, "tests/support.cpp": ok_call},
         {}, callers, [("src/library/a.cpp", 1)]),
        ("an enable call in a test",
         {"tests/unit/x.cpp": "TEST(a, b) { enableUserFileOpens(); }\n",
          "apps/gdal-util/src/runtime.cpp": ok_call, "tests/support.cpp": ok_call}, {}, callers,
         [("tests/unit/x.cpp", 1)]),
        ("the declaration and the definition are no calls",
         {"src/include/p.h": "namespace a { void enableUserFileOpens(); }\n",
          "src/library/p.cpp": "void ddb::gdalopen::enableUserFileOpens()\n{\n}\n"}, {}, {}, []),
        ("a mention in a comment or a string is no call",
         {"src/library/a.cpp": '// enableUserFileOpens()\nconst char *s = "enableUserFileOpens()";\n'}, {}, {}, []),
        ("a caller that no longer calls is stale",
         {"apps/gdal-util/src/runtime.cpp": ok_call, "tests/support.cpp": "int x;\n"}, {}, callers,
         [("tests/support.cpp", 0)]),
    ]

    failures = 0
    for name, files, case_allowed, case_callers, expected in cases:
        got = sorted((path, line) for path, line, _ in check(files, case_allowed, case_callers))
        if got != sorted(expected):
            failures += 1
            print(f"self-test FAILED: {name}\n  expected {sorted(expected)}\n  got      {got}", file=sys.stderr)
    if failures:
        print(f"{failures} of {len(cases)} self-test cases failed", file=sys.stderr)
        return 1
    print(f"self-test passed ({len(cases)} cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the check against built-in fixtures")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    findings = check(read_tree(ROOT), ALLOWED_FILES, ENABLE_CALLERS)
    for path, line, message in findings:
        print(f"{path}:{line}: {message}" if line else f"{path}: {message}", file=sys.stderr)
    if findings:
        print(f"{len(findings)} problem(s): the host must not open files of users with GDAL", file=sys.stderr)
        return 1
    print("ok: no raw GDAL open outside the allow-list, enableUserFileOpens() called only where it may be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
