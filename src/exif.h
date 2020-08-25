/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_H
#define EXIF_H

#include <exiv2/exiv2.hpp>
#include <stdio.h>
#include "utils.h"
#include "sensor_data.h"
#include "ddb_export.h"

namespace ddb {

struct ImageSize {
    int width;
    int height;
    DDB_DLL ImageSize(int width, int height) : width(width), height(height) {};
};

struct Focal {
    double length; // in mm
    double length35; // in 35mm film equivalent
    DDB_DLL Focal() : length(0), length35(0) {};
    DDB_DLL Focal(double length, double length35) : length(length), length35(length35) {};
};

struct SensorSize {
    double width; // mm
    double height; // mm
    DDB_DLL SensorSize() : width(0), height(0) {};
    DDB_DLL SensorSize(double width, double height) : width(width), height(height) {};
};

struct GeoLocation {
    double latitude;
    double longitude;
    double altitude;

    DDB_DLL GeoLocation() : latitude(0), longitude(0), altitude(0) {}
    DDB_DLL GeoLocation(double latitude, double longitude, double altitude) : latitude(latitude), longitude(longitude), altitude(altitude) {};
};

struct CameraOrientation {
    double pitch; // degrees. -90 = nadir, 0 = front straight
    double yaw; // degress. 0 = magnetic north, 90 = east, -90 = west, 180 = south
    double roll; // degrees. 20 = left roll, -20 = right roll

    DDB_DLL CameraOrientation() : pitch(0), yaw(0), roll(0) {};
    DDB_DLL CameraOrientation(double pitch, double yaw, double roll) : pitch(pitch), yaw(yaw), roll(roll) {};
};
inline std::ostream& operator<<(std::ostream& os, const CameraOrientation& c)
{
    os << "Pitch: " << c.pitch << " | Yaw: " << c.yaw << " | Roll: " << c.roll;
    return os;
}



class ExifParser {
    Exiv2::ExifData exifData;
    Exiv2::XmpData xmpData;
  public:
    DDB_DLL ExifParser(const Exiv2::Image *image) : exifData(image->exifData()), xmpData(image->xmpData()) {};
    DDB_DLL ExifParser(const Exiv2::ExifData &exifData, const Exiv2::XmpData &xmpData) : exifData(exifData), xmpData(xmpData) {};

    DDB_DLL Exiv2::ExifData::const_iterator findExifKey(const std::string &key);
    DDB_DLL Exiv2::ExifData::const_iterator findExifKey(const std::initializer_list<std::string>& keys);
    DDB_DLL Exiv2::XmpData::const_iterator findXmpKey(const std::string &key);
    DDB_DLL Exiv2::XmpData::const_iterator findXmpKey(const std::initializer_list<std::string>& keys);

    DDB_DLL ImageSize extractImageSize();
    DDB_DLL std::string extractMake();
    DDB_DLL std::string extractModel();
    DDB_DLL std::string extractSensor();
    DDB_DLL bool computeFocal(Focal &f);
    DDB_DLL bool extractSensorSize(SensorSize &r);
    DDB_DLL inline double getMmPerUnit(long resolutionUnit);

    DDB_DLL  bool extractGeo(GeoLocation &geo);
    DDB_DLL bool extractRelAltitude(double &relAltitude);
    DDB_DLL inline double geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag);
    DDB_DLL inline double evalFrac(const Exiv2::Rational &rational);

    DDB_DLL double extractCaptureTime();
    DDB_DLL int extractImageOrientation();

    DDB_DLL bool extractCameraOrientation(CameraOrientation &cameraOri);

    DDB_DLL void printAllTags();

    DDB_DLL bool hasExif();
    DDB_DLL bool hasXmp();
};

}

#endif // EXIF_H
