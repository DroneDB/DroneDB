/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "utils.h"

#include <boolinq/boolinq.h>

void fileWriteAllText(const fs::path& path, const std::string& content) {
    if (exists(path)) fs::remove(path);

    std::ofstream out(path, std::ofstream::out);
    out << content;
}

fs::path makeTree(const std::vector<ddb::SimpleEntry>& entries) {
    const auto tempFolder =
        fs::temp_directory_path() / "diff_test" / ddb::utils::generateRandomString(8);

    create_directories(tempFolder);

    const auto sortedEntries =
        boolinq::from(entries)
            .orderBy([](const ddb::SimpleEntry& entry) { return entry.path; })
            .toStdVector();

    for (const auto& entry : sortedEntries) {
        const auto relPath = tempFolder / entry.path;
        if (entry.type != ddb::Directory) {
            fileWriteAllText(relPath, entry.hash);
        } else {
            create_directories(relPath);
        }
    }

    return tempFolder;
}

bool compareTree(fs::path& sourceFolder, fs::path& destFolder) {
    const auto source = getEntries(sourceFolder);
    const auto dest = getEntries(destFolder);

    std::cout << "Source Folder Tree" << std::endl;

    printTree(sourceFolder);

    std::cout << std::endl << "Dest Folder Tree" << std::endl;

    printTree(destFolder);

    std::cout << std::endl;

    //return false;
    return source == dest;
    /*
    if (source.size() != dest.size()) return false;

    for (auto n = 0; n < source.size(); n++)
    {
        if (source[n] != dest[n]) return false;
    }

    return true;*/
}
void printTree(fs::path& folder) {
    std::vector<fs::path> entries;
     
    std::cout << "PrintTree: " << folder << std::endl;

    for (const auto item : fs::recursive_directory_iterator(folder)) {
        entries.push_back(item.path());
    }
    const auto sortedEntries =
        boolinq::from(entries)
            .orderBy([](const fs::path& entry) {
                return entry.generic_string();
            })
            .toStdVector();

    for (const auto& path : sortedEntries) {

        const auto rel = relative(path, folder);

        const auto relString = rel.generic_string();

        const auto depth = std::count(
            relString.begin(), relString.end(),
            '/');

        for (auto n = 0; n < depth; n++) std::cout << "\t";

        std::cout << rel.filename();

        if (fs::is_regular_file(relString))        
            std::cout << " (" << calculateHash(path) << ")";
        
        std::cout << std::endl;

    }
}

std::vector<ddb::SimpleEntry> getEntries(const fs::path& path) {
    std::vector<ddb::SimpleEntry> entries;

    const auto pathLength = path.generic_string().length();

    for (const auto& item : fs::recursive_directory_iterator(path)) {
        auto fileName = item.path().generic_string();
        const auto itm = item.path().generic_string();

        if (fileName.size() > pathLength + 1)
            fileName = fileName.substr(pathLength + 1);

        if (item.is_directory())
            entries.emplace_back(fileName);
        else
            entries.emplace_back(fileName, calculateHash(itm));
    }

    return entries;
}

std::string calculateHash(const fs::path& file) {
    return Hash::fileSHA256(file.string());
}
