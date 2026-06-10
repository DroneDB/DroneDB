/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "stac.h"
#include "ddb.h"
#include "test.h"
#include "testarea.h"

namespace
{

    using namespace ddb;

    class StacTest : public ::testing::Test
    {
    protected:
        std::unique_ptr<TestArea> ta;
        fs::path orthoPath;
        fs::path imagePath;

        void SetUp() override
        {
            ta = std::make_unique<TestArea>(TEST_NAME, true);

            // Download a GeoRaster (has geotransform + projection, no captureTime)
            orthoPath = ta->downloadTestAsset(
                "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
                "ortho.tif");

            // Download a GeoImage (has captureTime from EXIF)
            imagePath = ta->downloadTestAsset(
                "https://github.com/DroneDB/test_data/raw/master/images/DJI_0018.JPG",
                "DJI_0018.JPG");
        }
    };

    // Test STAC Item generation for a GeoRaster with projection extension
    TEST_F(StacTest, itemWithProjection)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string(), "ortho.tif");

        // Basic STAC Item structure
        EXPECT_EQ(j["type"], "Feature");
        EXPECT_EQ(j["stac_version"], "1.1.0");

        // ID should be prefixed with path-
        std::string id = j["id"];
        EXPECT_TRUE(id.rfind("path-", 0) == 0) << "ID should start with 'path-', got: " << id;

        // datetime property must exist (STAC requirement)
        EXPECT_TRUE(j["properties"].contains("datetime"));

        // GeoRaster should NOT have captureTime, so datetime falls back to mtime
        // mtime is always > 0 for real files, so datetime should be a non-null ISO string
        EXPECT_TRUE(j["properties"]["datetime"].is_string());
        std::string datetime = j["properties"]["datetime"];
        EXPECT_FALSE(datetime.empty());
        // Check ISO 8601 format (starts with year, ends with Z)
        EXPECT_TRUE(datetime.size() >= 20);
        EXPECT_EQ(datetime.back(), 'Z');

        // Projection extension fields
        EXPECT_TRUE(j.contains("stac_extensions"));
        EXPECT_TRUE(j["stac_extensions"].is_array());
        bool hasProjectionExt = false;
        for (const auto &ext : j["stac_extensions"])
        {
            if (ext.get<std::string>().find("projection") != std::string::npos)
            {
                hasProjectionExt = true;
                break;
            }
        }
        EXPECT_TRUE(hasProjectionExt) << "Should have projection STAC extension";

        // proj:transform should exist (mapped from geotransform)
        EXPECT_TRUE(j["properties"].contains("proj:transform"));
        EXPECT_TRUE(j["properties"]["proj:transform"].is_array());

        // proj:wkt2 should exist (mapped from projection)
        EXPECT_TRUE(j["properties"].contains("proj:wkt2"));
        EXPECT_TRUE(j["properties"]["proj:wkt2"].is_string());

        // proj:shape should exist (mapped from height/width)
        EXPECT_TRUE(j["properties"].contains("proj:shape"));
        EXPECT_TRUE(j["properties"]["proj:shape"].is_array());
        EXPECT_EQ(j["properties"]["proj:shape"].size(), 2);

        // Original keys should be removed (moved to proj:* fields)
        EXPECT_FALSE(j["properties"].contains("geotransform"));
        EXPECT_FALSE(j["properties"].contains("projection"));
        EXPECT_FALSE(j["properties"].contains("height"));
        EXPECT_FALSE(j["properties"].contains("width"));

        // proj:code must be present (Projection Extension v2.0.0 replaced proj:epsg)
        EXPECT_TRUE(j["properties"].contains("proj:code"));
        EXPECT_TRUE(j["properties"]["proj:code"].is_string());
        EXPECT_EQ(j["properties"]["proj:code"].get<std::string>().rfind("EPSG:", 0), 0)
            << "proj:code must be a CURIE like 'EPSG:32601'";
        EXPECT_FALSE(j["properties"].contains("proj:epsg"))
            << "proj:epsg is forbidden by Projection Extension v2.0.0";
    }

    // STAC Item must carry the top-level "collection" field when a rel:collection link is present
    TEST_F(StacTest, itemHasCollectionField)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        const std::string collectionRoot = "http://localhost:7000/orgs/acme/ds/brighton";
        const std::string collectionId = "acme/brighton";
        auto j = generateStac(ta->getFolder().string(), "ortho.tif", collectionRoot,
                              collectionId, "http://localhost:7000");

        ASSERT_TRUE(j.contains("collection"));
        EXPECT_EQ(j["collection"], collectionId);

        // The collection link must also be present and consistent
        bool hasCollectionLink = false;
        for (const auto &link : j["links"])
        {
            if (link["rel"] == "collection")
                hasCollectionLink = true;
        }
        EXPECT_TRUE(hasCollectionLink) << "Item must have a rel:collection link";
    }

    // STAC API ItemCollection (FeatureCollection) generation
    TEST_F(StacTest, itemCollectionStructure)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        const std::string collectionRoot = "http://localhost:7000/orgs/acme/ds/brighton";
        const std::string collectionId = "acme/brighton";
        auto fc = generateStacItemCollection(ta->getFolder().string(), collectionRoot,
                                             collectionId, "http://localhost:7000");

        EXPECT_EQ(fc["type"], "FeatureCollection");
        ASSERT_TRUE(fc.contains("features"));
        ASSERT_TRUE(fc["features"].is_array());
        EXPECT_GE(fc["features"].size(), 1);
        EXPECT_TRUE(fc.contains("links"));
        EXPECT_TRUE(fc.contains("numberMatched"));
        EXPECT_TRUE(fc.contains("numberReturned"));

        // Each feature must be a valid STAC Item with the collection field set
        for (const auto &feat : fc["features"])
        {
            EXPECT_EQ(feat["type"], "Feature");
            EXPECT_EQ(feat["stac_version"], "1.1.0");
            ASSERT_TRUE(feat.contains("collection"));
            EXPECT_EQ(feat["collection"], collectionId);
        }
    }

    // ItemCollection paging via limit/offset
    TEST_F(StacTest, itemCollectionPaging)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        auto page = generateStacItemCollection(ta->getFolder().string(), ".", "acme/brighton",
                                               "", {}, "", "", 1, 0);
        EXPECT_EQ(page["numberReturned"], 1);
        EXPECT_GE(page["numberMatched"].get<long long>(), 1);
    }

    // Test STAC Item generation for a GeoImage with captureTime
    TEST_F(StacTest, itemWithCaptureTime)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {imagePath.string()});

        auto j = generateStac(ta->getFolder().string(), "DJI_0018.JPG");

        // Basic STAC Item structure
        EXPECT_EQ(j["type"], "Feature");
        EXPECT_EQ(j["stac_version"], "1.1.0");

        // ID should be prefixed with path-
        std::string id = j["id"];
        EXPECT_TRUE(id.rfind("path-", 0) == 0) << "ID should start with 'path-', got: " << id;

        // datetime property must exist from captureTime
        EXPECT_TRUE(j["properties"].contains("datetime"));
        EXPECT_TRUE(j["properties"]["datetime"].is_string());

        std::string datetime = j["properties"]["datetime"];
        EXPECT_FALSE(datetime.empty());
        // Check ISO 8601 format
        EXPECT_EQ(datetime.back(), 'Z');
        EXPECT_TRUE(datetime.find('T') != std::string::npos);

        // GeoImage should NOT have projection extension
        if (j.contains("stac_extensions"))
        {
            for (const auto &ext : j["stac_extensions"])
            {
                EXPECT_TRUE(ext.get<std::string>().find("projection") == std::string::npos)
                    << "GeoImage should not have projection extension";
            }
        }
    }

    // Test STAC Collection with temporal extent from captureTime
    TEST_F(StacTest, collectionTemporalExtentFromCaptureTime)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {imagePath.string()});

        auto j = generateStac(ta->getFolder().string());

        EXPECT_EQ(j["type"], "Collection");
        EXPECT_EQ(j["stac_version"], "1.1.0");

        // Temporal extent should be populated from captureTime
        EXPECT_TRUE(j.contains("extent"));
        EXPECT_TRUE(j["extent"].contains("temporal"));
        EXPECT_TRUE(j["extent"]["temporal"].contains("interval"));
        EXPECT_TRUE(j["extent"]["temporal"]["interval"].is_array());
        EXPECT_GE(j["extent"]["temporal"]["interval"].size(), 1);

        auto interval = j["extent"]["temporal"]["interval"][0];
        EXPECT_TRUE(interval.is_array());
        EXPECT_EQ(interval.size(), 2);

        // Both start and end should be non-null ISO 8601 strings
        EXPECT_TRUE(interval[0].is_string());
        EXPECT_TRUE(interval[1].is_string());

        std::string start = interval[0];
        std::string end = interval[1];
        EXPECT_FALSE(start.empty());
        EXPECT_FALSE(end.empty());
        EXPECT_EQ(start.back(), 'Z');
        EXPECT_EQ(end.back(), 'Z');
    }

    // Test STAC Collection with temporal extent fallback to mtime
    TEST_F(StacTest, collectionTemporalExtentFromMtime)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);

        // Add only the ortho (GeoRaster has no captureTime), so falls back to mtime
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string());

        EXPECT_EQ(j["type"], "Collection");

        // Temporal extent should still be populated (from mtime)
        EXPECT_TRUE(j.contains("extent"));
        EXPECT_TRUE(j["extent"].contains("temporal"));
        EXPECT_TRUE(j["extent"]["temporal"].contains("interval"));

        auto interval = j["extent"]["temporal"]["interval"][0];

        // mtime for a real file should give non-null values
        EXPECT_TRUE(interval[0].is_string());
        EXPECT_TRUE(interval[1].is_string());
    }

    // Test STAC Collection spatial extent
    TEST_F(StacTest, collectionSpatialExtent)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string());

        EXPECT_TRUE(j.contains("extent"));
        EXPECT_TRUE(j["extent"].contains("spatial"));
        EXPECT_TRUE(j["extent"]["spatial"].contains("bbox"));
        EXPECT_TRUE(j["extent"]["spatial"]["bbox"].is_array());
        EXPECT_GE(j["extent"]["spatial"]["bbox"].size(), 1);
    }

    // Test that STAC Item IDs with path- prefix are unique for different paths
    TEST_F(StacTest, itemIdUniqueness)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        auto j1 = generateStac(ta->getFolder().string(), "ortho.tif");
        auto j2 = generateStac(ta->getFolder().string(), "DJI_0018.JPG");

        std::string id1 = j1["id"];
        std::string id2 = j2["id"];

        // IDs should be different for different entries
        EXPECT_NE(id1, id2);

        // Both should have path- prefix
        EXPECT_TRUE(id1.rfind("path-", 0) == 0);
        EXPECT_TRUE(id2.rfind("path-", 0) == 0);
    }

    // Test STAC Item links structure
    TEST_F(StacTest, itemLinksStructure)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string(), "ortho.tif");

        // Should have links array
        EXPECT_TRUE(j.contains("links"));
        EXPECT_TRUE(j["links"].is_array());

        // Should have assets
        EXPECT_TRUE(j.contains("assets"));
        EXPECT_TRUE(j["assets"].is_object());

        // Should have geometry
        EXPECT_TRUE(j.contains("geometry"));
    }

    // Test STAC Collection links contain items
    TEST_F(StacTest, collectionLinksContainItems)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        auto j = generateStac(ta->getFolder().string());

        EXPECT_TRUE(j.contains("links"));
        EXPECT_TRUE(j["links"].is_array());

        // Should have item links for each indexed entry with geometry
        int itemLinks = 0;
        for (const auto &link : j["links"])
        {
            if (link.contains("rel") && link["rel"] == "item")
            {
                itemLinks++;
            }
        }
        EXPECT_GE(itemLinks, 1) << "Collection should have at least one item link";
    }

    // #440: STAC Item bbox must be a flat array [minX, minY, maxX, maxY], not [[...]]
    TEST_F(StacTest, itemBboxIsFlatArray)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string(), "ortho.tif");

        ASSERT_TRUE(j.contains("bbox"));
        ASSERT_TRUE(j["bbox"].is_array());
        EXPECT_EQ(j["bbox"].size(), 4) << "Item bbox must have exactly 4 elements";

        // Each element must be a number, not an array
        for (size_t i = 0; i < j["bbox"].size(); i++)
        {
            EXPECT_TRUE(j["bbox"][i].is_number())
                << "bbox[" << i << "] should be a number, not an array";
        }
    }

    // Regression: Collection spatial.bbox must remain an array of arrays [[minX, minY, maxX, maxY]]
    TEST_F(StacTest, collectionBboxIsArrayOfArrays)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string());

        ASSERT_TRUE(j.contains("extent"));
        ASSERT_TRUE(j["extent"].contains("spatial"));
        ASSERT_TRUE(j["extent"]["spatial"].contains("bbox"));
        ASSERT_TRUE(j["extent"]["spatial"]["bbox"].is_array());
        ASSERT_GE(j["extent"]["spatial"]["bbox"].size(), 1);

        // First element must be an array (of coordinates)
        auto &firstBbox = j["extent"]["spatial"]["bbox"][0];
        ASSERT_TRUE(firstBbox.is_array()) << "Collection spatial.bbox[0] must be an array";
        EXPECT_GE(firstBbox.size(), 4) << "Collection spatial.bbox[0] must have at least 4 elements";

        for (size_t i = 0; i < firstBbox.size(); i++)
        {
            EXPECT_TRUE(firstBbox[i].is_number())
                << "Collection spatial.bbox[0][" << i << "] should be a number";
        }
    }

    // #441: STAC Item main asset must have roles:["data"] and a valid type (MIME)
    TEST_F(StacTest, itemAssetHasRolesAndType)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        // Check ortho (GeoRaster → image/tiff)
        {
            auto j = generateStac(ta->getFolder().string(), "ortho.tif");
            ASSERT_TRUE(j.contains("assets"));
            ASSERT_TRUE(j["assets"].contains("ortho.tif"));

            auto &asset = j["assets"]["ortho.tif"];
            ASSERT_TRUE(asset.contains("roles"));
            ASSERT_TRUE(asset["roles"].is_array());
            EXPECT_EQ(asset["roles"].size(), 1);
            EXPECT_EQ(asset["roles"][0], "data");

            ASSERT_TRUE(asset.contains("type"));
            EXPECT_EQ(asset["type"], "image/tiff");
        }

        // Check image (GeoImage → image/jpeg)
        {
            auto j = generateStac(ta->getFolder().string(), "DJI_0018.JPG");
            ASSERT_TRUE(j.contains("assets"));
            ASSERT_TRUE(j["assets"].contains("DJI_0018.JPG"));

            auto &asset = j["assets"]["DJI_0018.JPG"];
            ASSERT_TRUE(asset.contains("roles"));
            ASSERT_TRUE(asset["roles"].is_array());
            ASSERT_GE(asset["roles"].size(), 1);
            EXPECT_EQ(asset["roles"][0], "data");

            ASSERT_TRUE(asset.contains("type"));
            EXPECT_EQ(asset["type"], "image/jpeg");
        }
    }

    // #441: STAC Item thumbnail asset must have roles:["thumbnail"] and type
    TEST_F(StacTest, itemThumbnailHasRolesAndType)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string()});

        auto j = generateStac(ta->getFolder().string(), "ortho.tif",
                               "https://example.com/orgs/test/ds/test");

        ASSERT_TRUE(j["assets"].contains("thumbnail"));
        auto &thumb = j["assets"]["thumbnail"];

        ASSERT_TRUE(thumb.contains("roles"));
        ASSERT_TRUE(thumb["roles"].is_array());
        ASSERT_GE(thumb["roles"].size(), 1);
        EXPECT_EQ(thumb["roles"][0], "thumbnail");

        ASSERT_TRUE(thumb.contains("type"));
        EXPECT_EQ(thumb["type"], "image/jpeg");

        ASSERT_TRUE(thumb.contains("href"));
    }

    // #442: STAC Item asset href must be URL-encoded
    TEST_F(StacTest, itemAssetHrefIsUrlEncoded)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);

        // Copy ortho to a path with spaces
        fs::path spacePath = ta->getFolder() / "ortho with spaces.tif";
        fs::copy_file(orthoPath, spacePath);
        ddb::addToIndex(db.get(), {spacePath.string()});

        auto j = generateStac(ta->getFolder().string(), "ortho with spaces.tif",
                               "https://example.com/orgs/test/ds/test");

        ASSERT_TRUE(j.contains("assets"));
        ASSERT_TRUE(j["assets"].contains("ortho with spaces.tif"));

        auto &asset = j["assets"]["ortho with spaces.tif"];
        ASSERT_TRUE(asset.contains("href"));
        std::string href = asset["href"];

        // href must not contain raw spaces
        EXPECT_EQ(href.find(' '), std::string::npos)
            << "Asset href should not contain raw spaces: " << href;

        // href should contain URL-encoded spaces
        EXPECT_NE(href.find("%20"), std::string::npos)
            << "Asset href should contain %20 for spaces: " << href;

        // Thumbnail href should also be encoded (already was, but verify)
        if (j["assets"].contains("thumbnail"))
        {
            auto &thumbAsset = j["assets"]["thumbnail"];
            ASSERT_TRUE(thumbAsset.contains("href"));
            std::string thumbHref = thumbAsset["href"];
            EXPECT_EQ(thumbHref.find(' '), std::string::npos)
                << "Thumbnail href should not contain raw spaces: " << thumbHref;
        }
    }

    // ItemCollection bbox filtering
    TEST_F(StacTest, itemCollectionBboxFilter)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        // Bbox that covers nothing (remote ocean area) → 0 features
        auto fcEmpty = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                   {-179.0, -89.0, -178.0, -88.0});
        EXPECT_EQ(fcEmpty["numberReturned"].get<size_t>(), 0)
            << "Bbox covering no data should return 0 features";
        EXPECT_EQ(fcEmpty["numberMatched"].get<long long>(), 0);

        // World bbox → should return all geometry-bearing features
        auto fcWorld = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                   {-180.0, -90.0, 180.0, 90.0});
        EXPECT_GE(fcWorld["numberReturned"].get<size_t>(), 1)
            << "World bbox should return at least one feature";
        EXPECT_EQ(fcWorld["numberReturned"], fcWorld["numberMatched"]);
    }

    // ItemCollection datetime filtering (including open-ended intervals)
    TEST_F(StacTest, itemCollectionDatetimeFilter)
    {
        ddb::initIndex(ta->getFolder().string());
        auto db = ddb::open(ta->getFolder().string(), true);
        ddb::addToIndex(db.get(), {orthoPath.string(), imagePath.string()});

        // Far-future interval → 0 features
        auto fcFuture = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                    {}, "2099-01-01T00:00:00Z", "2099-12-31T23:59:59Z");
        EXPECT_EQ(fcFuture["numberMatched"].get<long long>(), 0)
            << "Future datetime range should return 0 features";

        // Open-ended start (../now+future): everything before far-future end
        auto fcOpenStart = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                       {}, "", "2099-12-31T23:59:59Z");
        EXPECT_GE(fcOpenStart["numberMatched"].get<long long>(), 1)
            << "Open-start range ending in far future should return all features";

        // Open-ended end (epoch start/..): everything after epoch
        auto fcOpenEnd = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                     {}, "1970-01-01T00:00:00Z", "");
        EXPECT_GE(fcOpenEnd["numberMatched"].get<long long>(), 1)
            << "Open-end range starting at epoch should return all features";

        // Timezone offset handling: same instant expressed differently should give same count
        auto fcUtc  = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                  {}, "1970-01-01T00:00:00Z", "2099-01-01T00:00:00+00:00");
        auto fcOff  = generateStacItemCollection(ta->getFolder().string(), ".", "", "",
                                                  {}, "1970-01-01T00:00:00Z", "2099-01-01T02:00:00+02:00");
        EXPECT_EQ(fcUtc["numberMatched"], fcOff["numberMatched"])
            << "Equivalent datetime with timezone offset should give the same result";
    }

} // namespace

// ---- C API tests for DDBStacItemCollection ----------------------------------

TEST(stacCApi, invalidArgsMissingDdbPath) {
    char *output = nullptr;
    EXPECT_NE(DDBStacItemCollection(nullptr, ".", "", "", nullptr, nullptr, 10, 0, &output), DDBERR_NONE);
    EXPECT_NE(DDBStacItemCollection("", ".", "", "", nullptr, nullptr, 10, 0, &output), DDBERR_NONE);
}

TEST(stacCApi, invalidArgsMissingOutput) {
    char *output = nullptr;
    EXPECT_NE(DDBStacItemCollection("some_path", ".", "", "", nullptr, nullptr, 10, 0, nullptr), DDBERR_NONE);
    (void)output;
}

TEST(stacCApi, invalidArgsBadBbox) {
    char *output = nullptr;
    // Non-numeric token
    EXPECT_NE(DDBStacItemCollection("some_path", ".", "", "", "abc,2,3,4", nullptr, 10, 0, &output), DDBERR_NONE);
    // Wrong number of values (3 instead of 4)
    EXPECT_NE(DDBStacItemCollection("some_path", ".", "", "", "1.0,2.0,3.0", nullptr, 10, 0, &output), DDBERR_NONE);
}

TEST(stacCApi, successReturnsFeatureCollection) {
    TestArea ta(TEST_NAME);
    fs::path ortho = ta.downloadTestAsset(
        "https://github.com/DroneDB/test_data/raw/master/brighton/odm_orthophoto.tif",
        "ortho.tif");

    // Ensure .ddb is removed (stale leftovers from previous runs on Windows)
    auto ddbDir = ta.getFolder() / ".ddb";
    if (fs::exists(ddbDir)) {
        std::error_code ec;
        fs::remove_all(ddbDir, ec);
    }
    ddb::initIndex(ta.getFolder().string());
    auto db = ddb::open(ta.getFolder().string(), true);
    ddb::addToIndex(db.get(), {ortho.string()});

    char *output = nullptr;
    auto err = DDBStacItemCollection(ta.getFolder().string().c_str(),
                                     "http://localhost:7000/orgs/test/ds/test",
                                     "test/ds", "http://localhost:7000",
                                     nullptr, nullptr, 10, 0, &output);
    EXPECT_EQ(err, DDBERR_NONE);
    ASSERT_NE(output, nullptr);

    auto fc = json::parse(std::string(output));
    EXPECT_EQ(fc["type"], "FeatureCollection");
    EXPECT_TRUE(fc.contains("features"));
    EXPECT_GE(fc["features"].size(), 1);

    DDBFree(output);
}
