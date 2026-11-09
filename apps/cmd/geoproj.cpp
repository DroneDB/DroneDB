/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/geoproj.h"
#include "dbops.h"
#include <geoproject.h>

namespace cmd
{

    void GeoProj::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("geoproj output/ *.JPG\n       geoproj -o output/ *.JPG");
    cmd::addOutputOption(opts, "Output path (file, or directory: one that exists or ends with /)");
    opts.add_options()
    ("i,images", "Images to project", cxxopts::value<std::vector<std::string>>())
    ("s,size", "Output image size (size[%]|0)", cxxopts::value<std::string>()->default_value("100%"));
    cmd::addOverwriteOption(opts);
    // Every positional argument, apart from -i: with -o they are all images, without it the
    // first is the output (not in the help, which shows them as [args])
    opts.add_options("positional")
    ("args", "Output and images", cxxopts::value<std::vector<std::string>>());
        // clang-format on
        opts.parse_positional({"args"});
    }

    std::string GeoProj::description()
    {
        return "Project images to georeferenced rasters";
    }

    int GeoProj::run(cxxopts::ParseResult &opts)
    {
        std::vector<std::string> images;
        if (opts.count("images"))
            images = opts["images"].as<std::vector<std::string>>();
        std::vector<std::string> args;
        if (opts.count("args"))
            args = opts["args"].as<std::vector<std::string>>();

        std::string output;
        auto firstImage = args.begin();
        if (opts.count("output"))
            output = opts["output"].as<std::string>();
        else if (!args.empty())
            output = *firstImage++;
        images.insert(images.end(), firstImage, args.end());

        if (output.empty() || images.empty())
        {
            usageError();
        }

        auto outsize = opts["size"].as<std::string>();

        ddb::geoProject(images, output, outsize, false, [](const std::string &imageWritten)
                        {
        std::cout << "W\t" << imageWritten << std::endl;
        return true; }, cmd::getOverwriteOption(opts));
        return EXIT_SUCCESS;
    }

}
