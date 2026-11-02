/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testarea.h"
#include "testfs.h"

#include <algorithm>
#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ddb.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "randomutil.h"

namespace {
    // SQLite magic header: "SQLite format 3\000" (16 bytes including NUL terminator)
    const char SQLITE_MAGIC[] = "SQLite format 3\0";
    const size_t SQLITE_MAGIC_SIZE = 16;

    /**
     * @brief Validates that a file is a valid SQLite database by checking the magic header.
     * @param filePath Path to the file to validate
     * @return true if the file has a valid SQLite header, false otherwise
     */
    bool isValidSqliteFile(const fs::path& filePath) {
        if (!fs::exists(filePath)) return false;

        auto fileSize = fs::file_size(filePath);
        if (fileSize < SQLITE_MAGIC_SIZE) {
            LOGD << "File too small to be SQLite: " << filePath << " (" << fileSize << " bytes)";
            return false;
        }

        std::ifstream ifs(filePath.string(), std::ios::binary);
        if (!ifs) {
            LOGD << "Cannot open file for validation: " << filePath;
            return false;
        }

        char header[16] = {0};
        ifs.read(header, SQLITE_MAGIC_SIZE);

        if (!ifs) {
            LOGD << "Failed to read header from: " << filePath;
            return false;
        }

        bool valid = std::memcmp(header, SQLITE_MAGIC, SQLITE_MAGIC_SIZE) == 0;
        if (!valid) {
            LOGD << "Invalid SQLite header in " << filePath << ". First bytes: "
                 << std::string(header, std::min(static_cast<size_t>(10), strlen(header)));
        }
        return valid;
    }
}

TestArea::TestArea(const std::string& name, bool recreateIfExists) : name(name) {
    if (name.find("..") != std::string::npos)
        throw ddb::FSException("Cannot use .. in name");

    // Calculate root path without creating the directory
    const fs::path root = processRoot() / fs::path(name);

    if (recreateIfExists) {
        if (fs::exists(root)) {
            LOGD << "Removing " << root;
            // The library keeps the index connections and the rasters of recent calls open
            // between calls: a host releases them before it deletes a dataset folder (ddb.h),
            // or Windows refuses to delete the open files. A test reusing its area is such a host.
            DDBReleaseConnections(ddb::io::toUtf8(root).c_str(), nullptr);
            std::error_code ec;
            const auto removed = fs::remove_all(root, ec);
            if (ec)
                throw ddb::FSException("Cannot recreate the test area " + ddb::io::toUtf8(root) + ": " + ec.message(),
                                       ec);
            LOGD << "Removed " << removed << " files/folders";
        }
    }
}

fs::path TestArea::getPath(const fs::path& p) {
    const fs::path root = processRoot() / fs::path(name);
    // Ensure the root directory exists
    if (!fs::exists(root)) {
        ddb::io::createDirectories(root);
        LOGD << "Created test folder " << root;
    }
    return root / p;
}

fs::path TestArea::getFolder(const fs::path& subfolder) {
    const fs::path root = processRoot() / fs::path(name);
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
    bool isSqliteFile = filename.size() >= 7 &&
                        filename.substr(filename.size() - 7) == ".sqlite";

    if (fs::exists(destination)) {
        // Check if file exists but is empty (0 bytes)
        if (fs::file_size(destination) == 0) {
            overwrite = true;
            LOGD << "Found empty file at " << destination << ", forcing overwrite";
        }
        // For SQLite files, validate the cached file has valid header
        else if (isSqliteFile && !isValidSqliteFile(destination)) {
            overwrite = true;
            LOGD << "Found corrupted SQLite file at " << destination << ", forcing re-download";
        }

        if (!overwrite)
            return destination;
        else
            fs::remove(destination);
    }

    std::function<bool(const fs::path&)> validator;
    if (isSqliteFile)
        validator = isValidSqliteFile;

    // Shared cache: downloaded once, copied to the test area
    const fs::path cached = TestFS::cachedAssetPath(url);
    fs::create_directories(cached.parent_path());
    if (isSqliteFile && fs::exists(cached) && !isValidSqliteFile(cached)) {
        LOGD << "Found corrupted SQLite file at " << cached << ", forcing re-download";
        fs::remove(cached);
    }
    TestFS::downloadTestAsset(url, cached, false, validator);
    fs::copy_file(cached, destination, fs::copy_options::overwrite_existing);
    return destination;
}

namespace {
    int currentProcessId() {
#ifdef _WIN32
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

    // The folder of this process's areas. Removed when the process ends (static destruction: also
    // after std::exit, which is how a death-test child ends), unless it is kept. Only by the
    // process that made it: a fork() child that exits would remove the folder of its parent.
    struct ProcessRoot {
        const fs::path path = fs::temp_directory_path() / "ddb_test_areas" / ddb::randomString(12);
        const int owner = currentProcessId();
        bool keep = false;

        ~ProcessRoot() {
            if (keep || currentProcessId() != owner)
                return;
            // Nothing to report from here: the logger may be gone, and a folder that stays
            // (open files on Windows) is only a leftover of the temp folder
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    ProcessRoot& processRootState() {
        static ProcessRoot root;
        return root;
    }
}

fs::path TestArea::processRoot() {
    return processRootState().path;
}

void TestArea::keepProcessRoot() {
    processRootState().keep = true;
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
