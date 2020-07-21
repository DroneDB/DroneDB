/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exifeditor.h"
#include "logger.h"
#include "exceptions.h"
#include <exiv2/exiv2.hpp>
#include <cmath>

namespace ddb{

ExifEditor::ExifEditor(std::vector<std::string> &files){
    this->files = std::vector<fs::path>(files.begin(), files.end());
}

ExifEditor::ExifEditor(const std::string &file){
    files.push_back(file);
}

// Verify that all files can be edited
bool ExifEditor::canEdit(){
    int rejectCount = 0;

    for (auto &file : files){
        try{
            if (!fs::exists(file)) throw FSException("does not exist");
            if (fs::is_directory(file)) throw FSException("cannot set EXIFs to a directory");
            auto image = Exiv2::ImageFactory::open(file.string(), false);
            if (!image.get()) throw FSException("cannot open " + file.string());
            image->readMetadata();
        }catch(const FSException &e){
            std::cerr << file.string() << ": " << e.what() << std::endl;
            rejectCount++;
        }catch(const Exiv2::AnyError &e){
            std::cerr << file.string() << ": " << e.what() << std::endl;
            rejectCount++;
        }

        // All good
    }

    return rejectCount == 0;
}

void ExifEditor::SetGPSAltitude(double altitude){
    eachFile([=](const fs::path &f, Exiv2::ExifData &exifData){
        exifData["Exif.GPSInfo.GPSAltitude"] = doubleToFraction(altitude, 4);
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = altitude < 0.0 ? "1" : "0";
        LOGD << "Setting altitude to " << exifData["Exif.GPSInfo.GPSAltitude"].toString() << " (" << exifData["Exif.GPSInfo.GPSAltitudeRef"].toString() << ") for " << f.string();

        // TODO: adjust XMP DJI tags
        // absolute/relative altitude
    });
}

void ExifEditor::SetGPSLatitude(double latitude){
    eachFile([=](const fs::path &f, Exiv2::ExifData &exifData){
        exifData["Exif.GPSInfo.GPSLatitude"] = doubleToDMS(latitude);
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = latitude >= 0.0 ? "N" : "S";

        LOGD << "Setting latitude to " << doubleToDMS(latitude) << " " <<
                exifData["Exif.GPSInfo.GPSLatitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLatitudeRef"].toString() << " " <<
                "for " << f.string();
    });
}

void ExifEditor::SetGPSLongitude(double longitude){
    eachFile([=](const fs::path &f, Exiv2::ExifData &exifData){
        exifData["Exif.GPSInfo.GPSLongitude"] = doubleToDMS(longitude);
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = longitude >= 0.0 ? "E" : "W";
        LOGD << "Setting longitude to " <<
                exifData["Exif.GPSInfo.GPSLongitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLongitudeRef"].toString() << " " <<
                "for " << f.string();
    });
}

void ExifEditor::SetGPS(double latitude, double longitude, double altitude){
    eachFile([=](const fs::path &f, Exiv2::ExifData &exifData){
        exifData["Exif.GPSInfo.GPSAltitude"] = doubleToFraction(altitude, 3);
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = altitude < 0.0 ? "1" : "0";
        exifData["Exif.GPSInfo.GPSLatitude"] = doubleToDMS(latitude);
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = latitude >= 0.0 ? "N" : "S";
        exifData["Exif.GPSInfo.GPSLongitude"] = doubleToDMS(longitude);
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = longitude >= 0.0 ? "E" : "W";

        LOGD << "Setting lat: " <<
                exifData["Exif.GPSInfo.GPSLatitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLatitudeRef"].toString() << " " <<
                "lon: " <<
                exifData["Exif.GPSInfo.GPSLongitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLongitudeRef"].toString() << " " <<
                "alt: " <<
                 exifData["Exif.GPSInfo.GPSAltitude"].toString() << " (" << exifData["Exif.GPSInfo.GPSAltitudeRef"].toString() << ") " <<
                "for " << f.string();
    });
}

// Convert a double into a DMS string
const std::string ExifEditor::doubleToDMS(double d){
    if ( d < 0.0 ) d = -d; // No negative numbers allowed
    int deg = (int)d;
    d -= deg;
    d *= 60;
    int min = (int)d ;
    d  -= min;
    d  *= 60;
    int sec = (int)round(d * 10000.0);

    return std::to_string(deg) + "/1 " +
           std::to_string(min) + "/1 " +
            std::to_string(sec) + "/10000";
}

// Convert a double into a fraction suitable for EXIF
const std::string ExifEditor::doubleToFraction(double d, int precision){
    if ( d < 0.0 ) d = -d; // No negative numbers allowed
    d *= pow(10.0, precision);

    std::string res = std::to_string((int)d) + "/1";
    for (int i = 0; i < precision; i++) res += "0";
    return res;
}

template<typename Func>
void ExifEditor::eachFile(Func f){
    for (auto &file : files){
        auto image = Exiv2::ImageFactory::open(file.string(), false);
        if (!image.get()) throw FSException("Cannot open " + file.string());
        image->readMetadata();

        f(file, image->exifData());

        image->setExifData(image->exifData());
        try{
            image->writeMetadata();
            //std::cout << "U\t" << file.string() << std::endl;
        }catch(const Exiv2::AnyError &){
            std::cerr << "Cannot write metadata to " + file.string();
        }
    }
}

}
