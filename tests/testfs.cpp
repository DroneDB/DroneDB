/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testfs.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include <cpr/cpr.h>
#include "mzip.h"
#include <utils.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <functional>

TestFS::TestFS(const std::string &testArchivePath, const std::string &baseTestFolder, bool setCurrentDirectory)
    : testArchivePath(testArchivePath), baseTestFolder(baseTestFolder), oldCurrentDirectory_(std::filesystem::current_path())
{

    // Generate a random test folder path
    testFolder = (std::filesystem::temp_directory_path() / baseTestFolder / ddb::utils::generateRandomString(16)).string();
    std::filesystem::create_directories(testFolder);

    if (!isLocalPath(testArchivePath))
    {
        // Handle remote URL
        auto tempPath = std::filesystem::temp_directory_path() / baseTestFolder / extractFileName(testArchivePath);
        if (!std::filesystem::exists(tempPath))
        {
            std::cout << "Downloading archive...\n";
            downloadTestAsset(testArchivePath, tempPath, true);
        }
        else
        {
            std::cout << "Using cached archive...\n";
        }
        ddb::zip::extractAll(tempPath.string(), testFolder, nullptr);
    }
    else
    {
        ddb::zip::extractAll(testArchivePath, testFolder, nullptr);
    }

    std::cout << "Created test FS '" << testArchivePath << "' in '" << testFolder << "'\n";

    if (setCurrentDirectory)
    {
        std::filesystem::current_path(testFolder);
        std::cout << "Set current directory to '" << testFolder << "'\n";
    }
}

/**
 * @brief Destructor for TestFS. Cleans up the test folder and restores the previous working directory.
 */
TestFS::~TestFS()
{
    if (!oldCurrentDirectory_.empty())
    {
        std::filesystem::current_path(oldCurrentDirectory_);
        std::cout << "Restored current directory to '" << oldCurrentDirectory_.string() << "'\n";
    }

    try
    {
        std::filesystem::remove_all(testFolder);
        std::cout << "Deleted test folder '" << testFolder << "'\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error deleting test folder: " << e.what() << "\n";
    }
}

/**
 * @brief Clears the cache by deleting the base test folder and its contents.
 * @param baseTestFolder The base test folder to be cleared.
 */
void TestFS::clearCache(const std::string &baseTestFolder)
{
    auto folder = std::filesystem::temp_directory_path() / baseTestFolder;
    if (std::filesystem::exists(folder))
    {
        std::filesystem::remove_all(folder);
    }
}

/**
 * @brief Determines if the given path is a local file path.
 * @param path The path to evaluate.
 * @return True if the path is local, otherwise false.
 */
bool TestFS::isLocalPath(const std::string &path)
{
    return path.rfind("file:/", 0) == 0 || (path.rfind("http://", 0) != 0 && path.rfind("https://", 0) != 0 && path.rfind("ftp://", 0) != 0);
}

/**
 * @brief Extracts the file name from a given path.
 * @param path The full path to extract the file name from.
 * @return The file name extracted from the path.
 */
std::string TestFS::extractFileName(const std::string &path)
{
    size_t pos = path.find_last_of("/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/**
 * @brief Downloads a test asset from a URL to a specified destination.
 * @param url The URL of the file to download.
 * @param destination The destination path to save the downloaded file.
 * @param overwrite If true, overwrites the file if it already exists.
 * @return The path of the downloaded file.
 */
std::filesystem::path TestFS::downloadTestAsset(const std::string &url, const std::filesystem::path &destination, bool overwrite, std::function<bool(const std::filesystem::path &)> validator)
{
    namespace fs = std::filesystem;

    if (fs::exists(destination))
    {
        if (fs::file_size(destination) == 0)
        {
            overwrite = true;
            LOGD << "Found empty file at " << destination << ", forcing overwrite";
        }

        if (!overwrite)
            return destination;
        else
            fs::remove(destination);
    }

    // Retry with exponential backoff: 1s, 2s, 4s, 8s, 16s, 32s, 64s
    const int maxRetries = 7;
    const int baseDelayMs = 1000;

    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        LOGD << "Downloading " << url << " (attempt " << attempt << "/" << maxRetries << ")";

        std::ofstream ofs(destination.string(), std::ios::binary);
        if (!ofs)
            throw ddb::FSException("Cannot create file: " + destination.string());

        auto request = cpr::Download(ofs, cpr::Url{url}, cpr::VerifySsl(false));
        ofs.close();

        // Check for network errors
        if (request.error)
        {
            LOGD << "Download error: " << request.error.message;
            if (fs::exists(destination)) fs::remove(destination);

            if (attempt < maxRetries)
            {
                int delayMs = baseDelayMs * (1 << (attempt - 1));
                LOGD << "Retrying in " << delayMs << "ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                continue;
            }
            throw ddb::NetException("Failed to download " + url + " to " + destination.string() +
                                    " after " + std::to_string(maxRetries) + " attempts: " +
                                    request.error.message);
        }

        // Check HTTP status code
        if (request.status_code != 200)
        {
            LOGD << "HTTP error " << request.status_code << " downloading " << url;
            if (fs::exists(destination)) fs::remove(destination);

            if (attempt < maxRetries)
            {
                int delayMs = baseDelayMs * (1 << (attempt - 1));
                LOGD << "Retrying in " << delayMs << "ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                continue;
            }
            throw ddb::NetException("Failed to download " + url + ": HTTP " +
                                    std::to_string(request.status_code));
        }

        // Run optional validator
        if (validator && !validator(destination))
        {
            LOGD << "Validation failed for downloaded file: " << destination;
            fs::remove(destination);

            if (attempt < maxRetries)
            {
                int delayMs = baseDelayMs * (1 << (attempt - 1));
                LOGD << "Retrying in " << delayMs << "ms...";
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                continue;
            }
            throw ddb::NetException("Downloaded file failed validation: " + url);
        }

        LOGD << "Downloaded " << url << " to " << destination;
        return destination;
    }

    // Should not reach here
    throw ddb::NetException("Failed to download " + url + " after " +
                            std::to_string(maxRetries) + " attempts");
}
