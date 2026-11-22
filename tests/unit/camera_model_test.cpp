/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The camera model that the footprint of an image and the position of a ground point in it share: a
// corner of the ground it sees is a corner of the image, and the point under a nadir camera is the
// middle of the image.

#include <cmath>

#include "camera_model.h"
#include "geo.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mathutil.h"

namespace
{
    using namespace ddb;

    constexpr int Width = 5472;
    constexpr int Height = 3648;

    CameraModel modelOf(double pitch, double yaw, double roll, double relAltitude = 100.0)
    {
        CameraModel model(SensorSize(13.2, 8.8), Focal(8.8, 24.0), CameraOrientation(pitch, yaw, roll), relAltitude);
        model.setImageSize(Width, Height);
        return model;
    }

    // A corner of the ground the camera sees, as metres east and north of the point under the camera: the footprint
    // is built from the corners of the extent, turned by -yaw
    Projected2D groundOf(const CameraModel &model, double x, double y)
    {
        Projected2D corner(x, y);
        corner.rotate(Projected2D(0.0, 0.0), -model.orientation().yaw);
        return corner;
    }

    void expectImagePosition(const CameraModel &model, const Projected2D &ground, double x, double y)
    {
        const auto position = model.imagePosition(ground.x, ground.y);
        ASSERT_TRUE(position.has_value());
        EXPECT_NEAR(position->x, x, 1e-6);
        EXPECT_NEAR(position->y, y, 1e-6);
    }

    TEST(CameraModel, GroundCornersAreImageCorners)
    {
        // Nadir, tilted, rolled and turned cameras
        const double orientations[][3] = {{-90, 0, 0}, {-90, 135, 0}, {-60, 30, 15}, {-45, -200, -8}, {-35, 270, 5}};
        for (const auto &o : orientations)
        {
            const auto model = modelOf(o[0], o[1], o[2]);
            const auto extent = model.groundExtent();
            SCOPED_TRACE("pitch " + std::to_string(o[0]) + " yaw " + std::to_string(o[1]) + " roll " + std::to_string(o[2]));
            expectImagePosition(model, groundOf(model, extent.left, extent.top), 0, 0);
            expectImagePosition(model, groundOf(model, extent.right, extent.top), Width, 0);
            expectImagePosition(model, groundOf(model, extent.left, extent.bottom), 0, Height);
            expectImagePosition(model, groundOf(model, extent.right, extent.bottom), Width, Height);
        }
    }

    TEST(CameraModel, PointUnderNadirCameraIsTheMiddle)
    {
        for (const double yaw : {0.0, 90.0, -135.0, 311.0})
            expectImagePosition(modelOf(-90, yaw, 0), Projected2D(0.0, 0.0), Width / 2.0, Height / 2.0);
    }

    TEST(CameraModel, AxisOfATiltedCameraIsTheMiddle)
    {
        // The ground the optical axis meets: pitch -60 is 30 degrees from the nadir, along the yaw, and a
        // roll moves it across
        const auto model = modelOf(-60, 30, 10);
        const double along = 100.0 * std::tan(deg2rad(30.0));
        const double across = 100.0 * std::tan(deg2rad(10.0));
        expectImagePosition(model, groundOf(model, across, along), Width / 2.0, Height / 2.0);
    }

    TEST(CameraModel, ImageSidesAreTheGroundEdges)
    {
        const auto model = modelOf(-90, 0, 0);
        const auto extent = model.groundExtent();
        // North up: the middle of the top edge, then of the left one
        expectImagePosition(model, Projected2D(0.0, extent.top), Width / 2.0, 0);
        expectImagePosition(model, Projected2D(extent.left, 0.0), 0, Height / 2.0);
        // East is to the right, and south is down
        const auto east = model.imagePosition(10.0, 0.0);
        const auto south = model.imagePosition(0.0, -10.0);
        ASSERT_TRUE(east && south);
        EXPECT_GT(east->x, Width / 2.0);
        EXPECT_GT(south->y, Height / 2.0);
    }

    TEST(CameraModel, YawTurnsTheImage)
    {
        // A camera that looks east has the east at the top of its image
        const auto model = modelOf(-90, 90, 0);
        const auto east = model.imagePosition(10.0, 0.0);
        ASSERT_TRUE(east.has_value());
        EXPECT_NEAR(east->x, Width / 2.0, 1e-6);
        EXPECT_LT(east->y, Height / 2.0);
    }

    TEST(CameraModel, PointsOutsideTheImageHaveNoPosition)
    {
        const auto model = modelOf(-90, 0, 0);
        const auto extent = model.groundExtent();
        EXPECT_FALSE(model.imagePosition(extent.right * 1.01, 0.0).has_value());
        EXPECT_FALSE(model.imagePosition(0.0, extent.bottom * 1.01).has_value());
        EXPECT_FALSE(model.imagePosition(1.0e6, 1.0e6).has_value());
        EXPECT_FALSE(model.imagePosition(std::nan(""), 0.0).has_value());
    }

    TEST(CameraModel, NoPositionWithoutImageSizeOrHeight)
    {
        CameraModel noSize(SensorSize(13.2, 8.8), Focal(8.8, 24.0), CameraOrientation(-90, 0, 0), 100.0);
        EXPECT_FALSE(noSize.imagePosition(0.0, 0.0).has_value());
        EXPECT_FALSE(modelOf(-90, 0, 0, 0.0).imagePosition(0.0, 0.0).has_value());
        EXPECT_FALSE(modelOf(-90, 0, 0, -5.0).imagePosition(0.0, 0.0).has_value());
    }

    TEST(CameraModel, PitchAboveTheCapIsNadir)
    {
        const auto capped = modelOf(-10, 0, 0).groundExtent();
        const auto nadir = modelOf(-90, 0, 0).groundExtent();
        EXPECT_EQ(capped.left, nadir.left);
        EXPECT_EQ(capped.right, nadir.right);
        EXPECT_EQ(capped.top, nadir.top);
        EXPECT_EQ(capped.bottom, nadir.bottom);
    }

    TEST(CameraModel, GroundHeightAndRelAltitudeAreInverse)
    {
        for (const double gps : {198.31, 0.0, -3.5})
            for (const double rel : {100.0, 12.25})
            {
                const double ground = CameraModel::groundHeight(gps, rel);
                EXPECT_NEAR(CameraModel::relAltitudeOf(gps, ground), rel, 1e-9) << gps << " " << rel;
            }
        // No GPS altitude: the ground is at the height above ground
        EXPECT_EQ(CameraModel::groundHeight(0.0, 80.0), 80.0);
        EXPECT_EQ(CameraModel::groundHeight(198.31, 100.0), 198.31 - 100.0);
    }

    json fullProperties()
    {
        return json{{"width", Width},
                    {"height", Height},
                    {"sensorWidth", 13.2},
                    {"sensorHeight", 8.8},
                    {"focalLength", 8.8},
                    {"cameraYaw", 20.0},
                    {"cameraPitch", -90.0},
                    {"cameraRoll", 0.0},
                    {"hasCameraOrientation", true}};
    }

    TEST(CameraModel, FromProperties)
    {
        const auto model = CameraModel::fromProperties(fullProperties(), 100.0);
        ASSERT_TRUE(model.has_value());
        EXPECT_EQ(model->imageWidth(), Width);
        EXPECT_EQ(model->imageHeight(), Height);
        EXPECT_EQ(model->orientation().yaw, 20.0);
        const auto middle = model->imagePosition(0.0, 0.0);
        ASSERT_TRUE(middle.has_value());
        EXPECT_NEAR(middle->x, Width / 2.0, 1e-6);
        EXPECT_NEAR(middle->y, Height / 2.0, 1e-6);
    }

    TEST(CameraModel, FromPropertiesNeedsEveryParameter)
    {
        for (const char *missing : {"width", "height", "sensorWidth", "sensorHeight", "focalLength", "cameraYaw",
                                    "cameraPitch", "cameraRoll", "hasCameraOrientation"})
        {
            json properties = fullProperties();
            properties.erase(missing);
            EXPECT_FALSE(CameraModel::fromProperties(properties, 100.0).has_value()) << missing;
        }

        // An orientation the tags did not give is a guess, not a camera
        json guessed = fullProperties();
        guessed["hasCameraOrientation"] = false;
        EXPECT_FALSE(CameraModel::fromProperties(guessed, 100.0).has_value());

        // A size or a focal length of 0, a number that is a string, a document that is not an object
        json zero = fullProperties();
        zero["focalLength"] = 0;
        EXPECT_FALSE(CameraModel::fromProperties(zero, 100.0).has_value());
        json text = fullProperties();
        text["sensorWidth"] = "13.2";
        EXPECT_FALSE(CameraModel::fromProperties(text, 100.0).has_value());
        EXPECT_FALSE(CameraModel::fromProperties(json::array(), 100.0).has_value());
    }
}
