/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include <algorithm>
#include <fstream>
#include <ctime>

#include "constants.h"
#include "dbops.h"
#include "exceptions.h"
#include "fs.h"
#include "ddb.h"

namespace
{

    using namespace ddb;

    // Builds a unique sandbox under a fresh subdirectory of the system temp.
    // The directory is removed when the fixture goes out of scope.
    class GlobSandbox
    {
    public:
        GlobSandbox()
        {
            const auto base = fs::temp_directory_path() / "ddb_glob_test";
            unsigned counter = 0;
            do
            {
                root_ = base / ("run_" + std::to_string(::time(nullptr)) +
                                "_" + std::to_string(counter++));
            } while (fs::exists(root_));
            fs::create_directories(root_);

            previous_ = fs::current_path();
            fs::current_path(root_);
        }

        ~GlobSandbox()
        {
            std::error_code ec;
            fs::current_path(previous_, ec);
            fs::remove_all(root_, ec);
        }

        const fs::path &root() const { return root_; }

        void touch(const std::string &rel)
        {
            const auto p = root_ / rel;
            fs::create_directories(p.parent_path());
            std::ofstream(p.string()).close();
        }

        void mkdir(const std::string &rel)
        {
            fs::create_directories(root_ / rel);
        }

    private:
        fs::path root_;
        fs::path previous_;
    };

    bool containsName(const std::vector<std::string> &paths,
                      const std::string &filename)
    {
        return std::any_of(paths.begin(), paths.end(),
                           [&](const std::string &p)
                           {
                               return fs::path(p).filename().string() == filename;
                           });
    }

    TEST(expandGlobPatterns, LiteralFile)
    {
        GlobSandbox sb;
        sb.touch("a.JPG");

        auto out = expandGlobPatterns({"a.JPG"});
        ASSERT_EQ(out.size(), 1u);
        EXPECT_TRUE(fs::path(out[0]).is_absolute());
        EXPECT_EQ(fs::path(out[0]).filename().string(), "a.JPG");
    }

    TEST(expandGlobPatterns, MissingLiteralThrows)
    {
        GlobSandbox sb;
        EXPECT_THROW(expandGlobPatterns({"nope.txt"}), FSException);
    }

    TEST(expandGlobPatterns, SimpleStarPattern)
    {
        GlobSandbox sb;
        sb.touch("a.JPG");
        sb.touch("b.JPG");
        sb.touch("readme.txt");

        auto out = expandGlobPatterns({"*.JPG"});
        EXPECT_EQ(out.size(), 2u);
        EXPECT_TRUE(containsName(out, "a.JPG"));
        EXPECT_TRUE(containsName(out, "b.JPG"));
        EXPECT_FALSE(containsName(out, "readme.txt"));
    }

    TEST(expandGlobPatterns, RecursiveDoubleStar)
    {
        GlobSandbox sb;
        sb.touch("a.JPG");
        sb.touch("nested/b.JPG");
        sb.touch("nested/deep/c.JPG");
        sb.touch("nested/skip.txt");

        auto out = expandGlobPatterns({"**/*.JPG"});
        EXPECT_GE(out.size(), 3u);
        EXPECT_TRUE(containsName(out, "a.JPG"));
        EXPECT_TRUE(containsName(out, "b.JPG"));
        EXPECT_TRUE(containsName(out, "c.JPG"));
        EXPECT_FALSE(containsName(out, "skip.txt"));
    }

    TEST(expandGlobPatterns, BareDirectoryRecursive)
    {
        GlobSandbox sb;
        sb.touch("images/a.JPG");
        sb.touch("images/sub/b.JPG");

        auto out = expandGlobPatterns({"images"});
        EXPECT_TRUE(containsName(out, "a.JPG"));
        EXPECT_TRUE(containsName(out, "b.JPG"));
    }

    TEST(expandGlobPatterns, SkipsDdbFolder)
    {
        GlobSandbox sb;
        sb.touch("images/a.JPG");
        sb.touch("images/sub/b.JPG");
        sb.touch(std::string(DDB_FOLDER) + "/index.db");
        sb.touch(std::string(DDB_FOLDER) + "/build/something.json");

        // Bare-directory expansion walks recursively and must skip .ddb.
        auto outDir = expandGlobPatterns({"."});
        EXPECT_FALSE(outDir.empty());
        for (const auto &p : outDir)
        {
            EXPECT_TRUE(fs::path(p).string().find(DDB_FOLDER) == std::string::npos)
                << "Result leaked .ddb path: " << p;
        }

        // Glob expansion must also skip .ddb entries.
        auto outGlob = expandGlobPatterns({"**/*"});
        for (const auto &p : outGlob)
        {
            EXPECT_TRUE(fs::path(p).string().find(DDB_FOLDER) == std::string::npos)
                << "Result leaked .ddb path: " << p;
        }
    }

    TEST(expandGlobPatterns, NoMatchThrowsWhenAllPatternsEmpty)
    {
        GlobSandbox sb;
        sb.touch("a.JPG");

        EXPECT_THROW(expandGlobPatterns({"*.NOPE"}), FSException);
    }

    TEST(expandGlobPatterns, EmptyPatternThrows)
    {
        EXPECT_THROW(expandGlobPatterns({""}), InvalidArgsException);
    }

    TEST(expandGlobPatterns, DeduplicatesAcrossPatterns)
    {
        GlobSandbox sb;
        sb.touch("a.JPG");

        auto out = expandGlobPatterns({"*.JPG", "a.JPG"});
        EXPECT_EQ(out.size(), 1u);
    }

} // namespace
