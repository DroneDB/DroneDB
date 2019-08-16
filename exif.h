#ifndef EXIF_H
#define EXIF_H

#include <exiv2/exiv2.hpp>
#include "utils.h"

namespace exif{

struct ImageSize{
    int width;
    int height;
    ImageSize(int width, int height) : width(width), height(height){};
};

struct FocalInfo{
    float focal35;
    float focalRatio;
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
    std::string sensor();
    FocalInfo computeFocal();
    float extractSensorWidth();
    float getMmPerUnit(long resolutionUnit);
};



}

#endif // EXIF_H
