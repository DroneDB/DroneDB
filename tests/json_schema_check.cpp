/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "json_schema_check.h"

#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>

namespace testutils
{
    namespace
    {
        const std::set<std::string> kAnnotations = {"$schema", "$id", "$comment", "title", "description",
                                                    "examples", "$defs"};
        const std::set<std::string> kKeywords = {"$ref", "type", "enum", "const", "anyOf", "oneOf", "allOf",
                                                 "properties", "required", "additionalProperties", "items",
                                                 "minItems", "maxItems", "minLength", "pattern", "minimum",
                                                 "maximum"};
        // A $ref cycle (or a schema nested this deep) is a broken schema
        constexpr int kMaxDepth = 64;

        bool hasType(const json &instance, const std::string &type)
        {
            if (type == "object")
                return instance.is_object();
            if (type == "array")
                return instance.is_array();
            if (type == "string")
                return instance.is_string();
            if (type == "integer")
                return instance.is_number_integer();
            if (type == "number")
                return instance.is_number();
            if (type == "boolean")
                return instance.is_boolean();
            if (type == "null")
                return instance.is_null();
            throw std::invalid_argument("unknown type \"" + type + "\"");
        }

        // JSON pointer escaping of one reference token
        std::string escapeToken(const std::string &token)
        {
            std::string out;
            for (char c : token)
            {
                if (c == '~')
                    out += "~0";
                else if (c == '/')
                    out += "~1";
                else
                    out += c;
            }
            return out;
        }

        std::string location(const std::string &where) { return where.empty() ? "/" : where; }
    }

    JsonSchemaCheck::JsonSchemaCheck(fs::path schemaDir) : schemaDir_(std::move(schemaDir)) {}

    const json &JsonSchemaCheck::document(const std::string &file)
    {
        auto it = documents_.find(file);
        if (it != documents_.end())
            return it->second;
        std::ifstream in(schemaDir_ / file);
        if (!in)
            throw std::runtime_error("Cannot read schema " + (schemaDir_ / file).string());
        return documents_.emplace(file, json::parse(in)).first->second;
    }

    const json &JsonSchemaCheck::resolve(const std::string &ref, std::string &file)
    {
        const auto hash = ref.find('#');
        if (hash != 0)
            file = ref.substr(0, hash);
        const json &doc = document(file);
        if (hash == std::string::npos || hash + 1 == ref.size())
            return doc;
        return doc.at(json::json_pointer(ref.substr(hash + 1)));
    }

    void JsonSchemaCheck::check(const json &instance, const json &schema, std::string file,
                                const std::string &where, std::vector<std::string> &errors, int depth)
    {
        const auto fail = [&](const std::string &message) { errors.push_back(location(where) + ": " + message); };
        if (depth > kMaxDepth)
            throw std::runtime_error("Schema nesting deeper than " + std::to_string(kMaxDepth) + " at " +
                                     location(where) + " (a $ref cycle?)");
        if (schema.is_boolean())
        {
            if (!schema.get<bool>())
                fail("no value is allowed here");
            return;
        }
        if (!schema.is_object())
            throw std::runtime_error("A schema must be an object or a boolean (" + file + ")");

        for (const auto &[keyword, _] : schema.items())
            if (kAnnotations.count(keyword) == 0 && kKeywords.count(keyword) == 0)
                throw std::runtime_error("Unsupported schema keyword \"" + keyword + "\" in " + file +
                                         " (json_schema_check.h lists the supported subset)");

        if (schema.contains("$ref"))
        {
            std::string target = file;
            const json &resolved = resolve(schema["$ref"].get<std::string>(), target);
            check(instance, resolved, target, where, errors, depth + 1);
        }

        if (schema.contains("type"))
        {
            const json &type = schema["type"];
            bool ok = false;
            if (type.is_string())
                ok = hasType(instance, type.get<std::string>());
            else
                for (const auto &t : type)
                    ok = ok || hasType(instance, t.get<std::string>());
            if (!ok)
            {
                fail("expected type " + type.dump() + ", found " + std::string(instance.type_name()) + " " +
                     instance.dump());
                return; // the other keywords would only repeat the type error
            }
        }
        if (schema.contains("enum"))
        {
            bool found = false;
            for (const auto &value : schema["enum"])
                found = found || value == instance;
            if (!found)
                fail(instance.dump() + " is not one of " + schema["enum"].dump());
        }
        if (schema.contains("const") && schema["const"] != instance)
            fail("expected " + schema["const"].dump() + ", found " + instance.dump());

        const auto countMatches = [&](const json &subschemas, std::vector<std::string> &subErrors)
        {
            int matches = 0;
            for (const auto &sub : subschemas)
            {
                std::vector<std::string> e;
                check(instance, sub, file, where, e, depth + 1);
                if (e.empty())
                    matches++;
                else
                    subErrors.insert(subErrors.end(), e.begin(), e.end());
            }
            return matches;
        };
        if (schema.contains("allOf"))
            for (const auto &sub : schema["allOf"])
                check(instance, sub, file, where, errors, depth + 1);
        if (schema.contains("anyOf"))
        {
            std::vector<std::string> subErrors;
            if (countMatches(schema["anyOf"], subErrors) == 0)
                fail("matches no anyOf branch: " + json(subErrors).dump());
        }
        if (schema.contains("oneOf"))
        {
            std::vector<std::string> subErrors;
            const int matches = countMatches(schema["oneOf"], subErrors);
            if (matches != 1)
                fail("matches " + std::to_string(matches) + " oneOf branches instead of 1: " + json(subErrors).dump());
        }

        if (instance.is_object())
        {
            if (schema.contains("required"))
                for (const auto &key : schema["required"])
                    if (!instance.contains(key.get<std::string>()))
                        fail("missing required property \"" + key.get<std::string>() + "\"");
            const json properties = schema.value("properties", json::object());
            for (const auto &[key, value] : instance.items())
            {
                const std::string child = where + "/" + escapeToken(key);
                if (properties.contains(key))
                    check(value, properties[key], file, child, errors, depth + 1);
                else if (schema.contains("additionalProperties"))
                    check(value, schema["additionalProperties"], file, child, errors, depth + 1);
            }
        }
        if (instance.is_array())
        {
            if (schema.contains("minItems") && instance.size() < schema["minItems"].get<size_t>())
                fail("fewer than " + schema["minItems"].dump() + " items");
            if (schema.contains("maxItems") && instance.size() > schema["maxItems"].get<size_t>())
                fail("more than " + schema["maxItems"].dump() + " items");
            if (schema.contains("items"))
                for (size_t i = 0; i < instance.size(); i++)
                    check(instance[i], schema["items"], file, where + "/" + std::to_string(i), errors, depth + 1);
        }
        if (instance.is_string())
        {
            const auto &s = instance.get_ref<const std::string &>();
            if (schema.contains("minLength") && s.size() < schema["minLength"].get<size_t>())
                fail("shorter than " + schema["minLength"].dump() + " characters");
            if (schema.contains("pattern") &&
                !std::regex_search(s, std::regex(schema["pattern"].get<std::string>(), std::regex::ECMAScript)))
                fail(instance.dump() + " does not match " + schema["pattern"].dump());
        }
        if (instance.is_number())
        {
            const double v = instance.get<double>();
            if (schema.contains("minimum") && v < schema["minimum"].get<double>())
                fail(instance.dump() + " is less than " + schema["minimum"].dump());
            if (schema.contains("maximum") && v > schema["maximum"].get<double>())
                fail(instance.dump() + " is greater than " + schema["maximum"].dump());
        }
    }

    std::vector<std::string> JsonSchemaCheck::validate(const json &instance, const std::string &schemaFile)
    {
        std::vector<std::string> errors;
        check(instance, document(schemaFile), schemaFile, "", errors, 0);
        return errors;
    }
}
