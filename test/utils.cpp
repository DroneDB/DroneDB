/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "utils.h"

#include <boolinq/boolinq.h>

void fileWriteAllText(const fs::path& path, const std::string& content) {
    if (exists(path)) fs::remove(path);

    std::ofstream out(path, std::ofstream::out);
    out << path;
}

fs::path makeTree(std::vector<ddb::SimpleEntry>& entries) {
    const auto tempFolder =
        fs::temp_directory_path() / "diff_test" / std::to_string(time(nullptr));

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
    std::vector<std::filesystem::directory_entry> entries;

    for (const auto item : fs::recursive_directory_iterator(folder)) {
        entries.push_back(item);
    }

    const auto sortedEntries =
        boolinq::from(entries)
            .orderBy([](const std::filesystem::directory_entry& entry) {
                return entry.path().generic_string();
            })
            .toStdVector();

    for (const auto& entry : sortedEntries) {

        const auto& path = entry.path();

        const auto rel = fs::relative(folder, path);

        const auto relString = rel.generic_string();

        const auto depth = std::count(
            relString.begin(), relString.end(),
            std::filesystem::path::preferred_separator);

        for (auto n = 0; n < depth; n++) std::cout << "\t";

        std::cout << rel.filename();

        if (!is_directory(rel))        
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
