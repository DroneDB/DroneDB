/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "geoproj.h"
#include "dbops.h"
#include "geoproject.h"


namespace cmd {

void GeoProj::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("geoproj output/ *.JPG")
    .add_options()
    ("o,output", "Output path (file or directory)", cxxopts::value<std::string>())
    ("i,images", "Images to project", cxxopts::value<std::vector<std::string>>())
    ("s,size", "Output image size (size[%]|0)", cxxopts::value<std::string>()->default_value("100%"));
    // clang-format on
    opts.parse_positional({"output", "images"});
}

std::string GeoProj::description() {
    return "Project images to georeferenced rasters";
}

void GeoProj::run(cxxopts::ParseResult &opts) {
    if (!opts.count("images") || !opts.count("output")) {
        printHelp();
    }

    auto images = opts["images"].as<std::vector<std::string>>();
    auto output = opts["output"].as<std::string>();
    auto outsize = opts["size"].as<std::string>();

    ddb::geoProject(images, output, outsize, false, [](const std::string &imageWritten){
        std::cout << "W\t" << imageWritten << std::endl;
        return true;
    });
}

}


