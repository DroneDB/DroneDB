/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TESTFS_H
#define TESTFS_H

#include <chrono>
#include <fstream>
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
 * @brief How often, and how patiently, a failed test download is retried (the delay doubles).
 *
 * TestFS::DownloadRetry; outside the class so it can be a default argument of its members.
 */
struct TestDownloadRetry
{
    int maxAttempts = 7;
    std::chrono::milliseconds firstDelay{1000};
};

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
     * @brief One transfer attempt of a download: writes the body to the stream it is given.
     * @return Empty on success, otherwise the reason of the failure (the attempt is retried).
     */
    using DownloadTransfer = std::function<std::string(std::ofstream &body)>;

    using DownloadRetry = TestDownloadRetry;

    /**
     * @brief downloadTestAsset with the transfer given: the one implementation of every download.
     *
     * Every attempt writes to a new "<destination>.part-<random>" file next to the destination,
     * which is published with a rename only once the transfer succeeded and the validator
     * accepted it, and removed otherwise. So the destination only ever holds a complete download:
     * a killed or failed download leaves no destination behind, and an existing destination
     * (the shared cache, other processes) can be reused as it is.
     * @param url Source of the transfer, for messages only.
     */
    static std::filesystem::path downloadWith(
        const std::string &url,
        const DownloadTransfer &transfer,
        const std::filesystem::path &destination,
        bool overwrite = false,
        const std::function<bool(const std::filesystem::path &)> &validator = nullptr,
        const DownloadRetry &retry = {});

    /**
     * @brief Folder of the shared download cache: $DDB_TEST_CACHE_DIR, else <temp>/ddb_test_cache.
     *
     * Every download (TestFS archives, TestArea::downloadTestAsset) is kept once, under the URL's
     * host and path, and copied to the test from there: tests run in processes of their own
     * (TestArea::processRoot), so it is the one place downloads outlive a test; CI keeps the
     * folder between runs.
     */
    static std::filesystem::path downloadCacheFolder();

    /**
     * @brief Path of a URL in the shared download cache (under downloadCacheFolder()).
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

};

#endif // TESTFS_H
