/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exifeditor.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include "exif.h"
#include <iostream>
#include <exiv2/exiv2.hpp>
#include <cmath>

namespace ddb
{

    ExifEditor::ExifEditor(std::vector<std::string> &files)
    {
        this->files = io::fromUtf8(files);
    }

    ExifEditor::ExifEditor(const std::string &file)
    {
        files.push_back(io::fromUtf8(file));
    }

    // Verify that all files can be edited
    bool ExifEditor::canEdit()
    {
        int rejectCount = 0;

        for (auto &file : files)
        {
            try
            {
                if (!fs::exists(file))
                    throw FSException("does not exist");
                if (fs::is_directory(file))
                    throw FSException("cannot set EXIFs to a directory");
                auto image = openExivImage(file);
                if (!image.get())
                    throw FSException("cannot open " + io::toUtf8(file));
                image->readMetadata();
            }
            catch (const FSException &e)
            {
                std::cerr << io::toUtf8(file) << ": " << e.what() << std::endl;
                rejectCount++;
            }
            catch (const Exiv2::Error &e)
            {
                std::cerr << io::toUtf8(file) << ": " << e.what() << std::endl;
                rejectCount++;
            }

            // All good
        }

        return rejectCount == 0;
    }

    void ExifEditor::SetGPSAltitude(double altitude)
    {
        eachFile([this, altitude](const fs::path &f, Exiv2::ExifData &exifData)
                 {
                     exifData["Exif.GPSInfo.GPSAltitude"] = doubleToFraction(altitude, 4);
                     exifData["Exif.GPSInfo.GPSAltitudeRef"] = altitude < 0.0 ? "1" : "0";
                     LOGD << "Setting altitude to " << exifData["Exif.GPSInfo.GPSAltitude"].toString() << " (" << exifData["Exif.GPSInfo.GPSAltitudeRef"].toString() << ") for " << io::toUtf8(f);

                     // TODO: adjust XMP DJI tags
                     // absolute/relative altitude
                 });
    }

    void ExifEditor::SetGPSLatitude(double latitude)
    {
        eachFile([this, latitude](const fs::path &f, Exiv2::ExifData &exifData)
                 {
        exifData["Exif.GPSInfo.GPSLatitude"] = doubleToDMS(latitude);
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = latitude >= 0.0 ? "N" : "S";

        LOGD << "Setting latitude to " << doubleToDMS(latitude) << " " <<
                exifData["Exif.GPSInfo.GPSLatitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLatitudeRef"].toString() << " " <<
                "for " << io::toUtf8(f); });
    }

    void ExifEditor::SetGPSLongitude(double longitude)
    {
        eachFile([this, longitude](const fs::path &f, Exiv2::ExifData &exifData)
                 {
        exifData["Exif.GPSInfo.GPSLongitude"] = doubleToDMS(longitude);
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = longitude >= 0.0 ? "E" : "W";
        LOGD << "Setting longitude to " <<
                exifData["Exif.GPSInfo.GPSLongitude"].toString() << " " <<
                exifData["Exif.GPSInfo.GPSLongitudeRef"].toString() << " " <<
                "for " << io::toUtf8(f); });
    }

    void ExifEditor::SetGPS(double latitude, double longitude, double altitude)
    {
        eachFile([this, latitude, longitude, altitude](const fs::path &f, Exiv2::ExifData &exifData)
                 {
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
                "for " << io::toUtf8(f); });
    }

    // Convert a double into a DMS string
    const std::string ExifEditor::doubleToDMS(double d)
    {
        if (d < 0.0)
            d = -d; // No negative numbers allowed
        int deg = static_cast<int>(d);
        d -= deg;
        d *= 60;
        int min = static_cast<int>(d);
        d -= min;
        d *= 60;
        int sec = static_cast<int>(round(d * 10000.0));

        return std::to_string(deg) + "/1 " +
               std::to_string(min) + "/1 " +
               std::to_string(sec) + "/10000";
    }

    // Convert a double into a fraction suitable for EXIF
    const std::string ExifEditor::doubleToFraction(double d, int precision)
    {
        if (d < 0.0)
            d = -d; // No negative numbers allowed
        d *= pow(10.0, precision);

        std::string res = std::to_string(static_cast<int>(d)) + "/1";
        for (int i = 0; i < precision; i++)
            res += "0";
        return res;
    }

    template <typename Func>
    void ExifEditor::eachFile(Func f)
    {
        for (auto &file : files)
        {
            auto image = openExivImage(file);
            if (!image.get())
                throw FSException("Cannot open " + io::toUtf8(file));
            image->readMetadata();

            f(file, image->exifData());

            image->setExifData(image->exifData());
            try
            {
                writeExivMetadata(*image, file);
            }
            catch (const Exiv2::Error &)
            {
                std::cerr << "Cannot write metadata to " + io::toUtf8(file);
            }
        }
    }

}
