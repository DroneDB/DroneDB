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
#include "thumbs.h"
#include "../libs/thumbs.h"
#include "../classes/exceptions.h"

namespace cmd {

void Thumbs::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("thumbs output/ *.JPG")
    .add_options()
    ("i,input", "Files or directories to generate thumbnails of", cxxopts::value<std::vector<std::string>>())
    ("o,output", "Output path where to store thumbnails (file or directory)", cxxopts::value<std::string>())
    ("s,size", "Size of the largest side of the images", cxxopts::value<int>()->default_value("512"))
    ("r,recursive", "Recursively process subdirectories", cxxopts::value<bool>())
    ("d,depth", "Max recursion depth", cxxopts::value<int>()->default_value("0"));

    opts.parse_positional({"output", "input"});
}

std::string Thumbs::description() {
    return "Generate thumbnails for images and rasters";
}

void Thumbs::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input") || !opts.count("output")) {
        printHelp();
    }

    auto input = opts["input"].as<std::vector<std::string>>();
    auto output = opts["output"].as<std::string>();
    auto thumbSize = opts["size"].as<int>();
    auto recursive = opts["recursive"].count();
    auto maxRecursionDepth = opts["depth"].as<int>();

    ddb::generateThumbs(input, output, thumbSize, recursive, maxRecursionDepth);
}

}


