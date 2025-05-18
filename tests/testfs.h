/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TESTFS_H
#define TESTFS_H

#include <string>
#include "fs.h"

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

    /**
     * @brief Downloads a test asset from a URL to a specified destination.
     * @param url The URL of the file to download.
     * @param destination The destination path to save the downloaded file.
     * @param overwrite If true, overwrites the file if it already exists.
     * @return The path of the downloaded file.
     */
    static std::filesystem::path downloadTestAsset(const std::string &url, const std::string &destination, bool overwrite);
};

#endif // TESTFS_H
