/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif.h"
#include "logger.h"
#include "timezone.h"
#include "dsmservice.h"
#include "sensor_data.h"

namespace ddb {

Exiv2::ExifData::const_iterator ExifParser::findExifKey(const std::string &key) {
    return findExifKey({key});
}

// Find the first available key, or exifData::end() if none exist
Exiv2::ExifData::const_iterator ExifParser::findExifKey(const std::initializer_list<std::string>& keys) {
    for (auto &k : keys) {
        auto it = exifData.findKey(Exiv2::ExifKey(k));
        if (it != exifData.end()) return it;
    }
    return exifData.end();
}

Exiv2::XmpData::const_iterator ExifParser::findXmpKey(const std::string &key) {
    return findXmpKey({key});
}

// Find the first available key, or xmpData::end() if none exist
Exiv2::XmpData::const_iterator ExifParser::findXmpKey(const std::initializer_list<std::string>& keys) {
    for (auto &k : keys) {
        try{
            auto it = xmpData.findKey(Exiv2::XmpKey(k));
            if (it != xmpData.end()) return it;
        }catch(Exiv2::AnyError&){
            // Do nothing
        }
    }
    return xmpData.end();
}

ImageSize ExifParser::extractImageSize() {
    auto imgWidth = findExifKey({"Exif.Photo.PixelXDimension", "Exif.Image.ImageWidth"});
    auto imgHeight = findExifKey({"Exif.Photo.PixelYDimension", "Exif.Image.ImageLength"});

    // TODO: fallback on actual image size

    if (imgWidth != exifData.end() && imgHeight != exifData.end()) {
        return ImageSize(static_cast<int>(imgWidth->toLong()), static_cast<int>(imgHeight->toLong()));
    } else {
        return ImageSize(0, 0);
    }
}

std::string ExifParser::extractMake() {
    auto k = findExifKey({"Exif.Photo.LensMake", "Exif.Image.Make"});

    if (k != exifData.end()) {
        return k->toString();
    } else {
        return "unknown";
    }
}

std::string ExifParser::extractModel() {
    auto k = findExifKey({"Exif.Image.Model", "Exif.Photo.LensModel"});

    if (k != exifData.end()) {
        return k->toString();
    } else {
        return "unknown";
    }
}

// Extract "${make} ${model}" lowercase
std::string ExifParser::extractSensor() {
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

bool ExifParser::computeFocal(Focal &f) {
    SensorSize r;

    if (extractSensorSize(r)) {
        double sensorWidth = r.width;
        auto focal35 = findExifKey("Exif.Photo.FocalLengthIn35mmFilm");
        auto focal = findExifKey("Exif.Photo.FocalLength");

        if (focal35 != exifData.end() && focal35->toFloat() > 0) {
            f.length35 = static_cast<double>(focal35->toFloat());
            f.length = (f.length35 / 36.0) * sensorWidth;
        } else if (focal != exifData.end() && focal->toFloat() > 0) {
            f.length = static_cast<double>(focal->toFloat());
            f.length35 = (36.0 * f.length) / sensorWidth;
        }

        return true;
    }

    return false;
}

// Extracts sensor sizes (in mm). Returns 0 on failure
bool ExifParser::extractSensorSize(SensorSize &r) {
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

            return true; // Good, exit here
        }
    }

    // If we reached this point, we fallback to database lookup
    std::string sensor = extractSensor();
    if (SensorData::contains(sensor)) {
        r.width = SensorData::getFocal(sensor);

        // TODO: is this the best way?
        auto imsize = extractImageSize();
        r.height = (r.width / imsize.width) * imsize.height;
        return true;
    }

    return false;
}

// Length of resolution unit in millimiters
// https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/EXIF.html
inline double ExifParser::getMmPerUnit(long resolutionUnit) {
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
bool ExifParser::extractGeo(GeoLocation &geo) {
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

        auto altitudeRef = findExifKey({"Exif.GPSInfo.GPSAltitudeRef"});
        if (altitudeRef != exifData.end()) {
            geo.altitude *= altitudeRef->toLong() == 1 ? -1 : 1;
        }
    }

    auto xmpAltitude = findXmpKey({"Xmp.drone-dji.AbsoluteAltitude"});
    if (xmpAltitude != xmpData.end()) {
        geo.altitude = evalFrac(xmpAltitude->toRational());
    }

    return true;
}

bool ExifParser::extractRelAltitude(double &relAltitude) {
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
inline double ExifParser::geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag) {
    if (geoTag == exifData.end()) return 0.0;

    // N/S, W/E
    double sign = 1.0;
    if (geoRefTag != exifData.end()) {
        std::string ref = geoRefTag->toString();
        utils::toUpper(ref);
        if (ref == "S" || ref == "W") sign = -1.0;
    }

//    LOGD << geoTag->toRational(0).first << "/" << geoTag->toRational(0).second << " "
//         << geoTag->toRational(1).first << "/" << geoTag->toRational(1).second << " "
//         << geoTag->toRational(2).first << "/" << geoTag->toRational(2).second;

    double degrees = evalFrac(geoTag->toRational(0));
    double minutes = evalFrac(geoTag->toRational(1));
    double seconds = evalFrac(geoTag->toRational(2));

    return sign * (degrees + minutes / 60.0 + seconds / 3600.0);
}

// Evaluates a rational
double ExifParser::evalFrac(const Exiv2::Rational &rational) {
    if (rational.second == 0) return 0.0;
    return static_cast<double>(rational.first) / static_cast<double>(rational.second);
}

// Extracts timestamp (seconds from Jan 1st 1970)
double ExifParser::extractCaptureTime() {
    auto time = findExifKey({"Exif.Photo.DateTimeOriginal",
                             "Exif.Photo.DateTimeDigitized",
                             "Exif.Image.DateTime"});
    if (time == exifData.end()) return 0.0;

    int year, month, day, hour, minute, second;

    if (sscanf(time->toString().c_str(),"%d:%d:%d %d:%d:%d", &year,&month,&day,&hour,&minute,&second) == 6) {
        double msecs = 0.0;
        auto subsec = findExifKey({"Exif.Photo.SubSecTimeOriginal",
                                 "Exif.Photo.SubSecTimeDigitized",
                                 "Exif.Photo.SubSecTime"});
        if (subsec != exifData.end()){
            double ss = static_cast<double>(subsec->toLong());
            size_t numDigits = subsec->toString().length();

            // ."1" --> "100"
            // ."12" --> "120"
            // ."12345" --> "123.45"
            if (numDigits == 0) msecs = 0.0;
            else if (numDigits == 1) msecs = ss * 100.0;
            else if (numDigits == 2) msecs = ss * 10.0;
            else if (numDigits == 3) msecs = ss;
            else msecs = ss / static_cast<double>(pow(10, numDigits - 3));
        }

        cctz::time_zone tz = cctz::utc_time_zone();

        // TODO: use OffsetTimeOriginal | OffsetTimeDigitized | OffsetTime
        // if they are available
        // https://github.com/mapillary/OpenSfM/blob/master/opensfm/exif.py#L373

        // Attempt to use geolocation information to
        // find the proper timezone and adjust the timestamp
        GeoLocation geo;
        if (extractGeo(geo)) {
            tz = Timezone::lookupTimezone(geo.latitude, geo.longitude);
        }

        return Timezone::getUTCEpoch(year, month, day, hour, minute, second, msecs, tz);
    }else{
        LOGD << "Invalid date/time format: " << time->toString();
        return 0.0;
    }
}

int ExifParser::extractImageOrientation() {
    auto k = findExifKey({"Exif.Image.Orientation"});
    if (k != exifData.end()) {
        return static_cast<int>(k->toLong());
    }

    return 1;
}

bool ExifParser::extractCameraOrientation(CameraOrientation &cameraOri) {
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
        cameraOri.roll *= -1;
    }

    return true;
}

void ExifParser::printAllTags() {
    Exiv2::ExifData::const_iterator end = exifData.end();
    for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
        const char* tn = i->typeName();
        std::cout << i->key() << " "
                  << i->value()
                  << " | " << tn
                  << std::endl;
    }
}

bool ExifParser::hasExif() {
    return !exifData.empty();
}

bool ExifParser::hasXmp() {
    return !xmpData.empty();
}

}
