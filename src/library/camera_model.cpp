/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "camera_model.h"

#include <cmath>

#include "geo.h"
#include "json.h"
#include "logger.h"
#include "mathutil.h"

namespace ddb
{

    namespace
    {
        // A finite number of a property, or nothing
        std::optional<double> numberOf(const json &properties, const char *name)
        {
            const auto it = properties.find(name);
            if (it == properties.end() || !it->is_number())
                return std::nullopt;
            const double value = it->get<double>();
            return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
        }

        // A positive finite number of a property, or nothing
        std::optional<double> positiveOf(const json &properties, const char *name)
        {
            const auto value = numberOf(properties, name);
            return value && *value > 0.0 ? value : std::nullopt;
        }
    } // namespace

    CameraModel::CameraModel(const SensorSize &sensorSize, const Focal &focal, const CameraOrientation &orientation,
                             double relAltitude)
        : sensorSize_(sensorSize), focal_(focal), orientation_(orientation), relAltitude_(relAltitude)
    {
        // Field of view
        // Wide
        xView_ = 2.0 * atan(sensorSize.width / (2.0 * focal.length));

        // Tall
        yView_ = 2.0 * atan(sensorSize.height / (2.0 * focal.length));

        // A camera that looks less than 30 degrees below the horizon is taken as nadir
        pitch_ = orientation.pitch;
        if (pitch_ > -30)
        {
            LOGD << "Pitch cap exceeded (" << pitch_ << ") using nadir";
            pitch_ = -90; // set to nadir
        }
    }

    std::optional<CameraModel> CameraModel::fromProperties(const json &properties, double relAltitude)
    {
        if (!properties.is_object())
            return std::nullopt;

        const auto oriented = properties.find("hasCameraOrientation");
        if (oriented == properties.end() || !oriented->is_boolean() || !oriented->get<bool>())
            return std::nullopt;

        const auto sensorWidth = positiveOf(properties, "sensorWidth");
        const auto sensorHeight = positiveOf(properties, "sensorHeight");
        const auto focalLength = positiveOf(properties, "focalLength");
        const auto pitch = numberOf(properties, "cameraPitch");
        const auto yaw = numberOf(properties, "cameraYaw");
        const auto roll = numberOf(properties, "cameraRoll");
        const auto width = positiveOf(properties, "width");
        const auto height = positiveOf(properties, "height");
        if (!sensorWidth || !sensorHeight || !focalLength || !pitch || !yaw || !roll || !width || !height)
            return std::nullopt;

        CameraModel model(SensorSize(*sensorWidth, *sensorHeight), Focal(*focalLength, 0.0),
                          CameraOrientation(*pitch, *yaw, *roll), relAltitude);
        model.setImageSize(static_cast<int>(*width), static_cast<int>(*height));
        return model;
    }

    double CameraModel::groundHeight(double gpsAltitude, double relAltitude)
    {
        return gpsAltitude != 0.0 ? gpsAltitude - relAltitude : relAltitude;
    }

    double CameraModel::relAltitudeOf(double gpsAltitude, double groundHeight)
    {
        return gpsAltitude != 0.0 ? gpsAltitude - groundHeight : groundHeight;
    }

    void CameraModel::setImageSize(int width, int height)
    {
        width_ = width;
        height_ = height;
    }

    CameraModel::GroundExtent CameraModel::groundExtent() const
    {
        // From drone to...
        GroundExtent extent;
        extent.bottom = relAltitude_ * tan(ddb::deg2rad(90.0 + pitch_) - 0.5 * yView_);
        extent.top = relAltitude_ * tan(ddb::deg2rad(90.0 + pitch_) + 0.5 * yView_);
        extent.left = relAltitude_ * tan(ddb::deg2rad(orientation_.roll) - 0.5 * xView_);
        extent.right = relAltitude_ * tan(ddb::deg2rad(orientation_.roll) + 0.5 * xView_);
        // ... of picture.
        return extent;
    }

    std::optional<ImagePosition> CameraModel::imagePosition(double east, double north) const
    {
        if (width_ <= 0 || height_ <= 0 || !(relAltitude_ > 0.0) || !std::isfinite(east) || !std::isfinite(north))
            return std::nullopt;

        // The footprint turns the ground by -yaw; this turns it back, into the frame of the camera
        Projected2D ground(east, north);
        ground.rotate(Projected2D(0.0, 0.0), orientation_.yaw);

        // The angles from the axis of the camera at which the ground point is seen (the footprint is
        // relAltitude * tan of them, so they are the inverse of its edges)
        const double across = atan(ground.x / relAltitude_) - ddb::deg2rad(orientation_.roll);
        const double along = atan(ground.y / relAltitude_) - ddb::deg2rad(90.0 + pitch_);

        // Within the field of view; a small slack keeps the corners of the footprint in the image
        constexpr double slack = 1e-9;
        if (std::abs(across) > 0.5 * xView_ + slack || std::abs(along) > 0.5 * yView_ + slack)
            return std::nullopt;

        // On the sensor, in millimetres from its centre, and in the image
        const double sensorX = focal_.length * tan(across);
        const double sensorY = focal_.length * tan(along);
        return ImagePosition{width_ * (0.5 + sensorX / sensorSize_.width),
                             height_ * (0.5 - sensorY / sensorSize_.height)};
    }

}
