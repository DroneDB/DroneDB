/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CONTRACT_FIXTURES_H
#define CONTRACT_FIXTURES_H

// Golden contract fixtures of libddb's JSON outputs (XI-09, tests/contract/README.md).
//
// expectMatchesFixture() validates an output against its JSON Schema, normalises its volatile
// values and compares it with tests/contract/fixtures/<name>.json. With the environment
// variable DDB_UPDATE_CONTRACT_FIXTURES=1 it writes the fixture instead (and still validates).

#include <string>
#include <utility>
#include <vector>

#include "fs.h"
#include "json.h"

namespace contract
{
    // tests/contract in the source tree
    fs::path contractDir();

    // tests/contract/inputs: the committed files the contract datasets are made from
    fs::path inputsDir();

    // The modification time the contract tests give every input file and folder, so entry
    // mtimes (and the STAC datetimes taken from them) need no normalisation
    constexpr time_t kInputMtime = 1700000000; // 2023-11-14T22:13:20Z

    // Copies `from` (a file or a folder tree) to `to` and gives every copied file and folder
    // the modification time kInputMtime
    void copyInputs(const fs::path &from, const fs::path &to);

    // Parses a JSON string returned by libddb and frees it with DDBFree
    json takeJson(char *output);

    // Indexes copies of `inputs` (files or folders, copyInputs) in a new dataset at `root`
    // (DDBInit, then DDBAdd, recursive). Returns DDBAdd's output.
    json makeDataset(const fs::path &root, const std::vector<fs::path> &inputs);

    // The placeholder `name` for the path `p` in both its native and its generic form
    std::vector<std::pair<std::string, std::string>> pathPlaceholders(const fs::path &p, const std::string &name);

    struct Fixture
    {
        std::string name;        // fixtures/<name>.json
        std::string description; // what the input is
        std::string call;        // the libddb call that produced the output
        std::string schema;      // schemas/<schema>
        // Literal text replaced in every string value (e.g. the absolute dataset root by
        // "<root>"), longest first
        std::vector<std::pair<std::string, std::string>> placeholders;
        // Object keys whose values change on every run (anywhere in the output): a string
        // becomes "<volatile>", a number 0, so the fixture keeps the output's types
        std::vector<std::string> volatileKeys;
        // When the output is an array that libddb orders by a volatile value (e.g. the random
        // UUIDs of meta items), its items are sorted by these keys, after normalisation
        std::vector<std::string> sortBy;
    };

    // The fixture document for `output` (normalised), as written to fixtures/<name>.json
    json fixtureDocument(const Fixture &fixture, const json &output);

    // Validates the raw output and the normalised one against the fixture's schema, then
    // compares the fixture document with the committed file, or writes it when
    // DDB_UPDATE_CONTRACT_FIXTURES=1. Fails the current test (gtest) on any difference.
    void expectMatchesFixture(const Fixture &fixture, const json &output);
}

#endif // CONTRACT_FIXTURES_H
