/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testarea.h"

#include <cpr/cpr.h>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"

TestArea::TestArea(const std::string& name, bool recreateIfExists) : name(name) {
    const auto root = getFolder();
    if (name.find("..") != std::string::npos)
        throw ddb::FSException("Cannot use .. in name");

    if (recreateIfExists) {
        if (fs::exists(root)) {
            LOGD << "Removing " << root;
            LOGD << "Removed " << fs::remove_all(root) << " files/folders";
        }
    }
}

fs::path TestArea::getPath(const fs::path& p) {
    return fs::temp_directory_path() / "ddb_test_areas" / fs::path(name) / p;
}

fs::path TestArea::getFolder(const fs::path& subfolder) {
    const fs::path root = fs::temp_directory_path() / "ddb_test_areas" / fs::path(name);
    auto dir = root;
    if (!subfolder.empty())
        dir = dir / subfolder;

    if (!fs::exists(dir)) {
        ddb::io::createDirectories(dir);
        LOGD << "Created test folder " << dir;
    }
    return dir;
}

fs::path TestArea::downloadTestAsset(const std::string& url,
                                     const std::string& filename,
                                     bool overwrite) {
    fs::path destination = getFolder() / fs::path(filename);

    if (fs::exists(destination)) {
        // Check if file exists but is empty (0 bytes)
        if (fs::file_size(destination) == 0) {
            // File exists but is empty, force overwrite
            overwrite = true;
            LOGD << "Found empty file at " << destination << ", forcing overwrite";
        }

        if (!overwrite)
            return destination;
        else
            fs::remove(destination);
    }

    std::ofstream ofs(destination.string(), std::ios::binary);
    auto request = cpr::Download(ofs, cpr::Url{url}, cpr::VerifySsl(false));

    if (request.error)
        throw ddb::NetException("Failed to download " + url + " to " + destination.string());

    return destination;
}

void TestArea::clearAll() {
    const fs::path testAreasRoot = fs::temp_directory_path() / "ddb_test_areas";
    if (!fs::exists(testAreasRoot)) {
        std::cout << "No test areas to clear\n";
        return;
    }

    LOGD << "Removing all test areas from " << testAreasRoot;
    auto removedCount = fs::remove_all(testAreasRoot);
    LOGD << "Removed " << removedCount << " files/folders from test areas";
    std::cout << "Cleared all test areas (" << removedCount << " items removed)\n";
}
