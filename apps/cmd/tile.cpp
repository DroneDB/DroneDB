/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/tile.h"
#include "tilerhelper.h"
#include "epttiler.h"
#include "exceptions.h"

namespace cmd
{

    void Tile::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("tile [geo.tif | image.jpg | ept.json | https://host.com/cog.tif | https://host.com/image.jpg | https://host.com/ept.json] [output directory]")
    .add_options()
    ("i,input", "Path or URL to file to tile", cxxopts::value<std::string>())
    ("o,output", "Output directory where to store tiles", cxxopts::value<std::string>()->default_value("{filename}_tiles/"))
    ("f,format", "Output format (text|json)", cxxopts::value<std::string>()->default_value("text"))
    ("z", "Zoom levels, either a single zoom level \"N\" or a range \"min-max\" or \"auto\" to generate all zoom levels", cxxopts::value<std::string>()->default_value("auto"))
    ("x", "Generate a single tile with the specified coordinate (XYZ, unless --tms is used). Must be used with -y", cxxopts::value<std::string>()->default_value("auto"))
    ("y", "Generate a single tile with the specified coordinate (XYZ, unless --tms is used). Must be used with -x", cxxopts::value<std::string>()->default_value("auto"))
    ("s,size", "Tile size", cxxopts::value<int>()->default_value("256"))
    ("tms", "Generate TMS tiles instead of XYZ", cxxopts::value<bool>());
        // clang-format on
        opts.parse_positional({"input", "output"});
    }

    std::string Tile::description()
    {
        return "Generate tiles for GeoTIFFs, GeoImages and EPT";
    }

    void Tile::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        auto input = opts["input"].as<std::string>();
        auto output = opts["output"].as<std::string>();
        if (!opts.count("output"))
        {
            // Set as filename_tiles
            output = fs::path(input).stem().string() + "_tiles";
        }

        bool tms = opts["tms"].count() > 0;
        auto format = opts["format"].as<std::string>();
        auto z = opts["z"].as<std::string>();
        auto x = opts["x"].as<std::string>();
        auto y = opts["y"].as<std::string>();
        auto tileSize = opts["size"].as<int>();

        ddb::TilerHelper::runTiler(input, output, tileSize, tms, std::cout, format, z, x, y);
    }

}
