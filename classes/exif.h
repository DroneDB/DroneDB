/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#ifndef EXIF_H
#define EXIF_H

#include <exiv2/exiv2.hpp>
#include <stdio.h>
#include "../utils.h"
#include "../sensor_data.h"

namespace exif {

struct ImageSize {
    int width;
    int height;
    ImageSize(int width, int height) : width(width), height(height) {};
};

struct Focal {
    double length; // in mm
    double length35; // in 35mm film equivalent
    Focal() : length(0), length35(0) {};
    Focal(double length, double length35) : length(length), length35(length35) {};
};

struct SensorSize {
    double width; // mm
    double height; // mm
    SensorSize() : width(0), height(0) {};
    SensorSize(double width, double height) : width(width), height(height) {};
};

struct GeoLocation {
    double latitude;
    double longitude;
    double altitude;

    GeoLocation() : latitude(0), longitude(0), altitude(0) {}
    GeoLocation(double latitude, double longitude, double altitude) : latitude(latitude), longitude(longitude), altitude(altitude) {};
};

struct CameraOrientation {
    double pitch; // degrees. -90 = nadir, 0 = front straight
    double yaw; // degress. 0 = magnetic north, 90 = east, -90 = west, 180 = south
    double roll; // degrees. 20 = left roll, -20 = right roll

    CameraOrientation() : pitch(0), yaw(0), roll(0) {};
    CameraOrientation(double pitch, double yaw, double roll) : pitch(pitch), yaw(yaw), roll(roll) {};
};
inline std::ostream& operator<<(std::ostream& os, const CameraOrientation& c)
{
    os << "Pitch: " << c.pitch << " | Yaw: " << c.yaw << " | Roll: " << c.roll;
    return os;
}

void Initialize();

class Parser {
    Exiv2::ExifData exifData;
    Exiv2::XmpData xmpData;
  public:
    Parser(const Exiv2::Image *image) : exifData(image->exifData()), xmpData(image->xmpData()) {};
    Parser(const Exiv2::ExifData &exifData, const Exiv2::XmpData &xmpData) : exifData(exifData), xmpData(xmpData) {};

    Exiv2::ExifData::const_iterator findExifKey(const std::string &key);
    Exiv2::ExifData::const_iterator findExifKey(const std::initializer_list<std::string>& keys);
    Exiv2::XmpData::const_iterator findXmpKey(const std::string &key);
    Exiv2::XmpData::const_iterator findXmpKey(const std::initializer_list<std::string>& keys);

    ImageSize extractImageSize();
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

    time_t extractCaptureTime();
    int extractImageOrientation();

    bool extractCameraOrientation(CameraOrientation &cameraOri);

    void printAllTags();

    bool hasExif();
    bool hasXmp();
};



}

#endif // EXIF_H
