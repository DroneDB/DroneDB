/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#include "gtest/gtest.h"

#include "entry.h"
#include "exceptions.h"
#include "mzip.h"
#include "test.h"
#include "testarea.h"
#include "tiles3d.h"

namespace
{

    using namespace ddb;

    // Writes a tileset.json into a fresh folder and packs it (and any extra files) into a
    // .3tz archive (a ZIP with tileset.json at the root). Returns the archive path.
    fs::path makeTtz(TestArea &ta, const std::string &name, const std::string &tilesetJson)
    {
        const fs::path dir = ta.getFolder(name + "_src");
        fs::create_directories(dir);
        {
            std::ofstream o((dir / "tileset.json").string(), std::ios::binary);
            o << tilesetJson;
        }
        // A dummy tile so the archive is not just the JSON.
        {
            std::ofstream o((dir / "root.b3dm").string(), std::ios::binary);
            o << "b3dm-placeholder";
        }
        const fs::path ttz = ta.getFolder(".") / (name + ".3tz");
        fs::remove(ttz);
        ddb::zip::zipFolder(dir.string(), ttz.string(), {});
        return ttz;
    }

    // Appends a little-endian integer to a byte buffer.
    template <typename T>
    void putLE(std::vector<uint8_t> &b, T v)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
            b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }

    // Builds a minimal, valid ZIP archive containing a single zero-length STORE entry with the
    // given name. Used to inject a path-traversal entry name that libzip's own writer would not
    // normally produce, so we can exercise the Zip-Slip guard in _extractAll.
    void writeMinimalZip(const fs::path &path, const std::string &entryName)
    {
        std::vector<uint8_t> buf;
        const uint16_t nameLen = static_cast<uint16_t>(entryName.size());

        // Local file header.
        putLE<uint32_t>(buf, 0x04034b50);
        putLE<uint16_t>(buf, 20);   // version needed
        putLE<uint16_t>(buf, 0);    // flags
        putLE<uint16_t>(buf, 0);    // method: store
        putLE<uint16_t>(buf, 0);    // mod time
        putLE<uint16_t>(buf, 0);    // mod date
        putLE<uint32_t>(buf, 0);    // crc32 (empty payload)
        putLE<uint32_t>(buf, 0);    // compressed size
        putLE<uint32_t>(buf, 0);    // uncompressed size
        putLE<uint16_t>(buf, nameLen);
        putLE<uint16_t>(buf, 0);    // extra len
        buf.insert(buf.end(), entryName.begin(), entryName.end());

        const uint32_t cdOffset = static_cast<uint32_t>(buf.size());

        // Central directory header.
        putLE<uint32_t>(buf, 0x02014b50);
        putLE<uint16_t>(buf, 20);   // version made by
        putLE<uint16_t>(buf, 20);   // version needed
        putLE<uint16_t>(buf, 0);    // flags
        putLE<uint16_t>(buf, 0);    // method
        putLE<uint16_t>(buf, 0);    // mod time
        putLE<uint16_t>(buf, 0);    // mod date
        putLE<uint32_t>(buf, 0);    // crc32
        putLE<uint32_t>(buf, 0);    // compressed size
        putLE<uint32_t>(buf, 0);    // uncompressed size
        putLE<uint16_t>(buf, nameLen);
        putLE<uint16_t>(buf, 0);    // extra len
        putLE<uint16_t>(buf, 0);    // comment len
        putLE<uint16_t>(buf, 0);    // disk number start
        putLE<uint16_t>(buf, 0);    // internal attrs
        putLE<uint32_t>(buf, 0);    // external attrs
        putLE<uint32_t>(buf, 0);    // local header offset
        buf.insert(buf.end(), entryName.begin(), entryName.end());

        const uint32_t cdSize = static_cast<uint32_t>(buf.size()) - cdOffset;

        // End of central directory.
        putLE<uint32_t>(buf, 0x06054b50);
        putLE<uint16_t>(buf, 0);    // disk number
        putLE<uint16_t>(buf, 0);    // cd start disk
        putLE<uint16_t>(buf, 1);    // entries this disk
        putLE<uint16_t>(buf, 1);    // total entries
        putLE<uint32_t>(buf, cdSize);
        putLE<uint32_t>(buf, cdOffset);
        putLE<uint16_t>(buf, 0);    // comment len

        std::ofstream o(path.string(), std::ios::binary);
        o.write(reinterpret_cast<const char *>(buf.data()), buf.size());
    }

    // A .3tz is recognised as EntryType::Tiles3D by extension.
    TEST(tiles3d, fingerprintTtz)
    {
        TestArea ta(TEST_NAME);
        const std::string json =
            R"({"asset":{"version":"1.1"},"geometricError":10,)"
            R"("root":{"boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":0}})";
        const fs::path ttz = makeTtz(ta, "local", json);

        Entry e;
        parseEntry(ttz, ta.getFolder("."), e, false);
        EXPECT_EQ(e.type, EntryType::Tiles3D);
    }

    // A tileset whose bounding box sits at the origin (local/engineering frame) is not
    // georeferenced and carries no footprint.
    TEST(tiles3d, localBoxNotGeoreferenced)
    {
        TestArea ta(TEST_NAME);
        const std::string json =
            R"({"asset":{"version":"1.0"},"geometricError":5,)"
            R"("root":{"transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],)"
            R"("boundingVolume":{"box":[0,0.07,0.02,0.075,0,0,0,-0.08,0,0,0,0.028]},"geometricError":0}})";
        const fs::path ttz = makeTtz(ta, "local", json);

        Tiles3DInfo info;
        ASSERT_TRUE(getTiles3DInfo(ttz.string(), info));
        EXPECT_FALSE(info.georeferenced);
    }

    // A tileset with a WGS84 region bounding volume is georeferenced; the footprint is the
    // region converted from radians to degrees.
    TEST(tiles3d, regionIsGeoreferenced)
    {
        TestArea ta(TEST_NAME);
        // region = [west, south, east, north, minH, maxH] in radians. ~ Milan.
        const std::string json =
            R"({"asset":{"version":"1.1"},"geometricError":100,)"
            R"("root":{"boundingVolume":{"region":[0.1603,0.7934,0.1606,0.7937,100,150]},)"
            R"("geometricError":0}})";
        const fs::path ttz = makeTtz(ta, "region", json);

        Tiles3DInfo info;
        ASSERT_TRUE(getTiles3DInfo(ttz.string(), info));
        EXPECT_TRUE(info.georeferenced);
        EXPECT_TRUE(info.hasBounds);
        // 0.1603 rad ~ 9.18 deg, 0.7934 rad ~ 45.46 deg.
        EXPECT_NEAR(info.centerLon, 9.185, 0.1);
        EXPECT_NEAR(info.centerLat, 45.46, 0.1);
    }

    // A tileset with an ECEF root transform (box centre far from the origin) is georeferenced;
    // the centroid reprojects to the expected WGS84 location.
    TEST(tiles3d, ecefBoxIsGeoreferenced)
    {
        TestArea ta(TEST_NAME);
        // ECEF origin ~ (lat 45.4642, lon 9.19). Column-major transform, translation in [12..14].
        const std::string json =
            R"({"asset":{"version":"1.0"},"geometricError":50,)"
            R"("root":{"transform":[)"
            R"(-0.1596,0.9872,0,0,-0.7027,-0.1136,0.7022,0,0.6932,0.1121,0.7119,0,)"
            R"(4423530.35,715663.30,4523765.04,1],)"
            R"("boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":0}})";
        const fs::path ttz = makeTtz(ta, "ecef", json);

        Tiles3DInfo info;
        ASSERT_TRUE(getTiles3DInfo(ttz.string(), info));
        EXPECT_TRUE(info.georeferenced);
        EXPECT_NEAR(info.centerLon, 9.19, 0.5);
        EXPECT_NEAR(info.centerLat, 45.46, 0.5);
    }

    // parseEntry stores the georeferenced flag and a footprint for an ECEF tileset.
    TEST(tiles3d, parseEntrySetsGeoreferenced)
    {
        TestArea ta(TEST_NAME);
        const std::string json =
            R"({"asset":{"version":"1.0"},"geometricError":50,)"
            R"("root":{"transform":[)"
            R"(-0.1596,0.9872,0,0,-0.7027,-0.1136,0.7022,0,0.6932,0.1121,0.7119,0,)"
            R"(4423530.35,715663.30,4523765.04,1],)"
            R"("boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":0}})";
        const fs::path ttz = makeTtz(ta, "ecef", json);

        Entry e;
        parseEntry(ttz, ta.getFolder("."), e, false);
        EXPECT_EQ(e.type, EntryType::Tiles3D);
        ASSERT_TRUE(e.properties.contains("georeferenced"));
        EXPECT_TRUE(e.properties["georeferenced"].get<bool>());
        EXPECT_GT(e.point_geom.size(), 0);
        EXPECT_GT(e.polygon_geom.size(), 0);
    }

    // Zip-Slip: an archive entry that escapes the extraction directory is rejected and nothing
    // is written outside the output folder.
    TEST(tiles3d, extractRejectsPathTraversal)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipPath = ta.getFolder(".") / "evil.zip";
        writeMinimalZip(zipPath, "../evil_escape.txt");

        const fs::path outdir = ta.getFolder("evilOut");
        EXPECT_THROW(ddb::zip::extractAll(zipPath.string(), outdir.string()), ZipException);

        // The escaping file must not have been created next to the output directory.
        EXPECT_FALSE(fs::exists(outdir.parent_path() / "evil_escape.txt"));
    }

    // Absolute-path entries are also rejected.
    TEST(tiles3d, extractRejectsAbsolutePath)
    {
        TestArea ta(TEST_NAME);
        const fs::path zipPath = ta.getFolder(".") / "abs.zip";
        writeMinimalZip(zipPath, "/etc/evil.txt");

        const fs::path outdir = ta.getFolder("absOut");
        EXPECT_THROW(ddb::zip::extractAll(zipPath.string(), outdir.string()), ZipException);
    }

} // namespace
