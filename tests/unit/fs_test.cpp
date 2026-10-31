/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "fs.h"
#include "mio.h"
#include "logger.h"
#include "test.h"
#include "testarea.h"
#include "exceptions.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <vector>
#include <string>
#include <thread>

namespace
{

    using namespace ddb;

    TEST(pathHasChildren, Normal)
    {
        EXPECT_TRUE(io::Path("/my/path").hasChildren({"/my/path/1", "/my/path/a/b/.."}));

#ifdef _WIN32
        EXPECT_TRUE(io::Path("C:\\my\\path").hasChildren({"C:\\my\\path\\1", "C:\\my\\path\\a\\b\\.."}));
#endif

        EXPECT_TRUE(io::Path("path").hasChildren({"path/1/2", "path/3", "path/././6"}));
        EXPECT_TRUE(io::Path("path/./").hasChildren({"path/1/2", "path/3/", "path/./6/7/../"}));

#ifdef _WIN32
        EXPECT_TRUE(io::Path("path\\.").hasChildren({"path\\1\\2", "path\\3", "path\\4\\"}));
#endif

        EXPECT_TRUE(io::Path("path/./").hasChildren({"path/./../path/a/"}));
        EXPECT_TRUE(io::Path("path/./.").hasChildren({"path/./../path/b"}));

        // A path equal to the parent is now considered contained (issue #453:
        // `ddb add .` from project root should succeed).
        EXPECT_TRUE(io::Path("path").hasChildren({"path/3", "path/a/.."}));
        EXPECT_TRUE(io::Path("path").hasChildren({"path"}));
        EXPECT_TRUE(io::Path("path").hasChildren({"path/."}));
        EXPECT_TRUE(io::Path(".").hasChildren({"."}));

        // Mismatched paths must still be rejected.
        EXPECT_FALSE(io::Path("/my/path").hasChildren({"/my/pat", "/my/path/1"}));

        // Guard against false prefix matches: "/foo" must not contain "/foobar".
        EXPECT_FALSE(io::Path("/foo").hasChildren({"/foobar"}));
        EXPECT_FALSE(io::Path("/foo").hasChildren({"/foo/ok", "/foobar/bad"}));
#ifdef _WIN32
        EXPECT_FALSE(io::Path("C:\\foo").hasChildren({"C:\\foobar"}));
#endif
    }

    TEST(pathDepth, Normal)
    {
        EXPECT_EQ(io::Path("").depth(), 0);

#ifdef _WIN32
        EXPECT_EQ(io::Path("\\").depth(), 0);
#else
        EXPECT_EQ(io::Path("/").depth(), 0);
#endif

        EXPECT_EQ(io::Path(fs::current_path().root_path()).depth(), 0); // C:\ or /
        EXPECT_EQ(io::Path(fs::current_path().root_path() / "file.txt").depth(), 0);
        EXPECT_EQ(io::Path((fs::current_path().root_path() / "a" / "file.txt")).depth(), 1);
        EXPECT_EQ(io::Path((fs::current_path().root_path() / "a" / "b" / "file.txt")).depth(), 2);
        EXPECT_EQ(io::Path(".").depth(), 0);
        EXPECT_EQ(io::Path(fs::path(".") / ".").depth(), 1);
    }

    TEST(pathIsParentOf, Normal)
    {
        EXPECT_TRUE(io::Path("/data/drone").isParentOf("/data/drone/a"));
        EXPECT_FALSE(io::Path("/data/drone").isParentOf("/data/drone/"));
        EXPECT_FALSE(io::Path("/data/drone").isParentOf("/data/drone"));
        EXPECT_FALSE(io::Path("/data/drone/").isParentOf("/data/drone"));
        EXPECT_TRUE(io::Path("data/drone").isParentOf("data/drone/123"));
        EXPECT_FALSE(io::Path("data/drone").isParentOf("data/drone/123/.."));
        EXPECT_FALSE(io::Path("data/drone").isParentOf("data/drone/123/./../"));
        EXPECT_FALSE(io::Path("data/drone").isParentOf("data/drone/123/./../.."));
        EXPECT_TRUE(io::Path("data/drone/a/..").isParentOf("data/drone/123"));
    }

    TEST(pathRelativeTo, Normal)
    {
        EXPECT_EQ(io::Path("/home/test/aaa").relativeTo("/home/test").generic(),
                  io::Path("aaa").generic());
#ifdef _WIN32
        EXPECT_EQ(io::Path("D:/home/test/aaa").relativeTo("/").generic(),
                  io::Path("home/test/aaa").generic());
#elif __APPLE__
        EXPECT_EQ(io::Path("/Users/test/aaa").relativeTo("/").generic(),
                  io::Path("Users/test/aaa").generic());
#else
        EXPECT_EQ(io::Path("/home/test/aaa").relativeTo("/").generic(),
                  io::Path("home/test/aaa").generic());
#endif
        EXPECT_EQ(io::Path("/home/test/aaa/bbb/ccc/../..").relativeTo("/home").generic(),
                  io::Path("test/aaa/").generic());
        EXPECT_EQ(io::Path("/home/test/aaa/").relativeTo("/home").generic(),
                  io::Path("test/aaa").generic());

#ifdef _WIN32
        EXPECT_EQ(io::Path("D:/home/test").relativeTo("/").generic(),
                  io::Path("home/test").generic());
        EXPECT_EQ(io::Path("D:/home/test").relativeTo("D:/").generic(),
                  io::Path("home/test").generic());
        EXPECT_EQ(io::Path("D:/home/test").relativeTo("D:\\").generic(),
                  io::Path("home/test").generic());
#elif __APPLE__
        EXPECT_EQ(io::Path("/Users/test").relativeTo("/").generic(),
                  io::Path("Users/test").generic());
#else
        EXPECT_EQ(io::Path("/home/test").relativeTo("/").generic(),
                  io::Path("home/test").generic());
#endif

#ifdef _WIN32
        EXPECT_EQ(io::Path("D:\\").relativeTo("/").generic(),
                  io::Path("").generic());
#else
        EXPECT_EQ(io::Path("/").relativeTo("/").generic(),
                  io::Path("").generic());
#endif

#ifdef _WIN32
        EXPECT_EQ(io::Path("C:\\a\\..").relativeTo("C:").generic(),
                  io::Path("").generic());
        EXPECT_EQ(io::Path("C:\\").relativeTo("C:\\a\\..").generic(),
                  io::Path("").generic());
#else
        EXPECT_EQ(io::Path("/a/..").relativeTo("/").generic(),
                  io::Path("").generic());
        EXPECT_EQ(io::Path("/").relativeTo("/a/..").generic(),
                  io::Path("").generic());
#endif

#ifdef _WIN32
        EXPECT_EQ(io::Path("C:\\test").relativeTo("/").generic(),
                  io::Path("test").generic());
        EXPECT_EQ(io::Path("D:\\test\\..\\aaa").relativeTo("D:\\").generic(),
                  io::Path("aaa").generic());
#endif
    }

    TEST(pathCheckExtension, Normal)
    {
        EXPECT_TRUE(io::Path("/home/test.JPG").checkExtension({"JPG"}));
        EXPECT_TRUE(io::Path("/home/test.JPG").checkExtension({"jpg"}));
        EXPECT_TRUE(io::Path("/home/test.jpg").checkExtension({"JpG"}));
        EXPECT_TRUE(io::Path("/home/test.jpeg").checkExtension({"JpG", "jpEG"}));
        EXPECT_FALSE(io::Path("/home/test.jpeg").checkExtension({"tif"}));
        EXPECT_FALSE(io::Path("/home/test.jpeg.tif").checkExtension({"JpG", "jpEG"}));
        EXPECT_TRUE(io::Path("/home/test.jpeg.tif").checkExtension({"tif"}));
    }

    TEST(bytesToHuman, Normal)
    {
        EXPECT_EQ(io::bytesToHuman(0), "0 B");
        EXPECT_EQ(io::bytesToHuman(1024), "1 KB");
        EXPECT_EQ(io::bytesToHuman(1048576), "1 MB");

        EXPECT_EQ(io::bytesToHuman(3372220416), "3.14 GB");
    }

    TEST(getModifiedTime, Normal)
    {
        // Works on directories
        EXPECT_TRUE(io::Path(io::getExeFolderPath()).getModifiedTime() > 0);

        // Works on files
        EXPECT_TRUE(io::Path(io::getDataPath("timezone21.bin")).getModifiedTime() > 0);
    }

    TEST(withoutRoot, Normal)
    {
#ifdef _WIN32
        EXPECT_EQ(io::Path("C:\\test\\abc").withoutRoot().get(),
                  io::Path("test\\abc").get());
        EXPECT_EQ(io::Path("D:\\..\\abc").withoutRoot().get(),
                  io::Path("..\\abc").get());
#else
        EXPECT_EQ(io::Path("/test/abc").withoutRoot().get(),
                  io::Path("test/abc").get());
        EXPECT_EQ(io::Path("../abc").withoutRoot().get(),
                  io::Path("../abc").get());
#endif
    }

    TEST(commonDirPath, Normal)
    {
        EXPECT_EQ(io::commonDirPath({"/test/123", "/test/abc"}).generic_string(), "/test");
        EXPECT_EQ(io::commonDirPath({"/test/123", "/test2/abc"}).generic_string(), "/");

        EXPECT_EQ(io::commonDirPath({"test/123", "test2/abc"}).generic_string(), "");
        EXPECT_EQ(io::commonDirPath({"test/123", "test/abc"}).generic_string(), "test");

        EXPECT_EQ(io::commonDirPath({"test/123"}).generic_string(), "test/123");
        EXPECT_EQ(io::commonDirPath({}).generic_string(), "");
        EXPECT_EQ(io::commonDirPath({"abc/abc/test.txt", "abc", "def"}).generic_string(), "");

        EXPECT_EQ(io::commonDirPath({"abc/abc/test.txt", "abc/abc/test2.txt"}).generic_string(), "abc/abc");
        EXPECT_EQ(io::commonDirPath({"/abc"}).generic_string(), "/abc");

#ifdef _WIN32
        EXPECT_EQ(io::commonDirPath({"d:\\test\\123", "d:\\test\\abc"}).string(), "d:\\test");
        EXPECT_EQ(io::commonDirPath({"c:\\test\\123", "c:\\test2\\abc"}).string(), "c:\\");

        EXPECT_EQ(io::commonDirPath({"test\\123", "test2\\abc"}).string(), "");
        EXPECT_EQ(io::commonDirPath({"test\\123", "test\\abc"}).string(), "test");

        EXPECT_EQ(io::commonDirPath({"c:\\test\\123", "d:\\test\\123"}).string(), "");
#endif
    }

    // The paths of the C API are UTF-8, whatever the locale
    TEST(utf8Path, RoundTrip)
    {
        const std::string name = "citt\xC3\xA0_\xC3\xBC_\xE6\x97\xA5.txt"; // "città_ü_日.txt"
        const fs::path p = io::fromUtf8(name);
        EXPECT_EQ(io::toUtf8(p), name);
        // The native form holds the characters, not the bytes read in another code page
        EXPECT_TRUE(p.filename().u8string() == std::u8string(name.begin(), name.end()));
        EXPECT_EQ(io::fromUtf8("a/b") / io::fromUtf8(name), io::fromUtf8("a/b/" + name));
#ifndef _WIN32
        // POSIX: bytes, not validated
        const std::string invalid = "bad\xFF\xFE";
        EXPECT_EQ(io::toUtf8(io::fromUtf8(invalid)), invalid);
#endif
    }

    // The other helpers of mio.h "Path encoding": same characters, whatever the code page
    TEST(utf8Path, Helpers)
    {
        const std::string folder = "citt\xC3\xA0";  // "città"
        const std::string name = "\xC3\xA9.tif";    // "é.tif"
        const fs::path p = io::fromUtf8(folder) / io::fromUtf8(name);

        // '/' separators on every platform
        EXPECT_EQ(io::toGenericUtf8(p), folder + "/" + name);
        EXPECT_EQ(io::toGenericUtf8(io::fromUtf8(folder + "/" + name)), folder + "/" + name);

        // UTF-8 text appended to the last component
        EXPECT_EQ(io::toGenericUtf8(io::concat(p, ".stats_\xC3\xBC.json")), folder + "/" + name + ".stats_\xC3\xBC.json");
        EXPECT_EQ(io::concat(p, ""), p);

        const std::vector<fs::path> paths = io::fromUtf8(std::vector<std::string>{folder, name});
        ASSERT_EQ(paths.size(), 2u);
        EXPECT_EQ(paths[0], io::fromUtf8(folder));
        EXPECT_EQ(paths[1], io::fromUtf8(name));

#ifdef _WIN32
        // UTF-16 for the W functions: "città" is 5 UTF-16 units, 6 UTF-8 bytes
        EXPECT_EQ(io::toWide(folder), std::wstring(L"citt\u00E0"));
        EXPECT_EQ(io::fromWide(L"citt\u00E0"), folder);
#endif
    }

    TEST(utf8Path, EnvironmentVariable)
    {
        const char *name = "DDB_TEST_UTF8_ENV";
        const std::string value = "C:/dati/citt\xC3\xA0_\xE6\x97\xA5";  // "C:/dati/città_日"
#ifdef _WIN32
        ASSERT_EQ(_wputenv_s(io::toWide(name).c_str(), io::toWide(value).c_str()), 0);
#else
        ASSERT_EQ(setenv(name, value.c_str(), 1), 0);
#endif
        EXPECT_EQ(io::getEnvUtf8(name), value);
#ifdef _WIN32
        ASSERT_EQ(_wputenv_s(io::toWide(name).c_str(), L""), 0);
#else
        ASSERT_EQ(unsetenv(name), 0);
#endif
        EXPECT_FALSE(io::getEnvUtf8(name).has_value());
    }

    TEST(utf8Path, ReadAllBytes)
    {
        TestArea ta(TEST_NAME);
        const fs::path file = ta.getFolder() / io::fromUtf8("\xC3\xA4.bin");  // "ä.bin"
        {
            std::ofstream out(file, std::ios::binary);
            out << std::string("a\0b", 3);
        }
        EXPECT_EQ(io::readAllBytes(file), (std::vector<uint8_t>{'a', 0, 'b'}));
        EXPECT_THROW(io::readAllBytes(ta.getFolder() / "missing.bin"), FSException);
    }

    TEST(fileLock, excludesTheOtherThreadsAndKeepsItsFile)
    {
        TestArea ta(TEST_NAME);
        const fs::path shared = ta.getFolder() / "shared";

        // Threads that take the lock again and again: one at a time is inside, and none fails
        // to open the file another one has just let go of
        std::atomic<int> inside{0}, most{0}, failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; t++)
            threads.emplace_back([&] {
                for (int i = 0; i < 25; i++)
                {
                    try
                    {
                        io::FileLock lock(shared);
                        const int now = ++inside;
                        for (int seen = most.load(); now > seen && !most.compare_exchange_weak(seen, now);)
                        {
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        --inside;
                    }
                    catch (const AppException &)
                    {
                        failures++;
                    }
                }
            });
        for (auto &t : threads)
            t.join();

        EXPECT_EQ(failures.load(), 0);
        EXPECT_EQ(most.load(), 1) << "two threads held the lock at once";
        // The file stays for the next one
        EXPECT_TRUE(fs::exists(io::concat(shared, ".lock")));
    }

}
