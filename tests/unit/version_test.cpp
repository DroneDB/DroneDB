/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The version string comes from one CMake-generated TU (ddb::version(),
// ddb::revision()) instead of global -DAPP_VERSION/-DAPP_REVISION defines on every compile
// line. DDBGetVersion keeps its signature and its "X.Y.Z (git commit abc)" / "X.Y.Z" format.
// DDB_PACKAGE_JSON is a compile definition of this source file only (tests/CMakeLists.txt).

#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "ddb.h"
#include "version.h"

namespace
{
    // Same rule as the root CMakeLists.txt: the first "version": "X.Y.Z" in package.json
    std::string packageJsonVersion()
    {
        std::ifstream in(DDB_PACKAGE_JSON);
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string json = ss.str();
        std::smatch m;
        static const std::regex re("\"version\"\\s*:\\s*\"([0-9]+\\.[0-9]+\\.[0-9]+)\"");
        return std::regex_search(json, m, re) ? m[1].str() : std::string();
    }

    bool isShortGitHash(const std::string &s)
    {
        if (s.size() < 4)
            return false;
        for (const char c : s)
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return false;
        return true;
    }

    TEST(Version, Target_DDBGetVersionReturnsLibraryVersion)
    {
        ASSERT_NE(DDBGetVersion(), nullptr);
        EXPECT_STREQ(DDBGetVersion(), ddb::version());
    }

    TEST(Version, Target_RevisionIsShortHashOrDev)
    {
        ASSERT_NE(ddb::revision(), nullptr);
        const std::string revision = ddb::revision();
        EXPECT_FALSE(revision.empty());
        EXPECT_TRUE(revision == "dev" || isShortGitHash(revision)) << revision;
    }

    // Pins today's format: "X.Y.Z (git commit <revision>)", or "X.Y.Z" without git
    TEST(Version, Target_VersionMatchesPackageJsonAndRevision)
    {
        const std::string expected = packageJsonVersion();
        ASSERT_FALSE(expected.empty()) << "cannot read the version from " << DDB_PACKAGE_JSON;

        const std::string version = ddb::version();
        const std::string revision = ddb::revision();
        if (revision == "dev")
            EXPECT_EQ(version, expected);
        else
            EXPECT_EQ(version, expected + " (git commit " + revision + ")");
    }
}
