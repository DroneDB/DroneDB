#ifndef EXIF_H
#define EXIF_H

#include <exiv2/exiv2.hpp>
#include "utils.h"
#include "sensor_data.h"

namespace exif{

struct ImageSize{
    int width;
    int height;
    ImageSize(int width, int height) : width(width), height(height){};
};

struct Focal{
    float f35;
    float ratio;
    Focal() : f35(0), ratio(0) {};
};

struct GeoLocation{
    double latitude;
    double longitude;
    double altitude;

    GeoLocation() : latitude(0), longitude(0), altitude(0){}
};

class Parser{
    Exiv2::ExifData exifData;
public:
    Parser(const Exiv2::ExifData &exifData) : exifData(exifData) {};

    Exiv2::ExifData::const_iterator findKey(const std::string &key);
    Exiv2::ExifData::const_iterator findKey(const std::initializer_list<std::string>& keys);

    ImageSize extractImageSize();
    std::string extractMake();
    std::string extractModel();
    std::string extractSensor();
    Focal computeFocal();
    float extractSensorWidth();
    inline float getMmPerUnit(long resolutionUnit);

    GeoLocation extractGeo();
    inline double geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag);
    inline double evalFrac(const Exiv2::Rational &rational);
    // TODO: extract orientation
    //       geo
    //       capture time
};



}

#endif // EXIF_H
