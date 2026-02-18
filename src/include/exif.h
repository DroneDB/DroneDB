/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_H
#define EXIF_H

#include <exiv2/exiv2.hpp>
#include <memory>
#include <stdio.h>
#include <cmath>
#include "utils.h"
#include "sensor_data.h"
#include "ddb_export.h"

namespace ddb
{

    struct ImageSize
    {
        int width;
        int height;
        DDB_DLL ImageSize(int width, int height) : width(width), height(height) {};
    };

    struct Focal
    {
        double length;   // in mm
        double length35; // in 35mm film equivalent
        DDB_DLL Focal() : length(0), length35(0) {};
        DDB_DLL Focal(double length, double length35) : length(length), length35(length35) {};
    };

    struct SensorSize
    {
        double width;  // mm
        double height; // mm
        DDB_DLL SensorSize() : width(0), height(0) {};
        DDB_DLL SensorSize(double width, double height) : width(width), height(height) {};
    };

    struct GeoLocation
    {
        double latitude;
        double longitude;
        double altitude;

        DDB_DLL GeoLocation() : latitude(0), longitude(0), altitude(0) {}
        DDB_DLL GeoLocation(double latitude, double longitude, double altitude) : latitude(latitude), longitude(longitude), altitude(altitude) {};
    };

    struct PanoramaInfo
    {
        std::string projectionType;
        int croppedWidth;
        int croppedHeight;
        int croppedX;
        int croppedY;
        float poseHeading; // 0 to 360
        float posePitch;   // -90 to 90
        float poseRoll;    // -180 to 180
    };

    struct CameraOrientation
    {
        double pitch; // degrees. -90 = nadir, 0 = front straight
        double yaw;   // degress. 0 = magnetic north, 90 = east, -90 = west, 180 = south
        double roll;  // degrees. 20 = left roll, -20 = right roll

        DDB_DLL CameraOrientation() : pitch(0), yaw(0), roll(0) {};
        DDB_DLL CameraOrientation(double pitch, double yaw, double roll) : pitch(pitch), yaw(yaw), roll(roll) {};
    };
    inline std::ostream &operator<<(std::ostream &os, const CameraOrientation &c)
    {
        os << "Pitch: " << c.pitch << " | Yaw: " << c.yaw << " | Roll: " << c.roll;
        return os;
    }

    struct FlightSpeed
    {
        double x;  // m/s, east-west axis (positive = east)
        double y;  // m/s, north-south axis (positive = north)
        double z;  // m/s, vertical axis (positive = up)

        DDB_DLL FlightSpeed() : x(0), y(0), z(0) {};
        DDB_DLL FlightSpeed(double x, double y, double z) : x(x), y(y), z(z) {};

        // Horizontal speed magnitude in m/s
        DDB_DLL double horizontal() const { return std::sqrt(x * x + y * y); }

        // 3D speed magnitude in m/s
        DDB_DLL double magnitude() const { return std::sqrt(x * x + y * y + z * z); }
    };
    inline std::ostream &operator<<(std::ostream &os, const FlightSpeed &s)
    {
        os << "X: " << s.x << " | Y: " << s.y << " | Z: " << s.z << " | Horizontal: " << s.horizontal() << " | 3D: " << s.magnitude();
        return os;
    }

    class ExifParser
    {
        Exiv2::Image *image;
        Exiv2::ExifData exifData;
        Exiv2::XmpData xmpData;

    public:
        ExifParser(Exiv2::Image *image) : image(image), exifData(image->exifData()), xmpData(image->xmpData()) {};

        Exiv2::ExifData::const_iterator findExifKey(const std::string &key);
        Exiv2::ExifData::const_iterator findExifKey(const std::initializer_list<std::string> &keys);
        Exiv2::XmpData::const_iterator findXmpKey(const std::string &key);
        Exiv2::XmpData::const_iterator findXmpKey(const std::initializer_list<std::string> &keys);

        ImageSize extractImageSize();
        ImageSize extractVideoSize();
        std::string extractMake();
        std::string extractModel();
        std::string extractSensor();
        bool computeFocal(Focal &f);
        bool extractSensorSize(SensorSize &r);
        inline double getMmPerUnit(long resolutionUnit);

        bool extractGeo(GeoLocation &geo);
        bool extractRelAltitude(double &relAltitude);
        inline double geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag);
        inline double evalFrac(const Exiv2::Rational &rational);

        double extractCaptureTime();
        int extractImageOrientation();

        bool extractCameraOrientation(CameraOrientation &cameraOri);
        bool extractFlightSpeed(FlightSpeed &speed);
        bool extractPanoramaInfo(PanoramaInfo &info);

        void printAllTags();

        bool hasExif();
        bool hasXmp();
        bool hasTags();
    };

    /**
     * @brief Context for caching Exiv2 image and parser to avoid duplicate file reads.
     *
     * This struct allows fingerprint() and parseEntry() to share the same Exiv2::Image
     * and ExifParser instance, eliminating the need to open and read the file twice.
     */
    struct FingerprintContext
    {
        std::unique_ptr<Exiv2::Image> exivImage;
        std::unique_ptr<ExifParser> parser;
        GeoLocation geo;
        bool hasGeo = false;
        bool populated = false;

        FingerprintContext() = default;

        // Non-copyable, moveable
        FingerprintContext(const FingerprintContext &) = delete;
        FingerprintContext &operator=(const FingerprintContext &) = delete;
        FingerprintContext(FingerprintContext &&) = default;
        FingerprintContext &operator=(FingerprintContext &&) = default;
    };

}

#endif // EXIF_H
