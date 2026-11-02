/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <zip.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "build.h"
#include "build_internal.h"
#include "dbops.h"
#include "exceptions.h"
#include "mzip.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{

    using namespace ddb;

    TEST(zip, extractAllRestoresTheArchivedTree)
    {
        TestArea ta(TEST_NAME);

        fs::path dir = ta.getFolder("zipTest");
        {
            std::ofstream output((dir / "a.txt").string());
        }
        fs::create_directories(dir / "subdir");
        {
            std::ofstream output((dir / "subdir" / "b.txt").string());
        }

        std::string zipFile = (ta.getFolder(".") / "archive.zip").string();
        fs::remove(zipFile);
        zipFolder(dir.string(), zipFile);

        fs::path outdir = ta.getFolder("zipOutput");
        ddb::zip::extractAll(zipFile, outdir.string());

        EXPECT_TRUE(fs::is_regular_file(outdir / "a.txt"));
        EXPECT_TRUE(fs::is_directory(outdir / "subdir"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "subdir" / "b.txt"));
    }

    // ---- Archives built in the test ----

    // A member of a test archive: a name and its bytes, or `zeros` zero bytes produced on the fly
    // (a gigabyte of them without holding it in memory), with an optional Unix mode.
    struct Member
    {
        std::string name;
        std::string content;
        uint64_t zeros = 0;
        uint32_t unixMode = 0;
    };

    // A libzip source that delivers `remaining` zero bytes: it compresses to a thousandth of its size.
    struct ZeroSource
    {
        uint64_t size;
        uint64_t remaining;
    };

    zip_int64_t zeroSourceCallback(void *state, void *data, zip_uint64_t len, zip_source_cmd_t cmd)
    {
        auto *z = static_cast<ZeroSource *>(state);
        switch (cmd)
        {
        case ZIP_SOURCE_OPEN:
            z->remaining = z->size;
            return 0;
        case ZIP_SOURCE_READ:
        {
            const zip_uint64_t n = std::min<zip_uint64_t>(len, z->remaining);
            std::memset(data, 0, n);
            z->remaining -= n;
            return static_cast<zip_int64_t>(n);
        }
        case ZIP_SOURCE_CLOSE:
            return 0;
        case ZIP_SOURCE_STAT:
        {
            auto *st = static_cast<zip_stat_t *>(data);
            zip_stat_init(st);
            st->size = z->size;
            st->valid = ZIP_STAT_SIZE;
            return sizeof(zip_stat_t);
        }
        case ZIP_SOURCE_FREE:
            delete z;
            return 0;
        case ZIP_SOURCE_SUPPORTS:
            return zip_source_make_command_bitmap(ZIP_SOURCE_OPEN, ZIP_SOURCE_READ, ZIP_SOURCE_CLOSE,
                                                  ZIP_SOURCE_STAT, ZIP_SOURCE_FREE, -1);
        default:
            return -1;
        }
    }

    // Writes a ZIP archive with the members, in the given order, deflated; the member names
    // are stored as given (libzip does not normalise them).
    void writeZip(const fs::path &path, const std::vector<Member> &members)
    {
        fs::remove(path);
        int error = 0;
        zip_t *archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        ASSERT_NE(archive, nullptr) << "libzip error " << error;

        for (const auto &m : members)
        {
            zip_source_t *source = m.zeros > 0
                                       ? zip_source_function(archive, zeroSourceCallback, new ZeroSource{m.zeros, m.zeros})
                                       : zip_source_buffer(archive, m.content.data(), m.content.size(), 0);
            ASSERT_NE(source, nullptr) << zip_strerror(archive);
            const zip_int64_t index = zip_file_add(archive, m.name.c_str(), source, ZIP_FL_ENC_UTF_8);
            if (index < 0)
                zip_source_free(source);
            ASSERT_GE(index, 0) << zip_strerror(archive);
            if (m.zeros > 0)
            {
                ASSERT_EQ(zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 9), 0);
            }
            if (m.unixMode != 0)
            {
                ASSERT_EQ(zip_file_set_external_attributes(archive, static_cast<zip_uint64_t>(index), 0, ZIP_OPSYS_UNIX,
                                                           m.unixMode << 16),
                          0);
            }
        }

        ASSERT_EQ(zip_close(archive), 0) << zip_strerror(archive);
    }

    // Makes the archive lie about its first member: the size in its local header and in its
    // central directory entry become `size`, whatever the compressed data holds.
    void declareSizeOfFirstMember(const fs::path &path, uint32_t size)
    {
        std::string bytes;
        {
            std::ifstream in(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        const auto put = [&bytes, size](size_t offset)
        {
            for (size_t i = 0; i < 4; ++i)
                bytes[offset + i] = static_cast<char>((size >> (8 * i)) & 0xFF);
        };

        ASSERT_EQ(bytes.compare(0, 4, "PK\x03\x04"), 0);
        put(22); // uncompressed size of the local header
        const size_t central = bytes.rfind("PK\x01\x02");
        ASSERT_NE(central, std::string::npos);
        put(central + 24); // uncompressed size of the central directory entry

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // Number of files and folders below dir (0 when it does not exist)
    size_t countItems(const fs::path &dir)
    {
        if (!fs::exists(dir))
            return 0;
        return static_cast<size_t>(std::distance(fs::recursive_directory_iterator(dir), fs::recursive_directory_iterator()));
    }

    uintmax_t bytesBelow(const fs::path &dir)
    {
        uintmax_t total = 0;
        if (fs::exists(dir))
            for (const auto &item : fs::recursive_directory_iterator(dir))
                if (item.is_regular_file())
                    total += item.file_size();
        return total;
    }

    constexpr uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;

    // ---- Expansion limits ----

    // A zip bomb: a gigabyte of zeros deflates to about a megabyte. It is refused from the
    // central directory, so not a byte (nor the output folder) is written.
    TEST(zip, extractAllRefusesAZipBombBeforeWritingAnything)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("bomb.zip");
        writeZip(zipFile, {{"tileset.json", "{}", 0, 0}, {"bomb.bin", "", GiB, 0}});
        ASSERT_LT(fs::file_size(zipFile), 4u * 1024 * 1024) << "the bomb must stay small";

        const fs::path outdir = ta.getPath("bombOut");
        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string()), ZipException);

        EXPECT_FALSE(fs::exists(outdir)) << "the output folder was created for a refused archive";
    }

    // The same archive is extracted when the caller turns the ratio off, so the refusal above is the
    // ratio limit and not a side effect of how the archive was built (a smaller bomb, to stay fast).
    TEST(zip, extractAllHonoursAnExpansionRatioOfZero)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("ratio.zip");
        writeZip(zipFile, {{"zeros.bin", "", 16 * 1024 * 1024, 0}});
        const fs::path outdir = ta.getPath("ratioOut");

        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string()), ZipException);
        EXPECT_FALSE(fs::exists(outdir));

        ddb::zip::ExtractOptions options;
        options.maxExpansionRatio = 0;
        ddb::zip::extractAll(zipFile.string(), outdir.string(), options);
        EXPECT_EQ(fs::file_size(outdir / "zeros.bin"), 16u * 1024 * 1024);
    }

    TEST(zip, extractAllRefusesMoreEntriesThanTheLimitBeforeWriting)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("many.zip");
        writeZip(zipFile, {{"a.txt", "a", 0, 0}, {"b.txt", "b", 0, 0}, {"c.txt", "c", 0, 0}});

        const fs::path outdir = ta.getPath("manyOut");
        ddb::zip::ExtractOptions options;
        options.maxEntries = 2;
        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string(), options), ZipException);
        EXPECT_FALSE(fs::exists(outdir));

        options.maxEntries = 3;
        ddb::zip::extractAll(zipFile.string(), outdir.string(), options);
        EXPECT_EQ(countItems(outdir), 3u);
    }

    // The default limit is a million entries: a real tile archive with tens of thousands of tiles passes
    TEST(zip, extractAllExtractsSeventyThousandSmallMembers)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("tiles.zip");
        std::vector<Member> members;
        members.reserve(70000);
        for (int i = 0; i < 70000; i++)
            members.push_back({"tiles/" + std::to_string(i % 70) + "/" + std::to_string(i) + ".b3dm", "tile " + std::to_string(i), 0, 0});
        writeZip(zipFile, members);

        const fs::path outdir = ta.getPath("tilesOut");
        ddb::zip::extractAll(zipFile.string(), outdir.string());

        EXPECT_EQ(fs::file_size(outdir / "tiles" / "69" / "69999.b3dm"), std::string("tile 69999").size());
        EXPECT_EQ(countItems(outdir), 70000u + 70u + 1u); // files, the 70 subfolders and tiles/
    }

    // A member that delivers more than its header declares is cut off at the declared size (plus the one
    // copy buffer in flight), instead of being written whole.
    TEST(zip, extractAllStopsAMemberThatExceedsItsDeclaredSize)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("lying.zip");
        writeZip(zipFile, {{"lie.bin", "", 64 * 1024 * 1024, 0}});
        declareSizeOfFirstMember(zipFile, 1000);

        const fs::path outdir = ta.getPath("lyingOut");
        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string()), ZipException);

        EXPECT_LE(bytesBelow(outdir), 1000u + 4096u) << "the member was written beyond its declared size";
    }

    // The callback sees the declared total before anything is written, and can refuse
    TEST(zip, extractAllAsksTheCallerBeforeWriting)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("hook.zip");
        writeZip(zipFile, {{"a.txt", "12345", 0, 0}, {"d/b.txt", "1234567", 0, 0}});
        const fs::path outdir = ta.getPath("hookOut");

        uint64_t seen = 0;
        ddb::zip::ExtractOptions options;
        options.beforeWrite = [&seen, &outdir](uint64_t total)
        {
            seen = total;
            EXPECT_FALSE(fs::exists(outdir));
            throw ZipException("no room");
        };
        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string(), options), ZipException);
        EXPECT_EQ(seen, 12u);
        EXPECT_FALSE(fs::exists(outdir));
    }

    // ---- Names ----

    TEST(zip, extractAllRefusesNamesWindowsCannotCreate)
    {
        const std::vector<std::string> names = {
            "con",          "CON",          "Prn.txt",       "dir/aux",      "dir/AUX.tar.gz", "NUL",
            "nul.json",     "COM1",         "com9.bin",      "LPT1",         "d/lpt9.txt",     "CON .txt",
            "trailing.",    "trailing ",    "dir./file.txt", "dir /file.txt", "a:b",           "dir/file:stream",
            "dir/c:/x",     "x/y.json:Zone.Identifier", "ok/ends.with.dot.",
        };

        TestArea ta(TEST_NAME);
        for (const auto &name : names)
        {
            SCOPED_TRACE(name);
            const fs::path zipFile = ta.getPath("windows.zip");
            writeZip(zipFile, {{"tileset.json", "{}", 0, 0}, {name, "x", 0, 0}});
            const fs::path outdir = ta.getPath("windowsOut");
            fs::remove_all(outdir);

            EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string()), ZipException);
            // The good member in front of it is not written either: names are checked first
            EXPECT_FALSE(fs::exists(outdir));
        }
    }

    TEST(zip, extractAllKeepsNamesThatOnlyLookLikeWindowsOnes)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("fine.zip");
        const std::vector<std::string> names = {"CONSOLE.txt", "com0", "COM10", "lpt", "auxiliary/nul_.json",
                                                ".hidden",     "a.b.c", "my file.txt", "./under/dot.json"};
        std::vector<Member> members;
        for (const auto &n : names)
            members.push_back({n, "x", 0, 0});
        writeZip(zipFile, members);

        const fs::path outdir = ta.getPath("fineOut");
        ddb::zip::extractAll(zipFile.string(), outdir.string());
        EXPECT_TRUE(fs::is_regular_file(outdir / "CONSOLE.txt"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "COM10"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "auxiliary" / "nul_.json"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "my file.txt"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "under" / "dot.json"));
    }

#ifndef _WIN32
    TEST(zip, extractAllTakesNamesWindowsCannotCreateWhenAsked)
    {
        // A pull extracts the files of a user where they live: a POSIX file system takes these
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("posix.zip");
        const std::vector<std::string> names = {"scan 12:00:00.jpg", "notes.", "nul.json", "dir /CON", "trailing "};
        std::vector<Member> members;
        for (const auto &n : names)
            members.push_back({n, "x", 0, 0});
        writeZip(zipFile, members);

        const fs::path refused = ta.getPath("refusedOut");
        EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), refused.string()), ZipException);
        EXPECT_FALSE(fs::exists(refused));

        ddb::zip::ExtractOptions options;
        options.refuseWindowsUnsafeNames = false;
        const fs::path outdir = ta.getPath("posixOut");
        ddb::zip::extractAll(zipFile.string(), outdir.string(), options);
        for (const auto &n : names)
        {
            SCOPED_TRACE(n);
            EXPECT_TRUE(fs::is_regular_file(outdir / n));
        }
    }
#endif

    TEST(zip, extractAllKeepsRefusingTraversalWhenWindowsNamesAreAllowed)
    {
        TestArea ta(TEST_NAME);
        ddb::zip::ExtractOptions options;
        options.refuseWindowsUnsafeNames = false;
        for (const std::string name : {"../escape.txt", "/abs.txt", "c:/x.txt", "a/../../b.txt"})
        {
            SCOPED_TRACE(name);
            const fs::path zipFile = ta.getPath("traversal.zip");
            writeZip(zipFile, {{name, "x", 0, 0}});
            const fs::path outdir = ta.getPath("traversalOut");
            fs::remove_all(outdir);
            EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string(), options), ZipException);
            EXPECT_FALSE(fs::exists(outdir));
        }
    }

    // ---- Protections that were there before the limits ----

    TEST(zip, extractAllRefusesTraversalAndBackslashNames)
    {
        TestArea ta(TEST_NAME);
        for (const std::string name : {"../escape.txt", "a/../../escape.txt", "..\\escape.txt", "/abs/escape.txt", "C:/escape.txt"})
        {
            SCOPED_TRACE(name);
            const fs::path zipFile = ta.getPath("slip.zip");
            writeZip(zipFile, {{name, "x", 0, 0}});
            const fs::path outdir = ta.getFolder("slipOut");
            EXPECT_THROW(ddb::zip::extractAll(zipFile.string(), outdir.string()), ZipException);
            EXPECT_FALSE(fs::exists(outdir.parent_path() / "escape.txt"));
            EXPECT_EQ(countItems(outdir), 0u);
        }
    }

    // A member that the archive marks as a symbolic link is extracted as a regular file holding the
    // link text, so a link in an archive cannot point a later member out of the folder
    TEST(zip, extractAllWritesASymbolicLinkMemberAsARegularFile)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipFile = ta.getPath("link.zip");
        writeZip(zipFile, {{"link", "../../etc/passwd", 0, 0120777}});
        const fs::path outdir = ta.getPath("linkOut");

        ddb::zip::extractAll(zipFile.string(), outdir.string());

        EXPECT_FALSE(fs::is_symlink(outdir / "link"));
        ASSERT_TRUE(fs::is_regular_file(outdir / "link"));
        std::ifstream in(outdir / "link", std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "../../etc/passwd");
    }

    // ---- The disk check of a build ----

    // Free space moves while tests run in parallel, so every expectation below holds whatever the
    // disk does in between, short of a swing of a large share of what is free: the sizes either
    // relate to the capacity of the file system, which does not move, or keep wide margins
    TEST(zip, ensureSufficientDiskSpaceCountsTheBytesToWrite)
    {
        TestArea ta(TEST_NAME);
        const fs::path dir = ta.getFolder("space");
        constexpr uintmax_t MiB = 1024 * 1024;

        std::error_code ec;
        const fs::space_info space = fs::space(dir, ec);
        ASSERT_FALSE(ec);
        const uintmax_t availableMb = space.available / MiB;
        ASSERT_GE(availableMb, 64u) << "too little free disk to run the test";

        {
            EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "1");
            EXPECT_NO_THROW(ensureSufficientDiskSpace(dir));
            EXPECT_NO_THROW(ensureSufficientDiskSpace(dir, 1024));

            // As much as the whole file system holds never fits next to a floor, and the message
            // names both terms
            try
            {
                ensureSufficientDiskSpace(dir, space.capacity);
                FAIL() << "expected RetryableBuildException";
            }
            catch (const RetryableBuildException &e)
            {
                EXPECT_EQ(e.reason(), RetryableBuildReason::LowDiskSpace);
                EXPECT_NE(std::string(e.what()).find(std::to_string(space.capacity / MiB) + " MB to write plus 1 MB of floor"),
                          std::string::npos)
                    << e.what();
            }

            // The sum saturates instead of wrapping around to a small number
            EXPECT_THROW(ensureSufficientDiskSpace(dir, UINTMAX_MAX), RetryableBuildException);
        }

        // The floor and the bytes to write each fit on their own (unless 40% of the free space
        // goes in the meantime) and not together (unless it grows by 20%): they are added
        {
            const uintmax_t shareMb = availableMb * 3 / 5;
            {
                EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", std::to_string(shareMb));
                EXPECT_NO_THROW(ensureSufficientDiskSpace(dir));
            }
            {
                EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "1");
                EXPECT_NO_THROW(ensureSufficientDiskSpace(dir, shareMb * MiB));
            }
            EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", std::to_string(shareMb));
            try
            {
                ensureSufficientDiskSpace(dir, shareMb * MiB);
                FAIL() << "expected RetryableBuildException";
            }
            catch (const RetryableBuildException &e)
            {
                EXPECT_EQ(e.reason(), RetryableBuildReason::LowDiskSpace);
                EXPECT_NE(std::string(e.what()).find(std::to_string(2 * shareMb) + " MB required (" + std::to_string(shareMb) +
                                                     " MB to write plus " + std::to_string(shareMb) + " MB of floor)"),
                          std::string::npos)
                    << e.what();
            }
        }

        // A floor of zero turns the check off, bytes to write included
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "0");
        EXPECT_NO_THROW(ensureSufficientDiskSpace(dir, UINTMAX_MAX / 2));
    }

    // ---- The build of a .3tz uses the limits ----

    struct Tiles3DDataset : IndexFixture
    {
        explicit Tiles3DDataset(const std::string &name) : IndexFixture(name, "") {}

        void add(const std::string &rel)
        {
            addToIndex(db.get(), {(root / rel).string()});
        }

        bool built(const std::string &rel)
        {
            return fs::exists(db->buildDirectory() / indexedHash(db.get(), rel) / "3dtiles" / "tileset.json");
        }
    };

    TEST(zip, buildOfA3tzExtractsATileset)
    {
        // A floor of 1 MB, so a disk with less than the default 512 MB free does not fail the build
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "1");
        Tiles3DDataset ds(TEST_NAME);
        writeZip(ds.root / "city.3tz", {{"tileset.json", R"({"asset":{"version":"1.1"},"geometricError":0,"root":{}})", 0, 0},
                                        {"tiles/0.b3dm", "tile", 0, 0}});
        ds.add("city.3tz");

        ASSERT_NO_THROW(build(ds.db.get(), "city.3tz", "", false));
        EXPECT_TRUE(ds.built("city.3tz"));
        EXPECT_TRUE(fs::is_regular_file(ds.db->buildDirectory() / indexedHash(ds.db.get(), "city.3tz") / "3dtiles" / "tiles" / "0.b3dm"));
    }

    TEST(zip, buildOfA3tzBombFailsAndPublishesNothing)
    {
        // A floor of 1 MB, so it is the expansion ratio that refuses the archive even on a fuller disk
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", "1");
        Tiles3DDataset ds(TEST_NAME);
        writeZip(ds.root / "bomb.3tz", {{"tileset.json", R"({"asset":{"version":"1.1"},"geometricError":0,"root":{}})", 0, 0},
                                        {"tiles/bomb.bin", "", GiB, 0}});
        ds.add("bomb.3tz");

        EXPECT_THROW(build(ds.db.get(), "bomb.3tz", "", false), ZipException);

        EXPECT_FALSE(ds.built("bomb.3tz"));
        const fs::path buildDir = ds.db->buildDirectory();
        EXPECT_LT(bytesBelow(buildDir), 1024u * 1024u) << "bytes of the bomb were written below the build folder";
    }

    // An archive whose declared size does not fit the free disk is a retryable failure of the build,
    // before a byte of it is written. Free space moves while tests run in parallel, so the floor
    // sits 300 MB below what is free: the check of the dispatcher, which knows only the floor,
    // passes unless 300 MB go in the meantime, and the one of the extraction, which adds the
    // 600 MB the archive declares, refuses it unless 300 MB come free. The archive declares more
    // than it holds, so it stays small while its 32 MB of noise (incompressible) keep it within the
    // expansion ratio: the declared total, not the ratio, is what refuses it.
    TEST(zip, buildOfA3tzThatDoesNotFitTheDiskWritesNothing)
    {
        constexpr uintmax_t MiB = 1024 * 1024;
        constexpr uintmax_t declaredMb = 600;
        constexpr uintmax_t marginMb = 300;

        Tiles3DDataset ds(TEST_NAME);
        std::string noise(32 * MiB, '\0');
        uint32_t state = 12345;
        for (auto &c : noise)
        {
            state = state * 1664525u + 1013904223u;
            c = static_cast<char>(state >> 24);
        }
        const fs::path archive = ds.root / "big.3tz";
        writeZip(archive, {{"tiles/noise.bin", noise, 0, 0}});
        declareSizeOfFirstMember(archive, static_cast<uint32_t>(declaredMb * MiB));
        ASSERT_LE(declaredMb * MiB, fs::file_size(archive) * ddb::zip::ExtractOptions().maxExpansionRatio);
        ds.add("big.3tz");

        std::error_code ec;
        const uintmax_t availableMb = fs::space(ds.root, ec).available / MiB;
        ASSERT_FALSE(ec);
        // On a disk with less than the margin free, the smallest floor still passes the first check
        const uintmax_t floorMb = availableMb > marginMb + 1 ? availableMb - marginMb : 1;
        EnvVarGuard floor("DDB_MIN_FREE_DISK_MB", std::to_string(floorMb));

        try
        {
            build(ds.db.get(), "big.3tz", "", false);
            FAIL() << "expected RetryableBuildException";
        }
        catch (const RetryableBuildException &e)
        {
            EXPECT_EQ(e.reason(), RetryableBuildReason::LowDiskSpace);
            EXPECT_NE(std::string(e.what()).find(std::to_string(declaredMb) + " MB to write plus " + std::to_string(floorMb) +
                                                 " MB of floor"),
                      std::string::npos)
                << e.what();
        }
        EXPECT_FALSE(ds.built("big.3tz"));
        EXPECT_LT(bytesBelow(ds.db->buildDirectory()), 1024u * 1024u) << "bytes of the archive were written below the build folder";
    }

} // namespace
