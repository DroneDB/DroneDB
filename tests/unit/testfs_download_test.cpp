/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// TestFS::downloadWith, the one implementation of the test downloads (and of the shared
// DDB_TEST_CACHE_DIR cache): a download is only ever published complete, so an interrupted or
// failed one can never be reused as a cached asset. Offline: the transfers are injected.

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

namespace
{
    const TestFS::DownloadRetry noDelay{2, std::chrono::milliseconds(0)};
    const std::string url = "https://example.invalid/asset.zip";

    std::string readFile(const fs::path &path)
    {
        std::ifstream in(path.string(), std::ios::binary);
        std::stringstream content;
        content << in.rdbuf();
        return content.str();
    }

    void writeFile(const fs::path &path, const std::string &content)
    {
        std::ofstream out(path.string(), std::ios::binary);
        out << content;
    }

    // The "<destination>.part-*" files next to the destination
    std::vector<fs::path> partFiles(const fs::path &destination)
    {
        std::vector<fs::path> parts;
        const auto prefix = destination.filename().string() + ".part-";
        for (const auto &entry : fs::directory_iterator(destination.parent_path()))
            if (entry.path().filename().string().rfind(prefix, 0) == 0)
                parts.push_back(entry.path());
        return parts;
    }

    // Writes a truncated body, then fails as a dropped connection does
    std::string interrupted(std::ofstream &body)
    {
        body << "PK\x03\x04 truncat";
        return "network error: connection reset";
    }

    TEST(testfsDownload, interruptedTransferLeavesNoDestination)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.zip";

        int attempts = 0;
        const auto transfer = [&attempts](std::ofstream &body)
        {
            ++attempts;
            return interrupted(body);
        };
        EXPECT_THROW(TestFS::downloadWith(url, transfer, destination, false, nullptr, noDelay), ddb::NetException);

        EXPECT_EQ(attempts, noDelay.maxAttempts);
        EXPECT_FALSE(fs::exists(destination));
        EXPECT_TRUE(partFiles(destination).empty());
    }

    TEST(testfsDownload, transferThatThrowsLeavesNoDestination)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.zip";

        const auto transfer = [](std::ofstream &body) -> std::string
        {
            body << "partial";
            body.flush();
            throw std::runtime_error("killed mid-transfer");
        };
        EXPECT_THROW(TestFS::downloadWith(url, transfer, destination, false, nullptr, noDelay), std::runtime_error);

        EXPECT_FALSE(fs::exists(destination));
        EXPECT_TRUE(partFiles(destination).empty());
    }

    TEST(testfsDownload, rejectedDownloadLeavesNoDestination)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.sqlite";

        std::vector<fs::path> validated;
        const auto validator = [&validated](const fs::path &path)
        {
            validated.push_back(path);
            return false;
        };
        const auto transfer = [](std::ofstream &body)
        {
            body << "<html>not a database</html>";
            return std::string();
        };
        EXPECT_THROW(TestFS::downloadWith(url, transfer, destination, false, validator, noDelay), ddb::NetException);

        // The validator saw each attempt's own file, never the destination
        ASSERT_EQ(validated.size(), static_cast<size_t>(noDelay.maxAttempts));
        EXPECT_NE(validated[0], destination);
        EXPECT_NE(validated[0], validated[1]);
        EXPECT_FALSE(fs::exists(destination));
        EXPECT_TRUE(partFiles(destination).empty());
    }

    TEST(testfsDownload, leftoverPartFileIsNeverUsed)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.zip";
        // What a killed download leaves behind
        const auto leftover = fs::path(destination.string() + ".part-leftover0000");
        writeFile(leftover, "truncated");

        EXPECT_THROW(TestFS::downloadWith(url, interrupted, destination, false, nullptr, noDelay), ddb::NetException);
        EXPECT_FALSE(fs::exists(destination));

        const auto complete = [](std::ofstream &body)
        {
            body << "complete";
            return std::string();
        };
        EXPECT_EQ(TestFS::downloadWith(url, complete, destination, false, nullptr, noDelay), destination);
        EXPECT_EQ(readFile(destination), "complete");

        // Not removed either: it may be the file of a download in progress in another process
        EXPECT_EQ(readFile(leftover), "truncated");
        EXPECT_EQ(partFiles(destination), std::vector<fs::path>{leftover});
    }

    TEST(testfsDownload, publishesOnlyTheSuccessfulAttempt)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.zip";

        int attempts = 0;
        const auto transfer = [&](std::ofstream &body)
        {
            // Every attempt writes to its own file; the destination appears only when published
            EXPECT_FALSE(fs::exists(destination));
            if (++attempts == 1)
                return interrupted(body);
            body << "complete";
            return std::string();
        };
        EXPECT_EQ(TestFS::downloadWith(url, transfer, destination, false, nullptr, noDelay), destination);

        EXPECT_EQ(attempts, 2);
        EXPECT_EQ(readFile(destination), "complete");
        EXPECT_TRUE(partFiles(destination).empty());
    }

    TEST(testfsDownload, publishedDownloadIsReused)
    {
        TestArea ta(TEST_NAME, true);
        const auto destination = ta.getFolder() / "asset.zip";
        writeFile(destination, "published");

        int attempts = 0;
        const auto transfer = [&attempts](std::ofstream &body)
        {
            ++attempts;
            body << "new";
            return std::string();
        };
        EXPECT_EQ(TestFS::downloadWith(url, transfer, destination, false, nullptr, noDelay), destination);
        EXPECT_EQ(attempts, 0);
        EXPECT_EQ(readFile(destination), "published");

        // overwrite replaces it with a new complete download
        EXPECT_EQ(TestFS::downloadWith(url, transfer, destination, true, nullptr, noDelay), destination);
        EXPECT_EQ(attempts, 1);
        EXPECT_EQ(readFile(destination), "new");
    }
}
