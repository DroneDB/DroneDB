/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "entry.h"
#include "exif.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // ========================================================================
    // GpsAccuracy struct unit tests
    // ========================================================================

    TEST(GpsAccuracy, DefaultConstructor)
    {
        GpsAccuracy acc;
        EXPECT_DOUBLE_EQ(acc.xyAccuracy, -1.0);
        EXPECT_DOUBLE_EQ(acc.zAccuracy, -1.0);
        EXPECT_DOUBLE_EQ(acc.dop, -1.0);
        EXPECT_FALSE(acc.hasData());
    }

    TEST(GpsAccuracy, ParameterizedConstructor)
    {
        GpsAccuracy acc(2.5, 3.0, 1.2);
        EXPECT_DOUBLE_EQ(acc.xyAccuracy, 2.5);
        EXPECT_DOUBLE_EQ(acc.zAccuracy, 3.0);
        EXPECT_DOUBLE_EQ(acc.dop, 1.2);
        EXPECT_TRUE(acc.hasData());
    }

    TEST(GpsAccuracy, HasDataPartial)
    {
        // Only XY accuracy set
        GpsAccuracy acc1;
        acc1.xyAccuracy = 5.0;
        EXPECT_TRUE(acc1.hasData());

        // Only Z accuracy set
        GpsAccuracy acc2;
        acc2.zAccuracy = 3.0;
        EXPECT_TRUE(acc2.hasData());

        // Only DOP set
        GpsAccuracy acc3;
        acc3.dop = 1.5;
        EXPECT_TRUE(acc3.hasData());
    }

    // ========================================================================
    // parseEntry integration tests: GPS accuracy (XMP Camera namespace)
    // ========================================================================

    TEST(parseEntry, SenseFly_GPSAccuracy)
    {
        // images-gps-acc contains senseFly/Parrot images with Xmp.Camera.GPSXYAccuracy/GPSZAccuracy
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1297_RGB.jpg",
            "IMG_1297_RGB.jpg");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        // Should be detected as GeoImage
        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // GPS accuracy properties should be present
        ASSERT_TRUE(entry.properties.contains("gpsXYAccuracy"))
            << "gpsXYAccuracy should be present for senseFly/Parrot images";
        ASSERT_TRUE(entry.properties.contains("gpsZAccuracy"))
            << "gpsZAccuracy should be present for senseFly/Parrot images";

        // Verify values from ExifTool analysis:
        // GPSXYAccuracy = 5.348000049591064
        // GPSZAccuracy = 6.131999969482422
        EXPECT_NEAR(entry.properties["gpsXYAccuracy"].get<double>(), 5.348, 0.01);
        EXPECT_NEAR(entry.properties["gpsZAccuracy"].get<double>(), 6.132, 0.01);

        // gpsDop should NOT be present (not in XMP Camera tags)
        EXPECT_FALSE(entry.properties.contains("gpsDop"));
    }

    TEST(parseEntry, SenseFly_MultipleImages_GPSAccuracy)
    {
        TestArea ta(TEST_NAME);

        fs::path img1 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1297_RGB.jpg",
            "IMG_1297_RGB.jpg");
        fs::path img2 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1298_RGB.jpg",
            "IMG_1298_RGB.jpg");
        fs::path img3 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1299_RGB.jpg",
            "IMG_1299_RGB.jpg");

        ASSERT_TRUE(fs::exists(img1));
        ASSERT_TRUE(fs::exists(img2));
        ASSERT_TRUE(fs::exists(img3));

        Entry e1, e2, e3;
        parseEntry(img1, ta.getFolder(), e1, false);
        parseEntry(img2, ta.getFolder(), e2, false);
        parseEntry(img3, ta.getFolder(), e3, false);

        // All images should have GPS accuracy
        EXPECT_TRUE(e1.properties.contains("gpsXYAccuracy"));
        EXPECT_TRUE(e2.properties.contains("gpsXYAccuracy"));
        EXPECT_TRUE(e3.properties.contains("gpsXYAccuracy"));

        EXPECT_TRUE(e1.properties.contains("gpsZAccuracy"));
        EXPECT_TRUE(e2.properties.contains("gpsZAccuracy"));
        EXPECT_TRUE(e3.properties.contains("gpsZAccuracy"));

        // All accuracy values should be positive
        EXPECT_GT(e1.properties["gpsXYAccuracy"].get<double>(), 0.0);
        EXPECT_GT(e2.properties["gpsXYAccuracy"].get<double>(), 0.0);
        EXPECT_GT(e3.properties["gpsXYAccuracy"].get<double>(), 0.0);
    }

    // ========================================================================
    // parseEntry integration tests: DJI images without GPS accuracy
    // ========================================================================

    TEST(parseEntry, DJI_NoGPSAccuracy)
    {
        // Standard DJI images should NOT have GPS accuracy properties
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0018.JPG",
            "DJI_0018.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // GPS accuracy properties should NOT be present for standard DJI (non-RTK)
        EXPECT_FALSE(entry.properties.contains("gpsXYAccuracy"))
            << "Standard DJI should not have gpsXYAccuracy";
        EXPECT_FALSE(entry.properties.contains("gpsZAccuracy"))
            << "Standard DJI should not have gpsZAccuracy";
        EXPECT_FALSE(entry.properties.contains("gpsDop"))
            << "Standard DJI should not have gpsDop";
    }

    // ========================================================================
    // parseEntry integration tests: captureTime uses GPS time when available
    // ========================================================================

    TEST(parseEntry, DJI_CaptureTimePresent)
    {
        // Standard DJI images should have captureTime even without GPSDateStamp/GPSTimeStamp
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0018.JPG",
            "DJI_0018.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // captureTime should always be present
        ASSERT_TRUE(entry.properties.contains("captureTime"))
            << "captureTime should always be present for images";
        EXPECT_GT(entry.properties["captureTime"].get<double>(), 0.0)
            << "captureTime should be a positive epoch value";
    }

    TEST(parseEntry, SenseFly_CaptureTimePresent)
    {
        // senseFly images should have captureTime
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1297_RGB.jpg",
            "IMG_1297_RGB.jpg");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // captureTime should be present
        ASSERT_TRUE(entry.properties.contains("captureTime"))
            << "captureTime should always be present for images";
        EXPECT_GT(entry.properties["captureTime"].get<double>(), 0.0)
            << "captureTime should be a positive epoch value";

        // gpsTime field should NOT exist anymore (unified into captureTime)
        EXPECT_FALSE(entry.properties.contains("gpsTime"))
            << "gpsTime field should no longer be emitted";
    }

    // ========================================================================
    // parseEntry integration: GPS accuracy + other properties coexistence
    // ========================================================================

    TEST(parseEntry, SenseFly_AllPropertiesPresent)
    {
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-acc/IMG_1297_RGB.jpg",
            "IMG_1297_RGB.jpg");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // Standard properties should still be present
        EXPECT_TRUE(entry.properties.contains("width"));
        EXPECT_TRUE(entry.properties.contains("height"));
        EXPECT_TRUE(entry.properties.contains("captureTime"));
        EXPECT_TRUE(entry.properties.contains("make"));
        EXPECT_TRUE(entry.properties.contains("model"));

        // GPS accuracy properties
        EXPECT_TRUE(entry.properties.contains("gpsXYAccuracy"));
        EXPECT_TRUE(entry.properties.contains("gpsZAccuracy"));

        // Camera orientation should be present (XMP Camera namespace)
        EXPECT_TRUE(entry.properties.contains("cameraYaw"));
        EXPECT_TRUE(entry.properties.contains("cameraPitch"));
        EXPECT_TRUE(entry.properties.contains("cameraRoll"));

        // GPS coordinates should be present
        EXPECT_FALSE(entry.point_geom.empty());
    }

}
