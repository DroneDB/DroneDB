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
            downloadTestAsset(testArchivePath, tempPath.string(), true);
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
std::filesystem::path TestFS::downloadTestAsset(const std::string &url, const std::string &destination, bool overwrite)
{
    std::filesystem::path destPath = destination;

    if (std::filesystem::exists(destPath))
    {
        if (!overwrite)
        {
            return destPath;
        }
        else
        {
            std::filesystem::remove(destPath);
        }
    }

    std::ofstream ofs(destPath.string(), std::ios::binary);
    auto request = cpr::Download(ofs, cpr::Url{url}, cpr::VerifySsl(false));

    if (request.error)
        throw std::runtime_error("Failed to download " + url + " to " + destPath.string());

    return destPath;
}
