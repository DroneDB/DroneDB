/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CAMERA_MODEL_H
#define CAMERA_MODEL_H

#include <optional>

#include "exif.h"
#include "json_fwd.h"

namespace ddb
{

    /// A position in an image, in pixels from its upper-left corner
    struct ImagePosition
    {
        double x;
        double y;
    };

    /// The camera of a drone image over flat ground, from the sensor size, the focal length, the
    /// orientation of the camera and the height above the ground. The footprint of the image and the
    /// position of a ground point in the image are the two directions of one model, so they agree: a
    /// corner of the footprint is a corner of the image.
    ///
    /// The ground is measured in metres from the point under the camera, x east and y north in the
    /// frame of a grid (UTM), as the footprint is built in.
    class CameraModel
    {
    public:
        /// The ground the camera sees, in metres from the point under it, before the yaw is applied: the
        /// frame in which the camera looks along +y and its right is +x
        struct GroundExtent
        {
            double left;
            double right;
            double bottom;
            double top;
        };

        CameraModel(const SensorSize &sensorSize, const Focal &focal, const CameraOrientation &orientation,
                    double relAltitude);

        /// The camera of an indexed image, from the properties the index stores (sensorWidth, sensorHeight,
        /// focalLength, the camera orientation, width and height). Empty when one is missing, or the
        /// orientation was not read from the image (it is then a guess, not something to place a marker by)
        static std::optional<CameraModel> fromProperties(const json &properties, double relAltitude);

        /// The height above the ground of a drone, as the footprint reads it: a GPS altitude of 0 means
        /// that the altitude is unknown, and the ground is then at the height above ground itself
        static double groundHeight(double gpsAltitude, double relAltitude);

        /// The height above the ground that the footprint of the image was built with, from the GPS
        /// altitude of the image and the height of its footprint (the inverse of groundHeight)
        static double relAltitudeOf(double gpsAltitude, double groundHeight);

        const CameraOrientation &orientation() const { return orientation_; }

        /// What the camera sees on the ground, before the yaw (see GroundExtent)
        GroundExtent groundExtent() const;

        /// The size of the image, for imagePosition (set by fromProperties)
        void setImageSize(int width, int height);
        int imageWidth() const { return width_; }
        int imageHeight() const { return height_; }

        /// Where a ground point is in the image, the ground point being metres east and north of the
        /// point under the camera. Empty when the image size is not known, the height above the ground is
        /// not positive, or the point is not in the image (the sensor is the image: a point outside it is
        /// outside the footprint)
        std::optional<ImagePosition> imagePosition(double east, double north) const;

    private:
        SensorSize sensorSize_;
        Focal focal_;
        CameraOrientation orientation_;
        double relAltitude_;
        // The fields of view across and along the image, in radians
        double xView_;
        double yView_;
        // The pitch of the model: a camera closer to the horizon than the cap looks down
        double pitch_;
        int width_ = 0;
        int height_ = 0;
    };

}

#endif // CAMERA_MODEL_H
