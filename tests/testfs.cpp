/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testfs.h"
#include "randomutil.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include <cpr/cpr.h>
#include "mzip.h"
#include <utils.h>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <fstream>
#include <functional>

namespace
{
    // Downloads verify the server certificate against the CA bundle libddb ships next to the
    // library (curl-ca-bundle.crt, found like thumbs.cpp and pctiler.cpp find it), so they do not
    // depend on the system store libcurl was built for (none on Windows CI). Without the bundle
    // libcurl's default store is used, still verified.
    cpr::SslOptions verifiedSslOptions()
    {
        auto options = cpr::Ssl(cpr::ssl::VerifyPeer{true}, cpr::ssl::VerifyHost{true});
        const auto caBundle = ddb::io::getDataPath("curl-ca-bundle.crt");
        if (!caBundle.empty())
            options.SetOption(cpr::ssl::CaInfo{caBundle.string()});
        return options;
    }
}

TestFS::TestFS(const std::string &testArchivePath, const std::string &baseTestFolder, bool setCurrentDirectory)
    : testArchivePath(testArchivePath), baseTestFolder(baseTestFolder), oldCurrentDirectory_(std::filesystem::current_path())
{

    // Generate a random test folder path
    testFolder = (std::filesystem::temp_directory_path() / baseTestFolder / ddb::randomString(16)).string();
    std::filesystem::create_directories(testFolder);

    if (!isLocalPath(testArchivePath))
    {
        // Handle remote URL
        const auto tempPath = cachedAssetPath(testArchivePath);
        std::filesystem::create_directories(tempPath.parent_path());
        // A download is only published complete (downloadWith), so an existing archive is whole
        if (!std::filesystem::exists(tempPath))
        {
            std::cout << "Downloading archive...\n";
            // No overwrite: another process may have published it meanwhile, and may be reading it
            downloadTestAsset(testArchivePath, tempPath);
        }
        else
        {
            std::cout << "Using cached archive...\n";
        }
        ddb::zip::extractAll(tempPath.string(), testFolder);
    }
    else
    {
        ddb::zip::extractAll(testArchivePath, testFolder);
    }

    std::cout << "Created test FS '" << testArchivePath << "' in '" << testFolder << "'\n";

    if (setCurrentDirectory)
    {
        std::filesystem::current_path(testFolder);
        std::cout << "Set current directory to '" << testFolder << "'\n";
    }
}

std::string testDataUrl(const std::string &path)
{
    return std::string("https://raw.githubusercontent.com/DroneDB/test_data/") + DDB_TEST_DATA_SHA + "/" + path;
}

std::filesystem::path TestFS::downloadCacheFolder()
{
    const char *folder = std::getenv("DDB_TEST_CACHE_DIR");
    return folder != nullptr && *folder != '\0' ? std::filesystem::path(folder)
                                                : std::filesystem::temp_directory_path() / "ddb_test_cache";
}

std::filesystem::path TestFS::cachedAssetPath(const std::string &url)
{
    const auto scheme = url.find("://");
    const auto hostAndPath = scheme == std::string::npos ? url : url.substr(scheme + 3);
    return downloadCacheFolder() / std::filesystem::path(hostAndPath).relative_path();
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
 * @brief Downloads a test asset from a URL to a specified destination.
 * @param url The URL of the file to download.
 * @param destination The destination path to save the downloaded file.
 * @param overwrite If true, overwrites the file if it already exists.
 * @return The path of the downloaded file.
 */
std::filesystem::path TestFS::downloadTestAsset(const std::string &url, const std::filesystem::path &destination, bool overwrite, std::function<bool(const std::filesystem::path &)> validator)
{
    const DownloadTransfer transfer = [&url](std::ofstream &body) -> std::string
    {
        const auto response = cpr::Download(body, cpr::Url{url}, cpr::VerifySsl(true), verifiedSslOptions());
        if (response.error)
            return "network error: " + response.error.message;
        if (response.status_code != 200)
            return "HTTP " + std::to_string(response.status_code);
        return {};
    };
    return downloadWith(url, transfer, destination, overwrite, validator);
}

namespace
{
    // Removes the file of a download attempt unless it was published
    class PartFile
    {
    public:
        explicit PartFile(const std::filesystem::path &destination)
            : path_(destination.string() + ".part-" + ddb::randomString(16)) {}
        ~PartFile()
        {
            std::error_code e;
            if (!published_)
                std::filesystem::remove(path_, e);
        }
        PartFile(const PartFile &) = delete;
        PartFile &operator=(const PartFile &) = delete;

        const std::filesystem::path &path() const { return path_; }

        // Same pattern as ensureEmptyIndexTemplate (index_lifecycle.cpp): a rename that fails because the
        // destination exists (Windows does not replace it) means another process published it.
        void publish(const std::filesystem::path &destination)
        {
            std::error_code e;
            std::filesystem::rename(path_, destination, e);
            if (!e)
            {
                published_ = true;
                return;
            }
            if (!std::filesystem::exists(destination))
                throw ddb::FSException("Cannot publish " + destination.string() + " (" + e.message() + ")");
            LOGD << destination << " was published by another process";
        }

    private:
        std::filesystem::path path_;
        bool published_ = false;
    };
}

std::filesystem::path TestFS::downloadWith(const std::string &url, const DownloadTransfer &transfer,
                                           const std::filesystem::path &destination, bool overwrite,
                                           const std::function<bool(const std::filesystem::path &)> &validator,
                                           const DownloadRetry &retry)
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
        fs::remove(destination);
    }

    std::string failure;
    for (int attempt = 1; attempt <= retry.maxAttempts; ++attempt)
    {
        if (attempt > 1)
        {
            const auto delay = retry.firstDelay * (1 << (attempt - 2));
            LOGD << failure << ", retrying in " << delay.count() << "ms...";
            std::this_thread::sleep_for(delay);
        }
        LOGD << "Downloading " << url << " (attempt " << attempt << "/" << retry.maxAttempts << ")";

        PartFile part(destination);
        {
            std::ofstream body(part.path().string(), std::ios::binary);
            if (!body)
                throw ddb::FSException("Cannot create file: " + part.path().string());

            failure = transfer(body);
            body.close();
            if (failure.empty() && body.fail())
                throw ddb::FSException("Cannot write file: " + part.path().string());
        }

        if (failure.empty() && validator && !validator(part.path()))
            failure = "downloaded file failed validation";
        if (!failure.empty())
            continue;

        part.publish(destination);
        LOGD << "Downloaded " << url << " to " << destination;
        return destination;
    }

    throw ddb::NetException("Failed to download " + url + " to " + destination.string() + " after " +
                            std::to_string(retry.maxAttempts) + " attempts: " + failure);
}
