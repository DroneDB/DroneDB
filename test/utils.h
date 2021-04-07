/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <filesystem>
#include <string>

#include "delta.h"

#ifndef TEST_UTILS_H
#define TEST_UTILS_H


void fileWriteAllText(const fs::path& path, const std::string& content);

fs::path makeTree(const std::vector<ddb::SimpleEntry>& entries);
bool compareTree(std::filesystem::path& sourceFolder,
                 std::filesystem::path& destFolder);
void printTree(fs::path& folder);
std::vector<ddb::SimpleEntry> getEntries(const fs::path& path);
std::string calculateHash(const fs::path& file);

#endif  // TEST_UTILS_H
