/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "ddb.h"
#include "entry.h"
#include "entry_types.h"
#include "exceptions.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "utils.h"
#include "entry_filters.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace
{

    using namespace ddb;

    // Helper function to get entry from database
    bool getEntryFromDb(Database *db, const std::string &path, Entry &entry)
    {
        auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE path = ?");
        q->bind(1, path);
        if (q->fetch())
        {
            entry.path = q->getText(0);
            entry.hash = q->getText(1);
            entry.type = static_cast<EntryType>(q->getInt(2));
            entry.properties = json::parse(q->getText(3));
            entry.mtime = q->getInt64(4);
            entry.size = q->getInt64(5);
            entry.depth = q->getInt(6);
            return true;
        }
        return false;
    }

    // Helper function to count entries in database
    int countEntries(Database *db, EntryType type = EntryType::Undefined)
    {
        std::string sql = "SELECT COUNT(*) FROM entries";
        if (type != EntryType::Undefined)
        {
            sql += " WHERE type = ?";
        }
        auto q = db->query(sql);
        if (type != EntryType::Undefined)
        {
            q->bind(1, static_cast<int>(type));
        }
        q->fetch();
        return q->getInt(0);
    }

    TEST(typeFromHuman, validTypes)
    {
        EXPECT_EQ(typeFromHuman("image"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("Image"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("IMAGE"), EntryType::Image);
        EXPECT_EQ(typeFromHuman("geoimage"), EntryType::GeoImage);
        EXPECT_EQ(typeFromHuman("GeoImage"), EntryType::GeoImage);
        EXPECT_EQ(typeFromHuman("pointcloud"), EntryType::PointCloud);
        EXPECT_EQ(typeFromHuman("PointCloud"), EntryType::PointCloud);
        EXPECT_EQ(typeFromHuman("georaster"), EntryType::GeoRaster);
        EXPECT_EQ(typeFromHuman("video"), EntryType::Video);
        EXPECT_EQ(typeFromHuman("geovideo"), EntryType::GeoVideo);
        EXPECT_EQ(typeFromHuman("model"), EntryType::Model);
        EXPECT_EQ(typeFromHuman("panorama"), EntryType::Panorama);
        EXPECT_EQ(typeFromHuman("geopanorama"), EntryType::GeoPanorama);
        EXPECT_EQ(typeFromHuman("vector"), EntryType::Vector);
        EXPECT_EQ(typeFromHuman("markdown"), EntryType::Markdown);
        EXPECT_EQ(typeFromHuman("generic"), EntryType::Generic);
        EXPECT_EQ(typeFromHuman("dronedb"), EntryType::DroneDB);
    }

    TEST(typeFromHuman, invalidTypes)
    {
        EXPECT_EQ(typeFromHuman("unknown"), EntryType::Undefined);
        EXPECT_EQ(typeFromHuman(""), EntryType::Undefined);
        EXPECT_EQ(typeFromHuman("xyz123"), EntryType::Undefined);
    }

    TEST(getEntryTypeNames, returnsAllTypes)
    {
        auto names = getEntryTypeNames();

        // Should not be empty
        EXPECT_FALSE(names.empty());

        // Should contain common types (lowercase)
        EXPECT_TRUE(std::find(names.begin(), names.end(), "image") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "geoimage") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "pointcloud") != names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "georaster") != names.end());

        // Should not contain directory or undefined
        EXPECT_TRUE(std::find(names.begin(), names.end(), "directory") == names.end());
        EXPECT_TRUE(std::find(names.begin(), names.end(), "undefined") == names.end());
    }

    TEST(rescanIndex, basicRescan)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create a simple text file
        fs::path textFile = testFolder / "test.txt";
        {
            std::ofstream ofs(textFile);
            ofs << "Hello, World!";
        }

        // Add file to index
        ddb::addToIndex(db.get(), {textFile.string()});

        // Verify file was added
        Entry beforeEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "test.txt", beforeEntry));
        EXPECT_EQ(beforeEntry.type, EntryType::Generic);

        // Rescan should succeed
        int rescanCount = 0;
        EXPECT_NO_THROW(
            ddb::rescanIndex(db.get(), {}, {}, true,
                [&rescanCount](const Entry &e, bool success, const std::string &error) {
                    if (success) rescanCount++;
                    return true;
                })
        );

        // Should have rescanned at least the text file
        EXPECT_GE(rescanCount, 1);

        // Entry should still exist after rescan
        Entry afterEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "test.txt", afterEntry));
        EXPECT_EQ(afterEntry.type, EntryType::Generic);
    }

    TEST(rescanIndex, rescanWithTypeFilter)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create two files: one text and one markdown
        fs::path textFile = testFolder / "test.txt";
        fs::path mdFile = testFolder / "readme.md";
        {
            std::ofstream ofs1(textFile);
            ofs1 << "Hello, World!";
            std::ofstream ofs2(mdFile);
            ofs2 << "# Readme\nThis is a test.";
        }

        // Add files to index
        ddb::addToIndex(db.get(), {textFile.string(), mdFile.string()});

        // Rescan only markdown files
        int rescanCount = 0;
        std::vector<EntryType> typeFilter = {EntryType::Markdown};
        ddb::rescanIndex(db.get(), typeFilter, {}, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                if (success) rescanCount++;
                return true;
            });

        // Should have rescanned only the markdown file
        EXPECT_EQ(rescanCount, 1);
    }

    TEST(rescanIndex, rescanMissingFile)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create and add a file
        fs::path textFile = testFolder / "test.txt";
        {
            std::ofstream ofs(textFile);
            ofs << "Hello, World!";
        }
        ddb::addToIndex(db.get(), {textFile.string()});

        // Remove the file from filesystem (but keep in database)
        fs::remove(textFile);

        // Rescan with stopOnError=true should throw
        EXPECT_THROW(
            ddb::rescanIndex(db.get(), {}, {}, true, nullptr),
            FSException
        );

        // Rescan with stopOnError=false should continue
        int errorCount = 0;
        int successCount = 0;
        EXPECT_NO_THROW(
            ddb::rescanIndex(db.get(), {}, {}, false,
                [&errorCount, &successCount](const Entry &e, bool success, const std::string &error) {
                    if (success) successCount++;
                    else errorCount++;
                    return true;
                })
        );

        // Should have reported the error
        EXPECT_GE(errorCount, 1);
    }

    TEST(rescanIndex, rescanCancellation)
    {
        TestArea ta(TEST_NAME, true);

        // Create test directory and initialize database
        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        // Create multiple files
        for (int i = 0; i < 5; i++)
        {
            fs::path textFile = testFolder / ("test" + std::to_string(i) + ".txt");
            std::ofstream ofs(textFile);
            ofs << "File " << i;
        }

        // Add all files
        std::vector<std::string> paths;
        for (int i = 0; i < 5; i++)
        {
            paths.push_back((testFolder / ("test" + std::to_string(i) + ".txt")).string());
        }
        ddb::addToIndex(db.get(), paths);

        // Rescan but cancel after first file
        int rescanCount = 0;
        ddb::rescanIndex(db.get(), {}, {}, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                rescanCount++;
                return false; // Cancel after first
            });

        // Should have processed only one file before cancellation
        EXPECT_EQ(rescanCount, 1);
    }

    TEST(rescanIndex, rescanWithGeoImage)
    {
        TestArea ta(TEST_NAME, true);

        // Download a real geo-referenced image
        fs::path orthoPath = ta.downloadTestAsset(
            testDataUrl("brighton/odm_orthophoto.tif"),
            "ortho.tif");

        // Initialize database and add file
        ddb::initIndex(ta.getFolder().string());
        auto db = ddb::open(ta.getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        // Get entry before rescan
        Entry beforeEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "ortho.tif", beforeEntry));
        EXPECT_EQ(beforeEntry.type, EntryType::GeoRaster);

        // Rescan
        int rescanCount = 0;
        ddb::rescanIndex(db.get(), {}, {}, true,
            [&rescanCount](const Entry &e, bool success, const std::string &error) {
                if (success) rescanCount++;
                return true;
            });

        EXPECT_GE(rescanCount, 1);

        // Verify entry still has correct type after rescan
        Entry afterEntry;
        EXPECT_TRUE(getEntryFromDb(db.get(), "ortho.tif", afterEntry));
        EXPECT_EQ(afterEntry.type, EntryType::GeoRaster);

        // Hash should remain the same (file content unchanged)
        EXPECT_EQ(beforeEntry.hash, afterEntry.hash);
    }

    // FR-SHR-03, D-050: an index written before the fix holds the vector footprint in UTM metres; the
    // rescan of the vector entries writes it in WGS 84
    TEST(rescanIndex, aVectorFootprintInMetresBecomesWgs84)
    {
        TestArea ta(TEST_NAME, true);
        const auto src = ta.getFolder() / "utm.geojson";
        std::ofstream(src)
            << R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::32615"}},)"
               R"("features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":)"
               R"([[[576764.77,5188207.22],[576864.77,5188207.22],[576864.77,5188307.22],[576764.77,5188207.22]]]}}]})";

        ddb::initIndex(ta.getFolder().string());
        auto db = ddb::open(ta.getFolder().string(), true);
        ddb::addToIndex(db.get(), {src.string()});

        // What the release before the fix wrote: the envelope of the layer as it is, in metres
        auto write = db->query(
            "UPDATE entries SET "
            "point_geom = GeomFromText('POINTZ (576814.77 5188257.22 0)', 4326), "
            "polygon_geom = GeomFromText('POLYGONZ ((576764.77 5188207.22 0, 576864.77 5188207.22 0, "
            "576864.77 5188307.22 0, 576764.77 5188307.22 0, 576764.77 5188207.22 0))', 4326) "
            "WHERE path = 'utm.geojson'");
        write->execute();

        const auto footprint = [&] {
            auto q = db->query("SELECT MbrMinX(polygon_geom), MbrMinY(polygon_geom), MbrMaxX(polygon_geom), "
                               "MbrMaxY(polygon_geom) FROM entries WHERE path = 'utm.geojson'");
            EXPECT_TRUE(q->fetch());
            return std::array<double, 4>{q->getDouble(0), q->getDouble(1), q->getDouble(2), q->getDouble(3)};
        };
        EXPECT_DOUBLE_EQ(footprint()[0], 576764.77);

        int rescanned = 0;
        ddb::rescanIndex(db.get(), {EntryType::Vector}, {}, true,
                         [&rescanned](const Entry &, bool success, const std::string &) {
                             if (success) rescanned++;
                             return true;
                         });
        EXPECT_EQ(rescanned, 1);

        const auto after = footprint();
        EXPECT_NEAR(after[0], -91.9932194933515, 1e-7);
        EXPECT_NEAR(after[1], 46.8429676951816, 1e-7);
        EXPECT_NEAR(after[2], -91.9918913544162, 1e-7);
        EXPECT_NEAR(after[3], 46.8438789991937, 1e-7);
    }

    // FR-SHR-03, D-050: a GeoJSON without a crs member is WGS 84 for GDAL, so one whose coordinates are metres
    // has no footprint now: the rescan removes the footprint in metres that an old index holds, it does not
    // keep it
    TEST(rescanIndex, aVectorWithoutCrsWhoseFootprintWasInMetresHasNoFootprintAfterTheRescan)
    {
        TestArea ta(TEST_NAME, true);
        const auto src = ta.getFolder() / "metres.geojson";
        std::ofstream(src)
            << R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":)"
               R"({"type":"Polygon","coordinates":)"
               R"([[[576764.77,5188207.22],[576864.77,5188207.22],[576864.77,5188307.22],[576764.77,5188207.22]]]}}]})";

        ddb::initIndex(ta.getFolder().string());
        auto db = ddb::open(ta.getFolder().string(), true);
        ddb::addToIndex(db.get(), {src.string()});

        const auto geometryIsNull = [&](const char *column) {
            auto q = db->query(std::string("SELECT ") + column + " IS NULL FROM entries WHERE path = 'metres.geojson'");
            EXPECT_TRUE(q->fetch());
            return q->getInt(0) != 0;
        };
        EXPECT_TRUE(geometryIsNull("polygon_geom")) << "no footprint at index time";

        // What the release before the fix wrote: the envelope of the layer as it is, in metres
        auto write = db->query(
            "UPDATE entries SET "
            "point_geom = GeomFromText('POINTZ (576814.77 5188257.22 0)', 4326), "
            "polygon_geom = GeomFromText('POLYGONZ ((576764.77 5188207.22 0, 576864.77 5188207.22 0, "
            "576864.77 5188307.22 0, 576764.77 5188307.22 0, 576764.77 5188207.22 0))', 4326) "
            "WHERE path = 'metres.geojson'");
        write->execute();
        ASSERT_FALSE(geometryIsNull("polygon_geom"));

        int rescanned = 0;
        ddb::rescanIndex(db.get(), {EntryType::Vector}, {}, true,
                         [&rescanned](const Entry &, bool success, const std::string &) {
                             if (success) rescanned++;
                             return true;
                         });
        EXPECT_EQ(rescanned, 1);
        EXPECT_TRUE(geometryIsNull("polygon_geom"));
        EXPECT_TRUE(geometryIsNull("point_geom"));
    }

    TEST(rescanIndex, invalidPath)
    {
        // Attempt to open a non-existent database should throw
        EXPECT_THROW(
            ddb::open("/nonexistent/path", false),
            FSException
        );
    }

    TEST(rescanIndex, invalidType)
    {
        // typeFromHuman should return Undefined for invalid types
        EXPECT_EQ(typeFromHuman("invalidtype"), EntryType::Undefined);
    }

    TEST(parseExtensionList, normalizesItems)
    {
        const auto exts = filters::parseExtensionList(" dwg, .DXF ,,copc.LAZ,");
        ASSERT_EQ(exts.size(), 3);
        EXPECT_EQ(exts[0], "dwg");
        EXPECT_EQ(exts[1], "dxf");
        EXPECT_EQ(exts[2], "copc.laz");

        EXPECT_TRUE(filters::parseExtensionList("").empty());
        EXPECT_TRUE(filters::parseExtensionList(nullptr).empty());
    }

    TEST(parseExtensionList, rejectsInvalidItems)
    {
        EXPECT_THROW(filters::parseExtensionList("."), InvalidArgsException);
        EXPECT_THROW(filters::parseExtensionList("*.dwg"), InvalidArgsException);
        EXPECT_THROW(filters::parseExtensionList("a/dwg"), InvalidArgsException);
        EXPECT_THROW(filters::parseExtensionList("tar gz"), InvalidArgsException);
    }

    TEST(hasAnyExtension, matchesSuffixAfterDot)
    {
        EXPECT_TRUE(filters::hasAnyExtension("plans/site.DWG", {"dwg"}));
        EXPECT_TRUE(filters::hasAnyExtension("scan.copc.laz", {"copc.laz"}));
        EXPECT_TRUE(filters::hasAnyExtension("scan.copc.laz", {"laz"}));
        EXPECT_FALSE(filters::hasAnyExtension("scan.laz", {"copc.laz"}));
        EXPECT_FALSE(filters::hasAnyExtension("dwg", {"dwg"}));
        EXPECT_FALSE(filters::hasAnyExtension("plans/sitedwg", {"dwg"}));
        EXPECT_FALSE(filters::hasAnyExtension("site.dwg.bak", {"dwg"}));
    }

    TEST(rescanIndex, rescanWithExtensionFilter)
    {
        TestArea ta(TEST_NAME, true);

        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        fs::path lowerTxt = testFolder / "a.txt";
        fs::path upperTxt = testFolder / "B.TXT";
        fs::path md = testFolder / "readme.md";
        {
            std::ofstream(lowerTxt) << "a";
            std::ofstream(upperTxt) << "b";
            std::ofstream(md) << "# Readme";
        }
        ddb::addToIndex(db.get(), {lowerTxt.string(), upperTxt.string(), md.string()});

        std::vector<std::string> rescanned;
        ddb::rescanIndex(db.get(), {}, {"txt"}, true,
            [&rescanned](const Entry &e, bool success, const std::string &) {
                if (success) rescanned.push_back(e.path);
                return true;
            });

        std::sort(rescanned.begin(), rescanned.end());
        ASSERT_EQ(rescanned.size(), 2);
        EXPECT_EQ(rescanned[0], "B.TXT");
        EXPECT_EQ(rescanned[1], "a.txt");
    }

    TEST(rescanIndex, typeAndExtensionFiltersMustBothMatch)
    {
        TestArea ta(TEST_NAME, true);

        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        auto db = ddb::open(testFolder.string(), true);

        fs::path txt = testFolder / "notes.txt";
        fs::path md = testFolder / "readme.md";
        {
            std::ofstream(txt) << "notes";
            std::ofstream(md) << "# Readme";
        }
        ddb::addToIndex(db.get(), {txt.string(), md.string()});

        int rescanCount = 0;
        auto countSuccess = [&rescanCount](const Entry &, bool success, const std::string &) {
            if (success) rescanCount++;
            return true;
        };

        // notes.txt is Generic: no entry is both Markdown and .txt
        ddb::rescanIndex(db.get(), {EntryType::Markdown}, {"txt"}, true, countSuccess);
        EXPECT_EQ(rescanCount, 0);

        ddb::rescanIndex(db.get(), {EntryType::Markdown}, {"md", "txt"}, true, countSuccess);
        EXPECT_EQ(rescanCount, 1);
    }

    TEST(DDBRescanEx, filtersByExtension)
    {
        TestArea ta(TEST_NAME, true);

        auto testFolder = ta.getFolder();
        ddb::initIndex(testFolder.string());
        {
            auto db = ddb::open(testFolder.string(), true);
            fs::path txt = testFolder / "notes.txt";
            fs::path md = testFolder / "readme.md";
            std::ofstream(txt) << "notes";
            std::ofstream(md) << "# Readme";
            ddb::addToIndex(db.get(), {txt.string(), md.string()});
        }

        char *output = nullptr;
        ASSERT_EQ(DDBRescanEx(testFolder.string().c_str(), &output, "", ".MD", true), DDBERR_NONE)
            << DDBGetLastError();
        ASSERT_NE(output, nullptr);
        json j = json::parse(output);
        DDBFree(output);

        ASSERT_TRUE(j.is_array());
        ASSERT_EQ(j.size(), 1);
        EXPECT_EQ(j[0]["path"], "readme.md");
        EXPECT_TRUE(j[0]["success"].get<bool>());

        output = nullptr;
        EXPECT_NE(DDBRescanEx(testFolder.string().c_str(), &output, "", "*.md", true), DDBERR_NONE);
    }

} // namespace
