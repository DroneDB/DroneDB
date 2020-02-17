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

#include <iostream>
#include "geoproj.h"
#include "../libs/ddb.h"
#include "../libs/geoproject.h"


namespace cmd {

void GeoProj::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("geoproj output/ *.JPG")
    .add_options()
    ("o,output", "Output path (file or directory)", cxxopts::value<std::string>())
    ("i,images", "Images to project", cxxopts::value<std::vector<std::string>>())
    ("s,size", "Output image size (size[%]|0)", cxxopts::value<std::string>());

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

    ddb::geoProject(images, output, outsize);
}

}


