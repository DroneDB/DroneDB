/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TEST_UTILS_H
#define TEST_UTILS_H

// Shared test helpers: the only home of the file, index and environment helpers the test
// files use (DQ-03), so a test never carries its own copy.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "database.h"
#include "delta.h"
#include "entry_types.h"
#include "testarea.h"

// ------------------------------------------------------------------------------- files

// Writes `content` to `path` as is (binary), replacing what the file held and creating the
// parent folders. Returns `path`.
std::filesystem::path writeFile(const std::filesystem::path &path, const std::string &content);
std::filesystem::path writeFile(const std::filesystem::path &path, const std::vector<uint8_t> &content);

// Deletes `path` first, then writes it: a file with other hard links is not written through.
void fileWriteAllText(const std::filesystem::path &path, const std::string &content);

std::filesystem::path makeTree(const std::vector<ddb::SimpleEntry> &entries);
bool compareTree(std::filesystem::path &sourceFolder,
                 std::filesystem::path &destFolder);
void printTree(std::filesystem::path &folder);
std::vector<ddb::SimpleEntry> getEntries(const std::filesystem::path &path);
std::string calculateHash(const std::filesystem::path &file);

void createFile(const std::filesystem::path &filePath);
bool createTestTree(const std::filesystem::path &root);
std::string toHumanReadableTime(long long timeMs);

// ------------------------------------------------------------------------------- index

// A fresh, open index: folder `folder` of the test area `testArea` (the whole area when
// empty), initialized. The test area is emptied first unless `recreateArea` is false (a
// second index in the same area).
struct IndexFixture
{
    TestArea ta;
    std::filesystem::path root;
    std::unique_ptr<ddb::Database> db;

    explicit IndexFixture(const std::string &testArea, const std::filesystem::path &folder = "ds",
                          bool recreateArea = true);
};

// An index like IndexFixture's with "a.txt" ("alpha") added, closed. Returns its root.
std::filesystem::path makeIndex(const std::string &testArea, const std::filesystem::path &folder = "ds",
                                bool recreateArea = true);

// Inserts an index row directly (no file needed): `path` with `hash` and `type`, properties
// "{}", mtime 0, size 1 and the depth of the path. For tests of the build rules, which read
// path, hash and type only.
void insertEntry(ddb::Database *db, const std::string &path, const std::string &hash, ddb::EntryType type);

// Hash of the entry `path` (relative to the dataset root), "<missing>" when it is not indexed
std::string hashOf(ddb::Database *db, const std::string &path);

// Hash of the entry `path`, which the test expects to be indexed: a failure (EXPECT) and an
// empty string when it is not
std::string indexedHash(ddb::Database *db, const std::string &path);

// -------------------------------------------------------------------------------- GDAL

// Number of GDAL datasets open in the process (all threads): a resource leak test compares it
// before and after the call under test.
int openDatasetCount();

// ------------------------------------------------------------------------- environment

// setenv/unsetenv on every platform. On Windows an empty value removes the variable
// (_putenv_s), as unsetEnv does.
void setEnv(const std::string &name, const std::string &value);
void unsetEnv(const std::string &name);

// Saves an environment variable and puts it back (or unsets it, if it was not set) on scope
// exit, even when the test fails or throws. The second constructor also sets it.
class EnvVarGuard
{
    std::string name_;
    std::optional<std::string> saved_;
    bool restored_ = false;

public:
    explicit EnvVarGuard(std::string name);
    EnvVarGuard(std::string name, const std::string &value);
    ~EnvVarGuard();

    EnvVarGuard(const EnvVarGuard &) = delete;
    EnvVarGuard &operator=(const EnvVarGuard &) = delete;

    // Restores the variable now; the destructor then does nothing
    void restore();
};

#endif // TEST_UTILS_H
