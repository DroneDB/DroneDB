/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exifeditor.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"
#include "exif_metadata.h"
#include <cmath>
#include <optional>

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

    namespace
    {
        // Why a file cannot be edited, nothing if it can: it exists and the metadata reader can
        // read it (a file with no metadata it can give, not an image or damaged, cannot be edited)
        std::optional<std::string> whyNotEditable(const fs::path &file)
        {
            if (!fs::exists(file))
                return "does not exist";
            if (fs::is_directory(file))
                return "cannot set EXIFs to a directory";

            try
            {
                const auto result = exif::metadataReader().read(file);
                if (!result.metadata)
                    return result.message.empty() ? "cannot read the metadata" : result.message;
            }
            catch (const IndexException &e)
            {
                // The reader broke on this file; a reader that cannot work at all propagates
                return std::string(e.what());
            }

            return std::nullopt;
        }
    } // namespace

    // Verify that all files can be edited
    bool ExifEditor::canEdit(std::ostream &rejected)
    {
        int rejectCount = 0;

        for (auto &file : files)
        {
            if (const auto reason = whyNotEditable(file))
            {
                rejected << io::toUtf8(file) << ": " << *reason << std::endl;
                rejectCount++;
            }
        }

        return rejectCount == 0;
    }

    std::vector<ExifWriteFailure> ExifEditor::SetGPSAltitude(double altitude)
    {
        // TODO: adjust XMP DJI tags
        // absolute/relative altitude
        return writeTags({{"Exif.GPSInfo.GPSAltitude", doubleToFraction(altitude, 4)},
                          {"Exif.GPSInfo.GPSAltitudeRef", altitude < 0.0 ? "1" : "0"}});
    }

    std::vector<ExifWriteFailure> ExifEditor::SetGPSLatitude(double latitude)
    {
        return writeTags({{"Exif.GPSInfo.GPSLatitude", doubleToDMS(latitude)},
                          {"Exif.GPSInfo.GPSLatitudeRef", latitude >= 0.0 ? "N" : "S"}});
    }

    std::vector<ExifWriteFailure> ExifEditor::SetGPSLongitude(double longitude)
    {
        return writeTags({{"Exif.GPSInfo.GPSLongitude", doubleToDMS(longitude)},
                          {"Exif.GPSInfo.GPSLongitudeRef", longitude >= 0.0 ? "E" : "W"}});
    }

    std::vector<ExifWriteFailure> ExifEditor::SetGPS(double latitude, double longitude, double altitude)
    {
        return writeTags({{"Exif.GPSInfo.GPSAltitude", doubleToFraction(altitude, 3)},
                          {"Exif.GPSInfo.GPSAltitudeRef", altitude < 0.0 ? "1" : "0"},
                          {"Exif.GPSInfo.GPSLatitude", doubleToDMS(latitude)},
                          {"Exif.GPSInfo.GPSLatitudeRef", latitude >= 0.0 ? "N" : "S"},
                          {"Exif.GPSInfo.GPSLongitude", doubleToDMS(longitude)},
                          {"Exif.GPSInfo.GPSLongitudeRef", longitude >= 0.0 ? "E" : "W"}});
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

    std::vector<ExifWriteFailure> ExifEditor::writeTags(const exif::TagAssignments &set)
    {
        std::vector<ExifWriteFailure> failures;

        for (auto &file : files)
        {
            if (!fs::exists(file))
                throw FSException("Cannot open " + io::toUtf8(file));

            for (const auto &[key, value] : set)
                LOGD << "Setting " << key << " to " << value << " for " << io::toUtf8(file);

            const auto result = exif::metadataReader().write(file, set);
            if (!result.ok)
                failures.push_back({io::toUtf8(file), result.message});
        }

        return failures;
    }

}
