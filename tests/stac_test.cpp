/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "dbops.h"
#include "stac.h"
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
        EXPECT_EQ(j["stac_version"], "1.0.0");

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
        EXPECT_EQ(j["stac_version"], "1.0.0");

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
        EXPECT_EQ(j["stac_version"], "1.0.0");

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

} // namespace
