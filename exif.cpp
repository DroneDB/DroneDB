#include "exif.h"
#include "logger.h"

namespace exif {

Exiv2::ExifData::const_iterator Parser::findKey(const std::string &key) {
    return findKey({key});
}

// Find the first available key, or exifData::end() if none exist
Exiv2::ExifData::const_iterator Parser::findKey(const std::initializer_list<std::string>& keys) {
    for (auto &k : keys) {
        auto it = exifData.findKey(Exiv2::ExifKey(k));
        if (it != exifData.end()) return it;
    }
    return exifData.end();
}

ImageSize Parser::extractImageSize() {
    auto imgWidth = findKey("Exif.Photo.PixelXDimension");
    auto imgHeight = findKey("Exif.Photo.PixelYDimension");

    // TODO: fallback on actual image size

    if (imgWidth != exifData.end() && imgHeight != exifData.end()) {
        return ImageSize(static_cast<int>(imgWidth->toLong()), static_cast<int>(imgHeight->toLong()));
    } else {
        return ImageSize(-1, -1);
    }
}

std::string Parser::extractMake() {
    auto k = findKey({"Exif.Photo.LensMake", "Exif.Image.Make"});

    if (k != exifData.end()) {
        return k->toString();
    } else {
        return "unknown";
    }
}

std::string Parser::extractModel() {
    auto k = findKey({"Exif.Photo.LensModel", "Exif.Image.Model"});

    if (k != exifData.end()) {
        return k->toString();
    } else {
        return "unknown";
    }
}

std::string Parser::extractSensor() {
    std::string make = extractMake();
    std::string model = extractModel();
    utils::toLower(make);
    utils::toLower(model);

    if (make != "unknown") {
        size_t pos = std::string::npos;

        // Remove duplicate make string from model (if any)
        while ((pos = model.find(make) )!= std::string::npos) {
            model.erase(pos, make.length());
        }
    }

    utils::trim(make);
    utils::trim(model);

    return make + " " + model;
}

Focal Parser::computeFocal() {
    auto focal35 = findKey({"Exif.Photo.FocalLengthIn35mmFilm", "Exif.Image.FocalLengthIn35mmFilm"});
    auto focal = findKey({"Exif.Photo.FocalLength", "Exif.Image.FocalLength"});
    Focal res;

    if (focal35 != exifData.end() && focal35->toFloat() > 0) {
        res.f35 = focal35->toFloat();
        res.ratio = res.f35 / 36.0f;
    } else {
        float sensorWidth = extractSensorWidth();
        std::string sensor = extractSensor();
        if (sensorWidth == 0.0f && sensorData.count(sensor) > 0) {
            sensorWidth = sensorData.at(sensor);
        }

        if (sensorWidth > 0.0f && focal != exifData.end()) {
            res.ratio = focal->toFloat() / sensorWidth;
            res.f35 = 36.0f * res.ratio;
        } else {
            res.f35 = 0.0f;
            res.ratio = 0.0f;
        }
    }

    return res;
}

// Extracts sensor width. Returns 0 on failure
float Parser::extractSensorWidth() {
    auto fUnit = findKey({"Exif.Photo.FocalPlaneResolutionUnit", "Exif.Image.FocalPlaneResolutionUnit"});
    auto fXRes = findKey({"Exif.Photo.FocalPlaneXResolution", "Exif.Image.FocalPlaneXResolution"});

    if (fUnit == exifData.end() || fXRes == exifData.end()) return 0.0;

    long resolutionUnit = fUnit->toLong();
    float mmPerUnit = getMmPerUnit(resolutionUnit);
    if (mmPerUnit == 0.0f) return 0.0f;

    float pixelsPerUnit = fXRes->toFloat();
    float unitsPerPixel = 1.0f / pixelsPerUnit;
    float widthInPixels = extractImageSize().width;
    return widthInPixels * unitsPerPixel * mmPerUnit;
}

// Length of resolution unit in millimiters
// https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/EXIF.html
float Parser::getMmPerUnit(long resolutionUnit) {
    if (resolutionUnit == 2) {
        return 25.4f; // mm in 1 inch
    } else if (resolutionUnit == 3) {
        return 10.0f; //  mm in 1 cm
    } else {
        LOGE << "Unknown EXIF resolution unit: " << resolutionUnit;
        return 0.0;
    }
}


}
