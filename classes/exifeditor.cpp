#include "exifeditor.h"
#include "../logger.h"
#include "../classes/exceptions.h"
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
// this includes only GeoImage and Image type files
bool ExifEditor::canEdit(){
    int rejectCount = 0;

    for (auto &file : files){
        auto image = Exiv2::ImageFactory::open(file);
        if (!image.get()) throw new FSException("Cannot open " + file.string());
        try{
            image->readMetadata();
        }catch(const Exiv2::AnyError &){
            std::cerr << "Cannot read EXIFs: " + file.string();
            rejectCount++;
        }

        // All good
    }

    return rejectCount == 0;
}

void ExifEditor::SetGPSAltitude(double altitude){
    eachFile([=](const fs::path &f, Exiv2::ExifData &exifData){
        exifData["Exif.GPSInfo.GPSAltitude"] = doubleToFraction(altitude, 2);
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = altitude < 0.0 ? "1" : "0";
        LOGD << "Setting altitude to " << doubleToDMS(altitude) << " for " << f.string();

        // TODO: adjust XMP DJI tags
        // absolute/relative altitude
    });
}

void ExifEditor::SetGPSLatitude(double latitude){

}

void ExifEditor::SetGPSLongitude(double longitude){

}

void ExifEditor::SetGPS(double latitude, double longitude, double altitude){

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
    int sec = (int)d;

    return std::to_string(deg) + "/1 " +
           std::to_string(min) + "/1 " +
            std::to_string(sec) + "/1";
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
        auto image = Exiv2::ImageFactory::open(file);
        if (!image.get()) throw new FSException("Cannot open " + file.string());
        image->readMetadata();

        f(file, image->exifData());

        image->setExifData(image->exifData());
        try{
            image->writeMetadata();
            std::cout << "U\t" << file.string() << std::endl;
        }catch(const Exiv2::AnyError &){
            std::cerr << "Cannot write metadata to " + file.string();
        }
    }
}

}
