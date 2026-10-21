#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Cross-checks the contract schemas and fixtures with the reference JSON Schema implementation.

The C++ tests validate with the subset checker in tests/json_schema_check.h (no JSON Schema
library is among the vcpkg dependencies). This script, optional and not run by ctest, checks the
same files with python-jsonschema (pip install jsonschema): every schema is a valid draft 2020-12
schema and every fixture output matches the schema it names.

Usage: python3 -I tests/contract/check_schemas.py [tests/contract]
"""
import json
import pathlib
import sys

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:
    sys.exit("python-jsonschema >= 4.18 is required: pip install jsonschema")


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).parent)
    schemas = {p.name: json.loads(p.read_text(encoding="utf-8")) for p in sorted((root / "schemas").glob("*.json"))}
    # Each schema under its file name, so "entry.schema.json#/$defs/..." resolves as in the C++ checker
    registry = Registry().with_resources((name, Resource.from_contents(s)) for name, s in schemas.items())

    failures = 0
    for name, schema in schemas.items():
        try:
            Draft202012Validator.check_schema(schema)
        except Exception as e:  # noqa: BLE001 - report every invalid schema
            print(f"schemas/{name}: invalid schema: {e}")
            failures += 1

    fixtures = sorted((root / "fixtures").glob("*.json"))
    for path in fixtures:
        fixture = json.loads(path.read_text(encoding="utf-8"))
        schema_name = fixture["schema"]
        validator = Draft202012Validator({"$ref": schema_name}, registry=registry)
        for error in validator.iter_errors(fixture["output"]):
            print(f"fixtures/{path.name}: {'/'.join(map(str, error.absolute_path))}: {error.message}")
            failures += 1

    print(f"{len(schemas)} schemas, {len(fixtures)} fixtures, {failures} problems")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
