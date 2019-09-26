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
#include "exif.h"
#include "../logger.h"
#include "timezone.h"

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

// Extract "${make} ${model}" lowercase
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
inline float Parser::getMmPerUnit(long resolutionUnit) {
    if (resolutionUnit == 2) {
        return 25.4f; // mm in 1 inch
    } else if (resolutionUnit == 3) {
        return 10.0f; //  mm in 1 cm
    } else if (resolutionUnit == 4) {
        return 1.0f; // mm in 1 mm
    } else if (resolutionUnit == 5) {
        return 0.001f; // mm in 1 um
    } else {
        LOGE << "Unknown EXIF resolution unit: " << resolutionUnit;
        return 0.0;
    }
}

// Extract geolocation information
GeoLocation Parser::extractGeo() {
    GeoLocation r;

    auto latitude = findKey({"Exif.GPSInfo.GPSLatitude"});
    auto latitudeRef = findKey({"Exif.GPSInfo.GPSLatitudeRef"});
    auto longitude = findKey({"Exif.GPSInfo.GPSLongitude"});
    auto longitudeRef = findKey({"Exif.GPSInfo.GPSLongitudeRef"});

    r.latitude = geoToDecimal(latitude, latitudeRef);
    r.longitude = geoToDecimal(longitude, longitudeRef);

    auto altitude = findKey({"Exif.GPSInfo.GPSAltitude"});
    if (altitude != exifData.end()) {
        r.altitude = evalFrac(altitude->toRational());
    }

    return r;
}

// Converts a geotag location to decimal degrees
inline double Parser::geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag) {
    if (geoTag == exifData.end()) return 0.0;

    // N/S, W/E
    double sign = 1.0;
    if (geoRefTag != exifData.end()) {
        std::string ref = geoRefTag->toString();
        utils::toUpper(ref);
        if (ref == "S" || ref == "W") sign = -1.0;
    }

    double degrees = evalFrac(geoTag->toRational(0));
    double minutes = evalFrac(geoTag->toRational(1));
    double seconds = evalFrac(geoTag->toRational(2));

    return sign * (degrees + minutes / 60.0 + seconds / 3600.0);
}

// Evaluates a rational
double Parser::evalFrac(const Exiv2::Rational &rational) {
    if (rational.second == 0) return 0.0;
    return static_cast<double>(rational.first) / static_cast<double>(rational.second);
}

// Extracts timestamp (seconds from Jan 1st 1970)
time_t Parser::extractCaptureTime() {
    for (auto &k : {
                "Exif.Photo.DateTimeOriginal",
                "Exif.Photo.DateTimeDigitized",
                "Exif.Image.DateTime"
            }) {
        auto time = findKey(k);
        if (time == exifData.end()) continue;

        int year, month, day, hour, minute, second;

        if (sscanf(time->toString().c_str(),"%d:%d:%d %d:%d:%d", &year,&month,&day,&hour,&minute,&second) == 6) {
            // Attempt to use geolocation information to
            // find the proper timezone and adjust the timestamp
            GeoLocation geo = extractGeo();
            if (geo.latitude != 0.0 && geo.longitude != 0.0) {
                return Timezone::getUTCEpoch(year, month, day, hour, minute, second, geo.latitude, geo.longitude);
            }

            return 0;
        }
    }

    return 0;
}

int Parser::extractOrientation() {
    auto k = findKey({"Exif.Image.Orientation"});
    if (k != exifData.end()) {
        return static_cast<int>(k->toLong());
    }

    return 1;
}


}
