/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef JSON_SCHEMA_CHECK_H
#define JSON_SCHEMA_CHECK_H

// A JSON Schema (draft 2020-12) checker for the subset of keywords the contract schemas in
// tests/contract/schemas use (XI-09). No JSON Schema library is among the vcpkg dependencies,
// so this one covers exactly that subset and rejects every other keyword: a schema cannot use
// a keyword that would be silently ignored here. tests/contract/check_schemas.py validates the
// same schemas and fixtures with the reference Python implementation, for a cross-check.
//
// Supported keywords:
//   annotations   $schema $id $comment title description examples
//   structure     $defs $ref ("#/$defs/name", "other.schema.json", "other.schema.json#/$defs/name")
//   any instance  type (one name or a list) enum const anyOf oneOf allOf
//   objects       properties required additionalProperties (boolean or schema)
//   arrays        items minItems maxItems
//   strings       minLength pattern (ECMAScript, unanchored as the specification says)
//   numbers       minimum maximum
//
// One deliberate difference from the specification: "integer" accepts JSON integer literals
// only, not 1.0. A time written as 1700000000.0 is not "integer epoch seconds".

#include <map>
#include <string>
#include <vector>

#include "fs.h"
#include "json.h"

namespace testutils
{
    class JsonSchemaCheck
    {
        fs::path schemaDir_;
        std::map<std::string, json> documents_;

        const json &document(const std::string &file);
        const json &resolve(const std::string &ref, std::string &file);
        void check(const json &instance, const json &schema, std::string file, const std::string &where,
                   std::vector<std::string> &errors, int depth);

    public:
        // Schemas are files of `schemaDir`; a $ref to another file is relative to it
        explicit JsonSchemaCheck(fs::path schemaDir);

        // The errors of `instance` against the schema file `schemaFile` ("" when valid). Each
        // error names the instance location as a JSON pointer.
        std::vector<std::string> validate(const json &instance, const std::string &schemaFile);
    };
}

#endif // JSON_SCHEMA_CHECK_H
