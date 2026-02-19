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
    // FlightSpeed struct unit tests
    // ========================================================================

    TEST(FlightSpeed, DefaultConstructor)
    {
        FlightSpeed speed;
        EXPECT_DOUBLE_EQ(speed.x, 0.0);
        EXPECT_DOUBLE_EQ(speed.y, 0.0);
        EXPECT_DOUBLE_EQ(speed.z, 0.0);
        EXPECT_DOUBLE_EQ(speed.horizontal(), 0.0);
        EXPECT_DOUBLE_EQ(speed.magnitude(), 0.0);
    }

    TEST(FlightSpeed, ParameterizedConstructor)
    {
        FlightSpeed speed(3.0, 4.0, 0.0);
        EXPECT_DOUBLE_EQ(speed.x, 3.0);
        EXPECT_DOUBLE_EQ(speed.y, 4.0);
        EXPECT_DOUBLE_EQ(speed.z, 0.0);
    }

    TEST(FlightSpeed, HorizontalSpeed)
    {
        // Classic 3-4-5 right triangle
        FlightSpeed speed(3.0, 4.0, 0.0);
        EXPECT_DOUBLE_EQ(speed.horizontal(), 5.0);

        // Vertical component should not affect horizontal speed
        FlightSpeed speed2(3.0, 4.0, 10.0);
        EXPECT_DOUBLE_EQ(speed2.horizontal(), 5.0);
    }

    TEST(FlightSpeed, Magnitude3D)
    {
        // sqrt(1^2 + 2^2 + 2^2) = sqrt(9) = 3
        FlightSpeed speed(1.0, 2.0, 2.0);
        EXPECT_DOUBLE_EQ(speed.magnitude(), 3.0);

        // With zero vertical: magnitude == horizontal
        FlightSpeed speed2(3.0, 4.0, 0.0);
        EXPECT_DOUBLE_EQ(speed2.magnitude(), speed2.horizontal());
    }

    TEST(FlightSpeed, NegativeComponents)
    {
        // Negative velocities (reverse direction) should still give positive magnitude
        FlightSpeed speed(-3.0, -4.0, 0.0);
        EXPECT_DOUBLE_EQ(speed.horizontal(), 5.0);
        EXPECT_GT(speed.magnitude(), 0.0);
    }

    TEST(FlightSpeed, SmallDroneSpeed)
    {
        // Typical DJI mapping flight speed: ~0.3 m/s per axis
        FlightSpeed speed(0.3, 0.3, 0.0);
        EXPECT_NEAR(speed.horizontal(), 0.4243, 1e-3);
        EXPECT_NEAR(speed.magnitude(), 0.4243, 1e-3);
    }

    // ========================================================================
    // parseEntry integration tests: hasCameraOrientation flag
    // ========================================================================

    TEST(parseEntry, DJI_HasCameraOrientation)
    {
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0018.JPG",
            "DJI_0018.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        // Should be detected as GeoImage
        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // hasCameraOrientation should be true for DJI images with XMP gimbal tags
        ASSERT_TRUE(entry.properties.contains("hasCameraOrientation"));
        EXPECT_TRUE(entry.properties["hasCameraOrientation"].get<bool>());

        // Camera orientation values should be present
        ASSERT_TRUE(entry.properties.contains("cameraYaw"));
        ASSERT_TRUE(entry.properties.contains("cameraPitch"));
        ASSERT_TRUE(entry.properties.contains("cameraRoll"));

        // DJI_0018: GimbalYaw=+45.00, GimbalPitch=-89.90, GimbalRoll=+0.00
        EXPECT_NEAR(entry.properties["cameraYaw"].get<double>(), 45.0, 0.5);
        EXPECT_NEAR(entry.properties["cameraPitch"].get<double>(), -89.9, 0.5);
        EXPECT_NEAR(entry.properties["cameraRoll"].get<double>(), 0.0, 0.5);
    }

    TEST(parseEntry, DJI_MultipleImagesHaveOrientation)
    {
        TestArea ta(TEST_NAME);

        fs::path img1 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0018.JPG",
            "DJI_0018.JPG");
        fs::path img2 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0022.JPG",
            "DJI_0022.JPG");
        fs::path img3 = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0032.JPG",
            "DJI_0032.JPG");

        ASSERT_TRUE(fs::exists(img1));
        ASSERT_TRUE(fs::exists(img2));
        ASSERT_TRUE(fs::exists(img3));

        Entry e1, e2, e3;
        parseEntry(img1, ta.getFolder(), e1, false);
        parseEntry(img2, ta.getFolder(), e2, false);
        parseEntry(img3, ta.getFolder(), e3, false);

        // All DJI images should have camera orientation
        EXPECT_TRUE(e1.properties["hasCameraOrientation"].get<bool>());
        EXPECT_TRUE(e2.properties["hasCameraOrientation"].get<bool>());
        EXPECT_TRUE(e3.properties["hasCameraOrientation"].get<bool>());

        // All are nadir shots (GimbalPitch ~ -90°)
        EXPECT_NEAR(e1.properties["cameraPitch"].get<double>(), -89.9, 1.0);
        EXPECT_NEAR(e2.properties["cameraPitch"].get<double>(), -90.0, 1.0);
        EXPECT_NEAR(e3.properties["cameraPitch"].get<double>(), -90.0, 1.0);

        // Different yaw values for different flight directions
        double yaw1 = e1.properties["cameraYaw"].get<double>();
        double yaw2 = e2.properties["cameraYaw"].get<double>();
        EXPECT_TRUE(yaw1 != 0.0 || yaw2 != 0.0) << "At least some images should have non-zero yaw";
    }

    TEST(parseEntry, NonDJI_NoCameraOrientation)
    {
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/test.png",
            "test.png");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        // test.png should have hasCameraOrientation = false (or the property might
        // not be present if it's not an image type that gets EXIF parsing)
        if (entry.properties.contains("hasCameraOrientation"))
        {
            EXPECT_FALSE(entry.properties["hasCameraOrientation"].get<bool>());
        }
    }

    // ========================================================================
    // parseEntry integration tests: flight speed properties
    // ========================================================================

    TEST(parseEntry, DJI_FC300S_NoFlightSpeedProperties)
    {
        // DJI FC300S (Phantom 3) does not have XMP FlightXSpeed tags,
        // so flightSpeed properties should NOT be written
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0022.JPG",
            "DJI_0022.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // Flight speed properties should NOT be present for FC300S
        EXPECT_FALSE(entry.properties.contains("flightSpeed"))
            << "FC300S should not have flightSpeed property (no XMP FlightXSpeed tags)";
        EXPECT_FALSE(entry.properties.contains("flightSpeed3D"))
            << "FC300S should not have flightSpeed3D property";
        EXPECT_FALSE(entry.properties.contains("flightSpeedX"))
            << "FC300S should not have flightSpeedX property";
        EXPECT_FALSE(entry.properties.contains("flightSpeedY"))
            << "FC300S should not have flightSpeedY property";
        EXPECT_FALSE(entry.properties.contains("flightSpeedZ"))
            << "FC300S should not have flightSpeedZ property";
    }

    // ========================================================================
    // parseEntry integration: verify all expected properties are present
    // ========================================================================

    TEST(parseEntry, DJI_AllExpectedProperties)
    {
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images/DJI_0022.JPG",
            "DJI_0022.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        // Standard properties
        EXPECT_TRUE(entry.properties.contains("width"));
        EXPECT_TRUE(entry.properties.contains("height"));
        EXPECT_TRUE(entry.properties.contains("make"));
        EXPECT_TRUE(entry.properties.contains("model"));
        EXPECT_TRUE(entry.properties.contains("captureTime"));

        // Camera orientation properties (should always be present for images)
        EXPECT_TRUE(entry.properties.contains("cameraYaw"));
        EXPECT_TRUE(entry.properties.contains("cameraPitch"));
        EXPECT_TRUE(entry.properties.contains("cameraRoll"));
        EXPECT_TRUE(entry.properties.contains("hasCameraOrientation"));

        // Make and model
        EXPECT_EQ(entry.properties["make"].get<std::string>(), "DJI");
        EXPECT_EQ(entry.properties["model"].get<std::string>(), "FC300S");

        // GPS coordinates should be present
        EXPECT_FALSE(entry.point_geom.empty());
    }

    // ========================================================================
    // parseEntry integration: DJI XMP FlightXSpeed/YSpeed/ZSpeed (Priority 1)
    // ========================================================================

    TEST(parseEntry, DJI_FlightSpeed_SingleImage)
    {
        // DJI_0164: has XMP FlightXSpeed/YSpeed/ZSpeed tags
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-flight-speed/DJI_0164.JPG",
            "DJI_0164.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);
        EXPECT_EQ(entry.properties["make"].get<std::string>(), "DJI");
        EXPECT_EQ(entry.properties["model"].get<std::string>(), "FC300S");

        // Flight speed properties MUST be present
        ASSERT_TRUE(entry.properties.contains("flightSpeed"));
        ASSERT_TRUE(entry.properties.contains("flightSpeed3D"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedX"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedY"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedZ"));

        // DJI_0164: FlightXSpeed=-4.8, FlightYSpeed=-0.2, FlightZSpeed=0.0
        EXPECT_NEAR(entry.properties["flightSpeedX"].get<double>(), -4.8, 0.01);
        EXPECT_NEAR(entry.properties["flightSpeedY"].get<double>(), -0.2, 0.01);
        EXPECT_NEAR(entry.properties["flightSpeedZ"].get<double>(), 0.0, 0.01);

        // Horizontal speed: sqrt(4.8^2 + 0.2^2) ≈ 4.804
        EXPECT_NEAR(entry.properties["flightSpeed"].get<double>(), 4.804, 0.01);

        // 3D speed: sqrt(4.8^2 + 0.2^2 + 0^2) ≈ 4.804 (same since z=0)
        EXPECT_NEAR(entry.properties["flightSpeed3D"].get<double>(), 4.804, 0.01);

        // Camera orientation should also be present
        EXPECT_TRUE(entry.properties["hasCameraOrientation"].get<bool>());
        EXPECT_NEAR(entry.properties["cameraPitch"].get<double>(), -90.0, 0.5);
    }

    TEST(parseEntry, DJI_FlightSpeed_WithVerticalComponent)
    {
        // DJI_0166: has non-zero Z speed component
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-flight-speed/DJI_0166.JPG",
            "DJI_0166.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        ASSERT_TRUE(entry.properties.contains("flightSpeedX"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedY"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedZ"));

        // DJI_0166: FlightXSpeed=-4.7, FlightYSpeed=-0.1, FlightZSpeed=0.1
        EXPECT_NEAR(entry.properties["flightSpeedX"].get<double>(), -4.7, 0.01);
        EXPECT_NEAR(entry.properties["flightSpeedY"].get<double>(), -0.1, 0.01);
        EXPECT_NEAR(entry.properties["flightSpeedZ"].get<double>(), 0.1, 0.01);

        // Horizontal: sqrt(4.7^2 + 0.1^2) ≈ 4.701
        EXPECT_NEAR(entry.properties["flightSpeed"].get<double>(), 4.701, 0.01);

        // 3D: sqrt(4.7^2 + 0.1^2 + 0.1^2) ≈ 4.702 (slightly higher than horizontal)
        double speed3D = entry.properties["flightSpeed3D"].get<double>();
        double speedH = entry.properties["flightSpeed"].get<double>();
        EXPECT_NEAR(speed3D, 4.702, 0.01);
        EXPECT_GT(speed3D, speedH) << "3D speed should be >= horizontal when Z != 0";
    }

    TEST(parseEntry, DJI_FlightSpeed_MultipleImages)
    {
        // All 5 images in images-flight-speed should have flight speed
        TestArea ta(TEST_NAME);

        const std::string baseUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-flight-speed/";
        std::vector<std::string> filenames = {
            "DJI_0164.JPG", "DJI_0165.JPG", "DJI_0166.JPG",
            "DJI_0167.JPG", "DJI_0168.JPG"};

        for (const auto &fn : filenames)
        {
            fs::path imagePath = ta.downloadTestAsset(baseUrl + fn, fn);
            ASSERT_TRUE(fs::exists(imagePath)) << fn << " not found";

            Entry entry;
            parseEntry(imagePath, ta.getFolder(), entry, false);

            EXPECT_EQ(entry.type, EntryType::GeoImage) << fn;

            // All images must have flight speed properties
            ASSERT_TRUE(entry.properties.contains("flightSpeed")) << fn << " missing flightSpeed";
            ASSERT_TRUE(entry.properties.contains("flightSpeed3D")) << fn << " missing flightSpeed3D";
            ASSERT_TRUE(entry.properties.contains("flightSpeedX")) << fn << " missing flightSpeedX";
            ASSERT_TRUE(entry.properties.contains("flightSpeedY")) << fn << " missing flightSpeedY";
            ASSERT_TRUE(entry.properties.contains("flightSpeedZ")) << fn << " missing flightSpeedZ";

            // Speed should be positive and reasonable for a mapping drone (< 30 m/s)
            double speed = entry.properties["flightSpeed"].get<double>();
            EXPECT_GT(speed, 0.0) << fn << " has zero horizontal speed";
            EXPECT_LT(speed, 30.0) << fn << " has unreasonably high speed";

            // Camera orientation must be present
            EXPECT_TRUE(entry.properties["hasCameraOrientation"].get<bool>()) << fn;
        }
    }

    // ========================================================================
    // parseEntry integration: EXIF GPSSpeed (Priority 2)
    // ========================================================================

    TEST(parseEntry, GPSSpeed_SingleImage)
    {
        // Parrot Sequoia image with EXIF GPSSpeed tag
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-speed/IMG_161122_163234_0000_RGB.JPG",
            "IMG_161122_163234_0000_RGB.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);
        EXPECT_EQ(entry.properties["make"].get<std::string>(), "Parrot");
        EXPECT_EQ(entry.properties["model"].get<std::string>(), "Sequoia");

        // Flight speed properties MUST be present (from GPSSpeed)
        ASSERT_TRUE(entry.properties.contains("flightSpeed"));
        ASSERT_TRUE(entry.properties.contains("flightSpeed3D"));
        ASSERT_TRUE(entry.properties.contains("flightSpeedX"));

        // GPSSpeed is scalar: stored as x=speed, y=0, z=0
        double speedX = entry.properties["flightSpeedX"].get<double>();
        double speedY = entry.properties["flightSpeedY"].get<double>();
        double speedZ = entry.properties["flightSpeedZ"].get<double>();

        EXPECT_GT(speedX, 0.0) << "Scalar GPSSpeed stored in X component";
        EXPECT_DOUBLE_EQ(speedY, 0.0) << "GPSSpeed has no Y component";
        EXPECT_DOUBLE_EQ(speedZ, 0.0) << "GPSSpeed has no Z component";

        // IMG_..._0000: GPSSpeed ≈ 12.63 m/s (converted from km/h)
        EXPECT_NEAR(entry.properties["flightSpeed"].get<double>(), 12.63, 0.1);

        // For scalar speed: horizontal == 3D == X
        EXPECT_DOUBLE_EQ(entry.properties["flightSpeed"].get<double>(),
                         entry.properties["flightSpeed3D"].get<double>());
    }

    TEST(parseEntry, GPSSpeed_DifferentSpeed)
    {
        // Another Parrot Sequoia image with different speed
        TestArea ta(TEST_NAME);
        fs::path imagePath = ta.downloadTestAsset(
            "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-speed/IMG_161122_163249_0003_RGB.JPG",
            "IMG_161122_163249_0003_RGB.JPG");

        ASSERT_TRUE(fs::exists(imagePath)) << "Test image not found";

        Entry entry;
        parseEntry(imagePath, ta.getFolder(), entry, false);

        EXPECT_EQ(entry.type, EntryType::GeoImage);

        ASSERT_TRUE(entry.properties.contains("flightSpeed"));

        // IMG_..._0003: GPSSpeed ≈ 9.96 m/s
        EXPECT_NEAR(entry.properties["flightSpeed"].get<double>(), 9.96, 0.1);

        // Camera orientation should be present (Parrot Sequoia has XMP tags)
        EXPECT_TRUE(entry.properties["hasCameraOrientation"].get<bool>());
    }

    TEST(parseEntry, GPSSpeed_MultipleImages)
    {
        // All 6 images in images-gps-speed should have flight speed from GPSSpeed
        TestArea ta(TEST_NAME);

        const std::string baseUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/images-gps-speed/";
        std::vector<std::string> filenames = {
            "IMG_161122_163234_0000_RGB.JPG",
            "IMG_161122_163239_0001_RGB.JPG",
            "IMG_161122_163244_0002_RGB.JPG",
            "IMG_161122_163249_0003_RGB.JPG",
            "IMG_161122_163338_0011_RGB.JPG",
            "IMG_161122_163342_0012_RGB.JPG"};

        for (const auto &fn : filenames)
        {
            fs::path imagePath = ta.downloadTestAsset(baseUrl + fn, fn);
            ASSERT_TRUE(fs::exists(imagePath)) << fn << " not found";

            Entry entry;
            parseEntry(imagePath, ta.getFolder(), entry, false);

            EXPECT_EQ(entry.type, EntryType::GeoImage) << fn;

            // All images must have flight speed from GPSSpeed
            ASSERT_TRUE(entry.properties.contains("flightSpeed")) << fn << " missing flightSpeed";
            ASSERT_TRUE(entry.properties.contains("flightSpeed3D")) << fn << " missing flightSpeed3D";

            double speed = entry.properties["flightSpeed"].get<double>();
            EXPECT_GT(speed, 0.0) << fn << " has zero speed";
            EXPECT_LT(speed, 30.0) << fn << " has unreasonably high speed";

            // GPSSpeed is scalar: Y and Z should be 0
            EXPECT_DOUBLE_EQ(entry.properties["flightSpeedY"].get<double>(), 0.0)
                << fn << " should have Y=0 for scalar GPSSpeed";
            EXPECT_DOUBLE_EQ(entry.properties["flightSpeedZ"].get<double>(), 0.0)
                << fn << " should have Z=0 for scalar GPSSpeed";

            // GPS coordinates should be present
            EXPECT_FALSE(entry.point_geom.empty()) << fn;
        }
    }

} // namespace
