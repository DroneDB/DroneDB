/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testarea.h"

#include <cpr/cpr.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"

namespace {
    // SQLite magic header: "SQLite format 3\000"
    const char SQLITE_MAGIC[] = "SQLite format 3";
    const size_t SQLITE_MAGIC_SIZE = 15;

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

        bool valid = std::strncmp(header, SQLITE_MAGIC, SQLITE_MAGIC_SIZE) == 0;
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
    const fs::path root = fs::temp_directory_path() / "ddb_test_areas" / fs::path(name);

    if (recreateIfExists) {
        if (fs::exists(root)) {
            LOGD << "Removing " << root;
            std::error_code ec;
            auto removed = fs::remove_all(root, ec);
            if (ec) {
                LOGD << "Error removing " << root << ": " << ec.message();
                // Try again with a small delay (Windows file locking issues)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                removed = fs::remove_all(root, ec);
                if (ec) {
                    LOGD << "Second attempt failed: " << ec.message();
                }
            }
            LOGD << "Removed " << removed << " files/folders";
        }
    }
}

fs::path TestArea::getPath(const fs::path& p) {
    const fs::path root = fs::temp_directory_path() / "ddb_test_areas" / fs::path(name);
    // Ensure the root directory exists
    if (!fs::exists(root)) {
        ddb::io::createDirectories(root);
        LOGD << "Created test folder " << root;
    }
    return root / p;
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

    // Retry with exponential backoff: 1s, 2s, 4s
    const int maxRetries = 3;
    const int baseDelayMs = 1000;

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        LOGD << "Downloading " << url << " (attempt " << attempt << "/" << maxRetries << ")";

        std::ofstream ofs(destination.string(), std::ios::binary);
        if (!ofs) {
            throw ddb::FSException("Cannot create file: " + destination.string());
        }

        auto request = cpr::Download(ofs, cpr::Url{url}, cpr::VerifySsl(false));
        ofs.close();

        // Check for network errors
        if (request.error) {
            LOGD << "Download error: " << request.error.message;
            if (fs::exists(destination)) fs::remove(destination);

            if (attempt < maxRetries) {
                int delayMs = baseDelayMs * (1 << (attempt - 1));  // 1s, 2s, 4s
                LOGD << "Retrying in " << delayMs << "ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                continue;
            }
            throw ddb::NetException("Failed to download " + url + " to " + destination.string() +
                                    " after " + std::to_string(maxRetries) + " attempts: " +
                                    request.error.message);
        }

        // Check HTTP status code
        if (request.status_code != 200) {
            LOGD << "HTTP error " << request.status_code << " downloading " << url;
            if (fs::exists(destination)) fs::remove(destination);

            if (attempt < maxRetries) {
                int delayMs = baseDelayMs * (1 << (attempt - 1));
                LOGD << "Retrying in " << delayMs << "ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                continue;
            }
            throw ddb::NetException("Failed to download " + url + ": HTTP " +
                                    std::to_string(request.status_code));
        }

        // For SQLite files, validate the downloaded content
        if (isSqliteFile) {
            if (!isValidSqliteFile(destination)) {
                LOGD << "Downloaded file is not a valid SQLite database: " << destination;
                fs::remove(destination);

                if (attempt < maxRetries) {
                    int delayMs = baseDelayMs * (1 << (attempt - 1));
                    LOGD << "Retrying in " << delayMs << "ms...";
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    continue;
                }
                throw ddb::NetException("Downloaded file is not a valid SQLite database: " + url);
            }
            LOGD << "SQLite file validated successfully: " << destination;
        }

        // Download successful
        LOGD << "Downloaded " << url << " to " << destination;
        return destination;
    }

    // Should not reach here, but just in case
    throw ddb::NetException("Failed to download " + url + " after " +
                            std::to_string(maxRetries) + " attempts");
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
