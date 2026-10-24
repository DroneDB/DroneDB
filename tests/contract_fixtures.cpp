/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "contract_fixtures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>

#include "ddb.h"
#include "gtest/gtest.h"
#include "json_schema_check.h"

#ifndef DDB_CONTRACT_DIR
#error "DDB_CONTRACT_DIR must name tests/contract (tests/CMakeLists.txt)"
#endif

namespace contract
{
    namespace
    {
        bool updateRequested()
        {
            const char *v = std::getenv("DDB_UPDATE_CONTRACT_FIXTURES");
            return v != nullptr && std::string(v) == "1";
        }

        void replaceAll(std::string &s, const std::string &from, const std::string &to)
        {
            if (from.empty())
                return;
            for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
                s.replace(pos, from.size(), to);
        }

        json normalise(const json &value, const std::vector<std::pair<std::string, std::string>> &placeholders,
                       const std::vector<std::string> &volatileKeys)
        {
            if (value.is_string())
            {
                const std::string original = value.get<std::string>();
                std::string s = original;
                for (const auto &[literal, placeholder] : placeholders)
                    replaceAll(s, literal, placeholder);
                // The rest of a path under a placeholder in the generic form (Windows writes '\\')
                if (s != original)
                    std::replace(s.begin(), s.end(), '\\', '/');
                return s;
            }
            if (value.is_array())
            {
                json out = json::array();
                for (const auto &item : value)
                    out.push_back(normalise(item, placeholders, volatileKeys));
                return out;
            }
            if (!value.is_object())
                return value;

            json out = json::object();
            for (const auto &[key, item] : value.items())
            {
                const bool isVolatile = std::find(volatileKeys.begin(), volatileKeys.end(), key) != volatileKeys.end();
                if (isVolatile && item.is_string())
                    out[key] = "<volatile>";
                else if (isVolatile && item.is_number_float())
                    out[key] = 0.0;
                else if (isVolatile && item.is_number())
                    out[key] = 0;
                else
                    out[key] = normalise(item, placeholders, volatileKeys);
            }
            return out;
        }

        // Longest literal first, so a path is replaced before a prefix of it
        std::vector<std::pair<std::string, std::string>> sortedPlaceholders(
            std::vector<std::pair<std::string, std::string>> placeholders)
        {
            std::stable_sort(placeholders.begin(), placeholders.end(),
                             [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
            return placeholders;
        }

        // A key as a JSON pointer reference token
        std::string pointerToken(const std::string &key)
        {
            std::string token = key;
            replaceAll(token, "~", "~0");
            replaceAll(token, "/", "~1");
            return token;
        }

        // The JSON pointer of the first place where `actual` is not `expected`, or nothing when
        // they are the same: same structure and JSON types (an integer is never equal to a
        // float, unlike json's operator==), floats within a relative 1e-9, so the last-digit
        // differences between compilers and math libraries do not count
        std::optional<std::string> firstDifference(const json &expected, const json &actual, const std::string &where)
        {
            const auto here = std::optional<std::string>(where.empty() ? "/" : where);
            // Signed and unsigned integers are one JSON type (the parser reads 5 as unsigned)
            if (expected.is_number_integer() && actual.is_number_integer())
                return expected == actual ? std::nullopt : here;
            if (expected.type() != actual.type())
                return here;
            if (expected.is_number_float())
            {
                const double e = expected.get<double>(), a = actual.get<double>();
                return std::fabs(e - a) <= 1e-9 * std::max({1.0, std::fabs(e), std::fabs(a)}) ? std::nullopt : here;
            }
            if (expected.is_array())
            {
                if (expected.size() != actual.size())
                    return here;
                for (size_t i = 0; i < expected.size(); i++)
                    if (auto d = firstDifference(expected[i], actual[i], where + "/" + std::to_string(i)))
                        return d;
                return std::nullopt;
            }
            if (expected.is_object())
            {
                for (const auto &[key, value] : actual.items())
                    if (!expected.contains(key))
                        return where + "/" + pointerToken(key);
                for (const auto &[key, value] : expected.items())
                {
                    if (!actual.contains(key))
                        return where + "/" + pointerToken(key);
                    if (auto d = firstDifference(value, actual[key], where + "/" + pointerToken(key)))
                        return d;
                }
                return std::nullopt;
            }
            return expected == actual ? std::nullopt : here;
        }

        void expectValid(testutils::JsonSchemaCheck &check, const json &instance, const std::string &schema,
                         const std::string &what)
        {
            const auto errors = check.validate(instance, schema);
            EXPECT_TRUE(errors.empty()) << what << " does not match schemas/" << schema << ":\n"
                                        << json(errors).dump(2);
        }
    }

    fs::path contractDir() { return fs::path(DDB_CONTRACT_DIR); }

    fs::path inputsDir() { return contractDir() / "inputs"; }

    void copyInputs(const fs::path &from, const fs::path &to)
    {
        fs::create_directories(to.parent_path());
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        // clock_cast is the portable C++20 spelling (MSVC has no file_clock::from_sys); libstdc++ has both
        const auto fixed = std::chrono::clock_cast<std::chrono::file_clock>(
            std::chrono::system_clock::from_time_t(kInputMtime));
        fs::last_write_time(to, fixed);
        if (fs::is_directory(to))
            for (const auto &e : fs::recursive_directory_iterator(to))
                fs::last_write_time(e.path(), fixed);
    }

    json takeJson(char *output)
    {
        const std::string text = output != nullptr ? output : "";
        DDBFree(output);
        return json::parse(text);
    }

    json makeDataset(const fs::path &root, const std::vector<fs::path> &inputs)
    {
        std::vector<std::string> paths;
        for (const auto &input : inputs)
        {
            copyInputs(input, root / input.filename());
            paths.push_back((root / input.filename()).string());
        }

        char *initOut = nullptr;
        if (DDBInit(root.string().c_str(), &initOut) != DDBERR_NONE)
            throw std::runtime_error(std::string("DDBInit: ") + DDBGetLastError());
        DDBFree(initOut);

        std::vector<const char *> cPaths;
        for (const auto &p : paths)
            cPaths.push_back(p.c_str());
        char *out = nullptr;
        if (DDBAdd(root.string().c_str(), cPaths.data(), static_cast<int>(cPaths.size()), &out, true) != DDBERR_NONE)
            throw std::runtime_error(std::string("DDBAdd: ") + DDBGetLastError());
        return takeJson(out);
    }

    std::vector<std::pair<std::string, std::string>> pathPlaceholders(const fs::path &p, const std::string &name)
    {
        return {{p.string(), name}, {p.generic_string(), name}};
    }

    json fixtureDocument(const Fixture &fixture, const json &output)
    {
        json placeholders = json::array();
        for (const auto &p : fixture.placeholders)
            if (std::find(placeholders.begin(), placeholders.end(), p.second) == placeholders.end())
                placeholders.push_back(p.second);

        json normalised = normalise(output, sortedPlaceholders(fixture.placeholders), fixture.volatileKeys);
        if (!fixture.sortBy.empty() && normalised.is_array())
        {
            const auto sortKey = [&](const json &item)
            {
                json key = json::array();
                for (const auto &k : fixture.sortBy)
                    key.push_back(item.value(k, json()));
                return key.dump();
            };
            std::stable_sort(normalised.begin(), normalised.end(),
                             [&](const json &a, const json &b) { return sortKey(a) < sortKey(b); });
        }

        json normalisation = {{"placeholders", placeholders}, {"volatileKeys", fixture.volatileKeys}};
        if (!fixture.sortBy.empty())
            normalisation["sortBy"] = fixture.sortBy;
        return {{"description", fixture.description},
                {"call", fixture.call},
                {"schema", fixture.schema},
                {"normalisation", normalisation},
                {"output", normalised}};
    }

    void expectMatchesFixture(const Fixture &fixture, const json &output)
    {
        SCOPED_TRACE("contract fixture " + fixture.name);
        testutils::JsonSchemaCheck check(contractDir() / "schemas");
        const json document = fixtureDocument(fixture, output);
        expectValid(check, output, fixture.schema, "The output");
        expectValid(check, document["output"], fixture.schema, "The normalised output");

        const fs::path file = contractDir() / "fixtures" / (fixture.name + ".json");
        if (updateRequested())
        {
            fs::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << document.dump(2) << "\n";
            ASSERT_TRUE(out.good()) << "Cannot write " << file;
            return;
        }

        std::ifstream in(file, std::ios::binary);
        ASSERT_TRUE(in.good()) << file << " does not exist: run the test with DDB_UPDATE_CONTRACT_FIXTURES=1 "
                                          "and review the new file (tests/contract/README.md)";
        const json expected = json::parse(in);
        const auto difference = firstDifference(expected, document, "");
        const auto valueAt = [&](const json &j)
        {
            const json::json_pointer ptr(difference && *difference != "/" ? *difference : "");
            return j.contains(ptr) ? j.at(ptr).dump() : std::string("(absent)");
        };
        EXPECT_FALSE(difference.has_value())
            << "libddb's output differs from " << file << " first at " << difference.value_or("") << " (fixture "
            << (difference ? valueAt(expected) : "") << ", output " << (difference ? valueAt(document) : "")
            << ").\nJSON patch from the fixture to the output (numbers that only changed between integer and float "
               "do not show):\n"
            << json::diff(expected, document).dump(2)
            << "\nAn intended change: run with DDB_UPDATE_CONTRACT_FIXTURES=1 and list it in tests/contract/README.md";
    }
}
