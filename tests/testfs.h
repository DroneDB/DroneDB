/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TESTFS_H
#define TESTFS_H

#include <string>
#include <functional>
#include "fs.h"

/**
 * @brief URL of a file of the DroneDB/test_data repository at the pinned commit.
 *
 * Every test asset is read at one commit (tests/test-data.sha, also read by CI), so a test never
 * sees an asset change under it and a cached download stays valid. To move to newer assets,
 * update the SHA in that file.
 * @param path Path of the file from the root of the repository, e.g. "brighton/point_cloud.laz".
 */
std::string testDataUrl(const std::string &path);

/**
 * @class TestFS
 * @brief This class is used to set up a test file system contained in a zip file.
 */
class TestFS
{
public:
    /**
     * @brief Constructor for TestFS.
     * @param testArchivePath The path of the test archive (zip file).
     * @param baseTestFolder The base test folder for grouping test files. Defaults to "TestFS".
     * @param setCurrentDirectory If true, sets the current directory to the test folder. Defaults to false.
     */
    TestFS(const std::string &testArchivePath, const std::string &baseTestFolder = "TestFS", bool setCurrentDirectory = false);

    /**
     * @brief Destructor for TestFS. Cleans up the test folder and restores the previous working directory.
     */
    ~TestFS();

    /**
     * @brief Clears the cache by deleting the base test folder and its contents.
     * @param baseTestFolder The base test folder to be cleared.
     */
    static void clearCache(const std::string &baseTestFolder);

    /**
     * @brief Downloads a test asset from a URL to a destination path with retry and optional validation.
     * @param url The URL of the file to download.
     * @param destination The destination path to save the downloaded file.
     * @param overwrite If true, overwrites the file if it already exists.
     * @param validator Optional callback to validate the downloaded file. If it returns false, the download is retried.
     * @return The path of the downloaded file.
     */
    static std::filesystem::path downloadTestAsset(
        const std::string &url,
        const std::filesystem::path &destination,
        bool overwrite = false,
        std::function<bool(const std::filesystem::path &)> validator = nullptr);

    /**
     * @brief Folder of the shared download cache: $DDB_TEST_CACHE_DIR, or empty when it is not set.
     *
     * With the cache, every download (TestFS archives, TestArea::downloadTestAsset) is kept once,
     * under the URL's host and path, and copied to the test from there; CI keeps the folder
     * between runs. Without it, downloads stay where they always were (the system temp folder).
     */
    static std::filesystem::path downloadCacheFolder();

    /**
     * @brief Path of a URL in the shared download cache (downloadCacheFolder() must be set).
     */
    static std::filesystem::path cachedAssetPath(const std::string &url);

    /**
     * @brief Path of the test archive (zip file).
     */
    std::string testArchivePath;

    /**
     * @brief Generated test folder (root file system).
     */
    std::string testFolder;

    /**
     * @brief Base test folder for test grouping.
     */
    std::string baseTestFolder;

private:
    std::filesystem::path oldCurrentDirectory_;

    /**
     * @brief Generates a random alphanumeric string.
     * @param length The length of the random string to generate.
     * @return A randomly generated string of the specified length.
     */
    static std::string randomString(size_t length);

    /**
     * @brief Determines if the given path is a local file path.
     * @param path The path to evaluate.
     * @return True if the path is local, otherwise false.
     */
    static bool isLocalPath(const std::string &path);

    /**
     * @brief Extracts the file name from a given path.
     * @param path The full path to extract the file name from.
     * @return The file name extracted from the path.
     */
    static std::string extractFileName(const std::string &path);


};

#endif // TESTFS_H
