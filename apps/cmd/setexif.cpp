/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/setexif.h"
#include "exifeditor.h"
#include "exceptions.h"

namespace cmd
{

    void SetExif::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("setexif *.JPG")
    .add_options()
    ("i,input", "File(s) to modify", cxxopts::value<std::vector<std::string>>())
    ("gps-alt", "Set GPS Altitude (decimal degrees)", cxxopts::value<double>())
    ("gps-lon", "Set GPS Longitude (decimal degrees)", cxxopts::value<double>())
    ("gps-lat", "Set GPS Latitude (decimal degrees)", cxxopts::value<double>())
    ("gps", "Set GPS Latitude,Longitude,Altitude (decimal degrees, comma separated)", cxxopts::value<std::vector<double>>());
        // clang-format on
        opts.parse_positional({"input"});
    }

    std::string SetExif::description()
    {
        return "Modify EXIF values in files.";
    }

    int SetExif::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            usageError();
        }

        if (opts.count("gps"))
        {
            auto gps = opts["gps"].as<std::vector<double>>();
            if (gps.size() != 3)
            {
                usageError();
            }
        }

        auto input = opts["input"].as<std::vector<std::string>>();
        ddb::ExifEditor exifEditor(input);

        if (!exifEditor.canEdit(std::cerr))
        {
            return EXIT_FAILURE;
        }

        // A file that cannot be written is reported and the next one is edited
        const auto report = [](const std::vector<ddb::ExifWriteFailure> &failures)
        {
            for (const auto &failure : failures)
                std::cerr << "Cannot write metadata to " << failure.file << ": " << failure.message << std::endl;
        };

        if (opts.count("gps-alt"))
        {
            report(exifEditor.SetGPSAltitude(opts["gps-alt"].as<double>()));
        }

        if (opts.count("gps-lat"))
        {
            report(exifEditor.SetGPSLatitude(opts["gps-lat"].as<double>()));
        }

        if (opts.count("gps-lon"))
        {
            report(exifEditor.SetGPSLongitude(opts["gps-lon"].as<double>()));
        }

        if (opts.count("gps"))
        {
            auto gps = opts["gps"].as<std::vector<double>>();
            report(exifEditor.SetGPS(gps[0], gps[1], gps[2]));
        }
        return EXIT_SUCCESS;
    }

}
