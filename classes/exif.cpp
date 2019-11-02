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
#include "dsmservice.h"

namespace exif {

// Register XMP namespaces
void Initialize(){
    Exiv2::XmpProperties::registerNs("http://www.dji.com/drone-dji/1.0/", "drone-dji");
}

Exiv2::ExifData::const_iterator Parser::findExifKey(const std::string &key) {
    return findExifKey({key});
}

// Find the first available key, or exifData::end() if none exist
Exiv2::ExifData::const_iterator Parser::findExifKey(const std::initializer_list<std::string>& keys) {
    for (auto &k : keys) {
        auto it = exifData.findKey(Exiv2::ExifKey(k));
        if (it != exifData.end()) return it;
    }
    return exifData.end();
}

Exiv2::XmpData::const_iterator Parser::findXmpKey(const std::string &key) {
    return findXmpKey({key});
}

// Find the first available key, or xmpData::end() if none exist
Exiv2::XmpData::const_iterator Parser::findXmpKey(const std::initializer_list<std::string>& keys) {
    for (auto &k : keys) {
        auto it = xmpData.findKey(Exiv2::XmpKey(k));
        if (it != xmpData.end()) return it;
    }
    return xmpData.end();
}

ImageSize Parser::extractImageSize() {
    auto imgWidth = findExifKey({"Exif.Photo.PixelXDimension", "Exif.Image.ImageWidth"});
    auto imgHeight = findExifKey({"Exif.Photo.PixelYDimension", "Exif.Image.ImageLength"});

    // TODO: fallback on actual image size

    if (imgWidth != exifData.end() && imgHeight != exifData.end()) {
        return ImageSize(static_cast<int>(imgWidth->toLong()), static_cast<int>(imgHeight->toLong()));
    } else {
        return ImageSize(0, 0);
    }
}

std::string Parser::extractMake() {
    auto k = findExifKey({"Exif.Photo.LensMake", "Exif.Image.Make"});

    if (k != exifData.end()) {
        return k->toString();
    } else {
        return "unknown";
    }
}

std::string Parser::extractModel() {
    auto k = findExifKey({"Exif.Image.Model", "Exif.Photo.LensModel"});

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
    Focal f;
    double sensorWidth = extractSensorSize().width;

    if (sensorWidth > 0.0) {
        auto focal35 = findExifKey("Exif.Photo.FocalLengthIn35mmFilm");
        auto focal = findExifKey("Exif.Photo.FocalLength");

        if (focal35 != exifData.end() && focal35->toFloat() > 0) {
            f.length35 = static_cast<double>(focal35->toFloat());
            f.length = (f.length35 / 36.0) * sensorWidth;
        } else if (focal != exifData.end() && focal->toFloat() > 0) {
            f.length = static_cast<double>(focal->toFloat());
            f.length35 = (36.0 * f.length) / sensorWidth;
        }
    }

    return f;
}

// Extracts sensor sizes (in mm). Returns 0 on failure
SensorSize Parser::extractSensorSize() {
    SensorSize r;

    auto fUnit = findExifKey("Exif.Photo.FocalPlaneResolutionUnit");
    auto fXRes = findExifKey("Exif.Photo.FocalPlaneXResolution");
    auto fYRes = findExifKey("Exif.Photo.FocalPlaneYResolution");

    if (fUnit != exifData.end() && fXRes != exifData.end() && fYRes != exifData.end()) {
        long resolutionUnit = fUnit->toLong();
        double mmPerUnit = getMmPerUnit(resolutionUnit);
        if (mmPerUnit != 0.0) {
            auto imsize = extractImageSize();

            double xUnitsPerPixel = 1.0 / static_cast<double>(fXRes->toFloat());
            r.width = imsize.width * xUnitsPerPixel * mmPerUnit;

            double yUnitsPerPixel = 1.0 / static_cast<double>(fYRes->toFloat());
            r.height = imsize.height * yUnitsPerPixel * mmPerUnit;

            return r; // Good, exit here
        }
    }

    // If we reached this point, we fallback to database lookup
    std::string sensor = extractSensor();
    if (sensorData.count(sensor) > 0) {
        r.width = sensorData.at(sensor);

        // This is an (inaccurate) estimate
        // TODO: is this the best way?
        auto imsize = extractImageSize();
        r.height = (r.width / imsize.width) * imsize.height;
    }

    return r;
}

// Length of resolution unit in millimiters
// https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/EXIF.html
inline double Parser::getMmPerUnit(long resolutionUnit) {
    if (resolutionUnit == 2) {
        return 25.4; // mm in 1 inch
    } else if (resolutionUnit == 3) {
        return 10.0; //  mm in 1 cm
    } else if (resolutionUnit == 4) {
        return 1.0; // mm in 1 mm
    } else if (resolutionUnit == 5) {
        return 0.001; // mm in 1 um
    } else {
        LOGE << "Unknown EXIF resolution unit: " << resolutionUnit;
        return 0.0;
    }
}

// Extract geolocation information
bool Parser::extractGeo(GeoLocation &geo) {
    auto latitude = findExifKey({"Exif.GPSInfo.GPSLatitude"});
    auto latitudeRef = findExifKey({"Exif.GPSInfo.GPSLatitudeRef"});
    auto longitude = findExifKey({"Exif.GPSInfo.GPSLongitude"});
    auto longitudeRef = findExifKey({"Exif.GPSInfo.GPSLongitudeRef"});

    if (latitude == exifData.end() || longitude == exifData.end()) return false;

    geo.latitude = geoToDecimal(latitude, latitudeRef);
    geo.longitude = geoToDecimal(longitude, longitudeRef);

    auto altitude = findExifKey({"Exif.GPSInfo.GPSAltitude"});
    if (altitude != exifData.end()) {
        geo.altitude = evalFrac(altitude->toRational());
    }

    auto xmpAltitude = findXmpKey({"Xmp.drone-dji.AbsoluteAltitude"});
    if (xmpAltitude != xmpData.end()) {
        geo.altitude = evalFrac(xmpAltitude->toRational());
    }

    return true;
}

bool Parser::extractRelAltitude(double &relAltitude) {
    // Some drones have a value for relative altitude
    auto k = findXmpKey("Xmp.drone-dji.RelativeAltitude");
    if (k != xmpData.end()){
        relAltitude = static_cast<double>(k->toFloat());
        return true;
    }

    // For others, we lookup an estimate from a world DSM source
    GeoLocation geo;
    if (extractGeo(geo) && geo.altitude > 0){
        relAltitude = geo.altitude - static_cast<double>(DSMService::get()->getAltitude(geo.latitude, geo.longitude));
        return true;
    }

    relAltitude = 0.0;
    return false; // Not available
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
        auto time = findExifKey(k);
        if (time == exifData.end()) continue;

        int year, month, day, hour, minute, second;

        if (sscanf(time->toString().c_str(),"%d:%d:%d %d:%d:%d", &year,&month,&day,&hour,&minute,&second) == 6) {
            // Attempt to use geolocation information to
            // find the proper timezone and adjust the timestamp
            GeoLocation geo;
            if (extractGeo(geo)) {
                return Timezone::getUTCEpoch(year, month, day, hour, minute, second, geo.latitude, geo.longitude);
            }

            return 0;
        }
    }

    return 0;
}

int Parser::extractImageOrientation() {
    auto k = findExifKey({"Exif.Image.Orientation"});
    if (k != exifData.end()) {
        return static_cast<int>(k->toLong());
    }

    return 1;
}

bool Parser::extractCameraOrientation(CameraOrientation &cameraOri) {
    auto pk = findXmpKey({"Xmp.drone-dji.GimbalPitchDegree", "Xmp.Camera.Pitch"});
    auto yk = findXmpKey({"Xmp.drone-dji.GimbalYawDegree", "Xmp.Camera.Yaw"});
    auto rk = findXmpKey({"Xmp.drone-dji.GimbalRollDegree", "Xmp.Camera.Roll"});

    if (pk == xmpData.end() || yk == xmpData.end() || rk == xmpData.end()) return false;

    cameraOri.pitch = static_cast<double>(pk->toFloat());
    cameraOri.yaw = static_cast<double>(yk->toFloat());
    cameraOri.roll = static_cast<double>(rk->toFloat());

    // TODO: JSON orientation database
    if (extractMake() == "senseFly"){
        cameraOri.pitch += -90;
    }

    return true;
}

void Parser::printAllTags() {
    Exiv2::ExifData::const_iterator end = exifData.end();
    for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
        const char* tn = i->typeName();
        std::cout << i->key() << " "
                  << i->value()
                  << " | " << tn
                  << std::endl;
    }
}

bool Parser::hasExif() {
    return !exifData.empty();
}

bool Parser::hasXmp() {
    return !xmpData.empty();
}


}
