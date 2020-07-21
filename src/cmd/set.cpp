/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "set.h"
#include "exifeditor.h"
#include "exceptions.h"

namespace cmd {

// TODO: should this be named "setexif" in the future?

void Set::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("set *.JPG")
    .add_options()
    ("i,input", "File(s) to modify", cxxopts::value<std::vector<std::string>>())
    ("gps-alt", "Set GPS Altitude (decimal degrees)", cxxopts::value<double>())
    ("gps-lon", "Set GPS Longitude (decimal degrees)", cxxopts::value<double>())
    ("gps-lat", "Set GPS Latitude (decimal degrees)", cxxopts::value<double>())
    ("gps", "Set GPS Latitude,Longitude,Altitude (decimal degrees, comma separated)", cxxopts::value<std::vector<double>>());

    opts.parse_positional({"input"});
}

std::string Set::description() {
    return "Modify EXIF values in files.";
}

void Set::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input")) {
        printHelp();
    }

    if (opts.count("gps")){
        auto gps = opts["gps"].as<std::vector<double>>();
        if (gps.size() != 3){
            printHelp();
        }
    }

    auto input = opts["input"].as<std::vector<std::string>>();
    ddb::ExifEditor exifEditor(input);

    if (!exifEditor.canEdit()){
        exit(EXIT_FAILURE);
    }

    if (opts.count("gps-alt")){
        exifEditor.SetGPSAltitude(opts["gps-alt"].as<double>());
    }

    if (opts.count("gps-lat")){
        exifEditor.SetGPSLatitude(opts["gps-lat"].as<double>());
    }

    if (opts.count("gps-lon")){
        exifEditor.SetGPSLongitude(opts["gps-lon"].as<double>());
    }

    if (opts.count("gps")){
        auto gps = opts["gps"].as<std::vector<double>>();
        exifEditor.SetGPS(gps[0], gps[1], gps[2]);
    }
}

}


