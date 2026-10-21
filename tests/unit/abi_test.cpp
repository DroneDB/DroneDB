/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// XI-01: the ABI handshake. DDBGetAbiVersion() encodes DDB_ABI_MAJOR/DDB_ABI_MINOR, and
// DDBGetCapabilities() returns the version, the error codes, the cancellable exports and the
// symbol manifest (src/include/ddb.symbols, embedded at build time). Both are exempt accessors:
// they never change the last error. scripts/check-abi.py checks the same manifest against the
// symbols the built library exports (CI); the capabilities JSON shape is a contract fixture
// (ContractFixturesCapabilities, tests/contract).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"

namespace
{
    struct Manifest
    {
        std::vector<std::string> symbols; // in file order
        int major = -1;                   // the '# abi MAJOR.MINOR' line
        int minor = -1;
    };

    // src/include/ddb.symbols (DDB_SYMBOLS_MANIFEST, tests/CMakeLists.txt)
    Manifest readManifest()
    {
        Manifest manifest;
        std::ifstream in(DDB_SYMBOLS_MANIFEST);
        EXPECT_TRUE(in.good()) << DDB_SYMBOLS_MANIFEST;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (line[0] == '#')
            {
                int major = 0, minor = 0;
                if (std::sscanf(line.c_str(), "# abi %d.%d", &major, &minor) == 2)
                {
                    manifest.major = major;
                    manifest.minor = minor;
                }
                continue;
            }
            manifest.symbols.push_back(line);
        }
        return manifest;
    }

    json capabilities()
    {
        const char *text = DDBGetCapabilities();
        EXPECT_NE(text, nullptr);
        return json::parse(text != nullptr ? text : "null");
    }

    TEST(AbiVersion, encodesTheHeaderVersion)
    {
        const uint32_t version = DDBGetAbiVersion();
        EXPECT_EQ(version >> 16, static_cast<uint32_t>(DDB_ABI_MAJOR));
        EXPECT_EQ(version & 0xFFFFu, static_cast<uint32_t>(DDB_ABI_MINOR));
        // The handshake starts at ABI 1.1: 1.0 is every release without it
        EXPECT_EQ(DDB_ABI_MAJOR, 1);
        EXPECT_GE(DDB_ABI_MINOR, 1);
        EXPECT_GE(version, 0x00010001u);
    }

    TEST(AbiVersion, manifestDeclaresTheHeaderVersion)
    {
        const Manifest manifest = readManifest();
        EXPECT_EQ(manifest.major, DDB_ABI_MAJOR);
        EXPECT_EQ(manifest.minor, DDB_ABI_MINOR);
    }

    TEST(Capabilities, reportsTheAbiAndVersion)
    {
        const json caps = capabilities();
        ASSERT_TRUE(caps.is_object()) << caps.dump();
        EXPECT_EQ(caps["abi"], json({{"major", DDB_ABI_MAJOR}, {"minor", DDB_ABI_MINOR}}));
        EXPECT_EQ(caps["version"], DDBGetVersion());
    }

    TEST(Capabilities, symbolsEqualTheManifest)
    {
        const json caps = capabilities();
        const Manifest manifest = readManifest();
        ASSERT_FALSE(manifest.symbols.empty());
        EXPECT_EQ(caps["symbols"], json(manifest.symbols));
        // The handshake exports list themselves
        const std::set<std::string> symbols(manifest.symbols.begin(), manifest.symbols.end());
        EXPECT_EQ(symbols.size(), manifest.symbols.size()) << "duplicate symbols";
        EXPECT_EQ(symbols.count("DDBGetAbiVersion"), 1u);
        EXPECT_EQ(symbols.count("DDBGetCapabilities"), 1u);
    }

    TEST(Capabilities, errorCodesMatchTheEnum)
    {
        json expected = json::object();
#define EXPECT_CODE(name) expected[#name] = DDBERR_##name
        EXPECT_CODE(NONE);
        EXPECT_CODE(EXCEPTION);
        EXPECT_CODE(BUILDDEPMISSING);
        EXPECT_CODE(BUILDINPROGRESS);
        EXPECT_CODE(CANCELED);
        EXPECT_CODE(BUSY);
        EXPECT_CODE(NOTFOUND);
        EXPECT_CODE(INVALIDARGS);
        EXPECT_CODE(RETRYABLE);
        EXPECT_CODE(SHUTDOWN);
#undef EXPECT_CODE
        EXPECT_EQ(capabilities()["errorCodes"], expected);
    }

    // The reasons DDBGetLastErrorReason returns (ABI 1.1): sorted, unique, the documented names
    TEST(Capabilities, errorReasonsAreTheDocumentedNames)
    {
        const json reasons = capabilities()["errorReasons"];
        ASSERT_TRUE(reasons.is_array()) << reasons.dump();
        EXPECT_EQ(reasons, json({"idleTimeout", "lowDiskSpace", "noSpaceOnDevice"}));
    }

    // The exports that honour a cancel token (each one is called with a canceled token in
    // c_abi_cancel_test.cpp): exports, sorted, DDBExportRaster2 (progress callback) among them
    TEST(Capabilities, cancellableAreSortedExports)
    {
        const json caps = capabilities();
        const auto &symbols = caps["symbols"];
        ASSERT_TRUE(caps["cancellable"].is_array());
        for (const auto &name : caps["cancellable"])
            EXPECT_NE(std::find(symbols.begin(), symbols.end(), name), symbols.end()) << name;
        EXPECT_TRUE(std::is_sorted(caps["cancellable"].begin(), caps["cancellable"].end()));
        EXPECT_NE(std::find(caps["cancellable"].begin(), caps["cancellable"].end(), "DDBExportRaster2"),
                  caps["cancellable"].end());
    }

    // Built once: every call and every thread gets the same string, valid for the process
    TEST(Capabilities, sameStringOnEveryThread)
    {
        constexpr int kThreads = 8;
        std::vector<const char *> seen(kThreads, nullptr);
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; i++)
            threads.emplace_back([&seen, i] { seen[static_cast<size_t>(i)] = DDBGetCapabilities(); });
        for (auto &t : threads)
            t.join();
        ASSERT_NE(seen[0], nullptr);
        for (const char *p : seen)
            EXPECT_EQ(p, seen[0]);
        EXPECT_EQ(DDBGetCapabilities(), seen[0]);
    }

    // Exempt accessors: a caller reads the handshake between a failing call and DDBGetLastError()
    TEST(Capabilities, accessorsKeepTheLastError)
    {
        char *out = nullptr;
        ASSERT_EQ(DDBInit(nullptr, &out), DDBERR_EXCEPTION);
        const std::string error = DDBGetLastError();
        ASSERT_FALSE(error.empty());

        EXPECT_NE(DDBGetAbiVersion(), 0u);
        EXPECT_NE(DDBGetCapabilities(), nullptr);
        EXPECT_EQ(DDBGetLastError(), error);
        // A second call (the string is built already) does not touch it either
        EXPECT_NE(DDBGetCapabilities(), nullptr);
        EXPECT_EQ(DDBGetLastError(), error);
    }
}
