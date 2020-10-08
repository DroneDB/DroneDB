/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "fs.h"
#include "mio.h"
#include "logger.h"
#include <vector>
#include <string>

namespace {

using namespace ddb;

TEST(pathHasChildren, Normal) {
    EXPECT_TRUE(io::Path("/my/path").hasChildren({ "/my/path/1", "/my/path/a/b/.." }));

#ifdef _WIN32
    EXPECT_TRUE(io::Path("C:\\my\\path").hasChildren({ "C:\\my\\path\\1", "C:\\my\\path\\a\\b\\.." }));
#endif

    EXPECT_TRUE(io::Path("path").hasChildren({"path/1/2", "path/3", "path/././6"}));
    EXPECT_TRUE(io::Path("path/./").hasChildren({ "path/1/2", "path/3/", "path/./6/7/../" }));

#ifdef _WIN32
    EXPECT_TRUE(io::Path("path\\.").hasChildren({ "path\\1\\2", "path\\3", "path\\4\\" }));
#endif

    EXPECT_TRUE(io::Path("path/./").hasChildren({"path/./../path/a/"}));
    EXPECT_TRUE(io::Path("path/./.").hasChildren({"path/./../path/b"}));

    EXPECT_FALSE(io::Path("path").hasChildren({"path/3", "path/a/.."}));
    EXPECT_FALSE(io::Path("/my/path").hasChildren({"/my/pat", "/my/path/1"}));
}

TEST(pathDepth, Normal) {
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

TEST(pathIsParentOf, Normal){
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

TEST(pathRelativeTo, Normal) {
    EXPECT_EQ(io::Path("/home/test/aaa").relativeTo("/home/test").generic(),
              io::Path("aaa").generic());
#ifdef _WIN32
    EXPECT_EQ(io::Path("D:/home/test/aaa").relativeTo("/").generic(),
              io::Path("home/test/aaa").generic());
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

TEST(pathCheckExtension, Normal) {
	EXPECT_TRUE(io::Path("/home/test.JPG").checkExtension({ "JPG" }));
	EXPECT_TRUE(io::Path("/home/test.JPG").checkExtension({ "jpg" }));
	EXPECT_TRUE(io::Path("/home/test.jpg").checkExtension({ "JpG" }));
	EXPECT_TRUE(io::Path("/home/test.jpeg").checkExtension({ "JpG", "jpEG" }));
	EXPECT_FALSE(io::Path("/home/test.jpeg").checkExtension({ "tif" }));
	EXPECT_FALSE(io::Path("/home/test.jpeg.tif").checkExtension({ "JpG", "jpEG" }));
	EXPECT_TRUE(io::Path("/home/test.jpeg.tif").checkExtension({ "tif" }));
}

TEST(bytesToHuman, Normal) {
    EXPECT_EQ(io::bytesToHuman(0), "0 B");
    EXPECT_EQ(io::bytesToHuman(1024), "1 KB");
    EXPECT_EQ(io::bytesToHuman(1048576), "1 MB");

    EXPECT_EQ(io::bytesToHuman(3372220416), "3.14 GB");
}

TEST(getModifiedTime, Normal) {
    // Works on directories
    EXPECT_TRUE(io::Path(io::getExeFolderPath()).getModifiedTime() > 0);

    // Works on files
    EXPECT_TRUE(io::Path(io::getDataPath("timezone21.bin")).getModifiedTime() > 0);
}

TEST(withoutRoot, Normal){
#ifdef _WIN32
    EXPECT_EQ(io::Path("C:\\test\\abc").withoutRoot().string(),
              io::Path("test\\abc").string());
    EXPECT_EQ(io::Path("D:\\..\\abc").withoutRoot().string(),
        io::Path("..\\abc").string());
#else
    EXPECT_EQ(io::Path("/test/abc").withoutRoot().string(),
        io::Path("test/abc").string());
    EXPECT_EQ(io::Path("../abc").withoutRoot().string(),
        io::Path("../abc").string());
#endif
}

TEST(commonDirPath, Normal){
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

}
