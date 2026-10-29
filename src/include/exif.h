/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_H
#define EXIF_H

#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <stdio.h>
#include <cmath>
#include <string>
#include <utility>
#include "exif_metadata.h"
#include "fs.h"
#include "sensor_data.h"

namespace ddb
{

    struct ImageSize
    {
        int width;
        int height;
        ImageSize(int width, int height) : width(width), height(height) {};
    };

    struct Focal
    {
        double length;   // in mm
        double length35; // in 35mm film equivalent
        Focal() : length(0), length35(0) {};
        Focal(double length, double length35) : length(length), length35(length35) {};
    };

    struct SensorSize
    {
        double width;  // mm
        double height; // mm
        SensorSize() : width(0), height(0) {};
        SensorSize(double width, double height) : width(width), height(height) {};
    };

    struct GeoLocation
    {
        double latitude;
        double longitude;
        double altitude;

        GeoLocation() : latitude(0), longitude(0), altitude(0) {}
        GeoLocation(double latitude, double longitude, double altitude) : latitude(latitude), longitude(longitude), altitude(altitude) {};
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

        CameraOrientation() : pitch(0), yaw(0), roll(0) {};
        CameraOrientation(double pitch, double yaw, double roll) : pitch(pitch), yaw(yaw), roll(roll) {};
    };
    inline std::ostream &operator<<(std::ostream &os, const CameraOrientation &c)
    {
        os << "Pitch: " << c.pitch << " | Yaw: " << c.yaw << " | Roll: " << c.roll;
        return os;
    }

    struct GpsAccuracy
    {
        double xyAccuracy;  // meters, horizontal accuracy (-1 = not available)
        double zAccuracy;   // meters, vertical accuracy (-1 = not available)
        double dop;         // dilution of precision (-1 = not available)

        GpsAccuracy() : xyAccuracy(-1), zAccuracy(-1), dop(-1) {};
        GpsAccuracy(double xyAccuracy, double zAccuracy, double dop) : xyAccuracy(xyAccuracy), zAccuracy(zAccuracy), dop(dop) {};

        // Returns true if at least one accuracy value is available
        bool hasData() const { return xyAccuracy >= 0 || zAccuracy >= 0 || dop >= 0; }
    };
    inline std::ostream &operator<<(std::ostream &os, const GpsAccuracy &a)
    {
        os << "XY: " << a.xyAccuracy << "m | Z: " << a.zAccuracy << "m | DOP: " << a.dop;
        return os;
    }

    struct FlightSpeed
    {
        double x;  // m/s, east-west axis (positive = east)
        double y;  // m/s, north-south axis (positive = north)
        double z;  // m/s, vertical axis (positive = up)

        FlightSpeed() : x(0), y(0), z(0) {};
        FlightSpeed(double x, double y, double z) : x(x), y(y), z(z) {};

        // Horizontal speed magnitude in m/s
        double horizontal() const { return std::sqrt(x * x + y * y); }

        // 3D speed magnitude in m/s
        double magnitude() const { return std::sqrt(x * x + y * y + z * z); }
    };
    inline std::ostream &operator<<(std::ostream &os, const FlightSpeed &s)
    {
        os << "X: " << s.x << " | Y: " << s.y << " | Z: " << s.z << " | Horizontal: " << s.horizontal() << " | 3D: " << s.magnitude();
        return os;
    }

    struct GpsDirection
    {
        double imgDirection;        // degrees (0-360), direction the image is facing
        std::string imgDirectionRef; // "T" = true north, "M" = magnetic north
        bool hasImgDirection;

        double track;               // degrees (0-360), direction of GPS movement
        std::string trackRef;       // "T" = true north, "M" = magnetic north
        bool hasTrack;

        GpsDirection() : imgDirection(0), imgDirectionRef("T"), hasImgDirection(false),
                                  track(0), trackRef("T"), hasTrack(false) {};

        bool hasData() const { return hasImgDirection || hasTrack; }
    };
    inline std::ostream &operator<<(std::ostream &os, const GpsDirection &d)
    {
        if (d.hasImgDirection)
            os << "ImgDirection: " << d.imgDirection << "° (" << d.imgDirectionRef << ")";
        if (d.hasImgDirection && d.hasTrack)
            os << " | ";
        if (d.hasTrack)
            os << "Track: " << d.track << "° (" << d.trackRef << ")";
        return os;
    }

    // Reads the EXIF and XMP tags of an image that the reader of the process (exif_metadata.h)
    // returned. The extractors answer from these tags only: where a tag is missing they give a
    // default, and where a tag has fewer components than the value needs they throw
    // std::out_of_range (a GPS latitude with two numbers).
    class ExifParser
    {
        // Keeps the metadata alive when the parser owns it; null when the caller keeps it
        std::shared_ptr<const exif::ImageMetadata> owned;
        const exif::ImageMetadata *metadata;

    public:
        // The metadata must outlive the parser
        explicit ExifParser(const exif::ImageMetadata &data) : metadata(&data) {}
        ExifParser(const exif::ImageMetadata &&) = delete;
        // The parser shares the ownership of the metadata
        explicit ExifParser(std::shared_ptr<const exif::ImageMetadata> data) : owned(std::move(data)), metadata(owned.get()) {}

        // The first tag found, in the order of the keys; nullptr when none exists (a key that is
        // not a valid XMP key is not found)
        const exif::Tag *findExifKey(const std::string &key);
        const exif::Tag *findExifKey(const std::initializer_list<std::string> &keys);
        const exif::Tag *findXmpKey(const std::string &key);
        const exif::Tag *findXmpKey(const std::initializer_list<std::string> &keys);

        ImageSize extractImageSize();
        ImageSize extractVideoSize();
        std::string extractMake();
        std::string extractModel();
        std::string extractSensor();
        bool computeFocal(Focal &f);
        bool extractSensorSize(SensorSize &r);
        double getMmPerUnit(long resolutionUnit);

        bool extractGeo(GeoLocation &geo);
        bool extractRelAltitude(double &relAltitude);
        inline double geoToDecimal(const exif::Tag *geoTag, const exif::Tag *geoRefTag);
        inline double evalFrac(const exif::Rational &rational);

        double parseSubSec(const exif::Tag *subsec);
        bool parseOffsetTime(const exif::Tag *offset, int &offsetSeconds);

        double extractCaptureTime();
        int extractImageOrientation();

        bool extractCameraOrientation(CameraOrientation &cameraOri);
        bool extractFlightSpeed(FlightSpeed &speed);
        bool extractGpsAccuracy(GpsAccuracy &accuracy);
        bool extractGpsDirection(GpsDirection &direction);
        bool extractPanoramaInfo(PanoramaInfo &info);

        bool hasExif();
        bool hasXmp();
        bool hasTags();

    private:
        // The steps of extractGeo and extractCaptureTime, in their order of priority
        bool extractExifGeo(GeoLocation &geo);
        bool extractDjiXmpGeo(GeoLocation &geo);
        std::optional<double> captureTimeFromXmpVideo();
        std::optional<double> captureTimeFromGpsStamp();
        std::optional<double> captureTimeFromOffsetTime();
        std::optional<double> captureTimeFromGeoTimezone();
    };

    namespace raster
    {
        struct ProbeOutcome; // raster_helper.h
    }

    /**
     * @brief Context for caching the parsed metadata of an image between fingerprint and parseEntry.
     *
     * fingerprint() and parseEntry() share the outcome of the metadata read, so the file is read
     * once: the ExifParser that owns the metadata, or the answer that the file has none. They
     * share what the raster helper found in the file the same way (probe): one request for the
     * file, whichever step asks first.
     */
    struct FingerprintContext
    {
        // Null when the file has no metadata the reader can give, or when it was not read
        std::unique_ptr<ExifParser> parser;
        GeoLocation geo;
        bool hasGeo = false;
        // The reader was asked for the metadata of the file, whatever it answered: parsing the
        // entry takes parser as it is instead of asking again
        bool metadataRead = false;
        // The read of the file that parseEntry started before hashing it
        // (EntryHandler::prefetch), so the reader works while the file is hashed: detection
        // takes its result instead of reading. Empty outside parseEntry.
        exif::PendingRead pendingRead;
        // What the raster helper found in the file (raster::probeFile): the detection of an
        // entry and its metadata step both need it. Null when the file was not probed in this
        // pass.
        std::shared_ptr<const raster::ProbeOutcome> probe;

        FingerprintContext() = default;

        // Non-copyable, moveable
        FingerprintContext(const FingerprintContext &) = delete;
        FingerprintContext &operator=(const FingerprintContext &) = delete;
        FingerprintContext(FingerprintContext &&) = default;
        FingerprintContext &operator=(FingerprintContext &&) = default;
    };

    // Reads the metadata of a file with the reader of the process and returns a parser that owns
    // it. nullptr, with a DEBUG log, when the file has no metadata the reader can give (it is not
    // an image, it cannot be opened, its metadata is damaged): callers treat that as an image
    // without EXIF. Throws what the reader throws when the reader itself cannot work
    // (MetadataHelperException, IndexException).
    std::unique_ptr<ExifParser> readExifParser(const fs::path &path);
    // The same, of the result of a read of `path` that was already started (it is taken)
    std::unique_ptr<ExifParser> readExifParser(const fs::path &path, exif::PendingRead &pending);

}

#endif // EXIF_H
