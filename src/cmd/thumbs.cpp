/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "thumbs.h"
#include "../thumbs.h"
#include "exceptions.h"

namespace cmd {

void Thumbs::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("thumbs [image.tif | *.JPG] -o [thumb.jpg | output/]")
    .add_options()
    ("i,input", "File(s) to process", cxxopts::value<std::vector<std::string>>())
    ("o,output", "Output file or directory where to store thumbnail(s)", cxxopts::value<std::string>())
    ("s,size", "Size of the largest side of the images", cxxopts::value<int>()->default_value("512"))
    ("use-crc", "Use CRC for output filenames", cxxopts::value<bool>());
    // clang-format on
    opts.parse_positional({"input"});
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
    auto useCrc = opts["use-crc"].count();

    ddb::generateThumbs(input, output, thumbSize, useCrc);
}

}


