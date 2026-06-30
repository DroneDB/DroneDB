/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/tiles3d.h"

#include <iostream>
#include <optional>

#include "3d.h"
#include "fs.h"

namespace cmd
{

    void Tiles3d::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("3dtiles model.obj [output_dir]")
    .add_options()
    ("i,input", "Model to process (OBJ/GLTF/GLB)", cxxopts::value<std::string>())
    ("o,output", "Output 3D Tiles directory (defaults to a 3dtiles/ folder next to the input)", cxxopts::value<std::string>())
    ("lat", "Latitude of the model origin (WGS84). With --lon, forces a georeferenced tileset", cxxopts::value<double>())
    ("lon", "Longitude of the model origin (WGS84). With --lat, forces a georeferenced tileset", cxxopts::value<double>())
    ("alt", "Altitude of the model origin in meters (used with --lat/--lon)", cxxopts::value<double>()->default_value("0"))
    ("local", "Force a non-georeferenced (local) tileset, skipping sidecar auto-detection", cxxopts::value<bool>()->default_value("false"))
    ("overwrite", "Overwrite the output directory if it exists", cxxopts::value<bool>()->default_value("false"));
        // clang-format on
        opts.parse_positional({"input", "output"});
    }

    std::string Tiles3d::description()
    {
        return "Generate OGC 3D Tiles (tileset.json + b3dm) from OBJs using Obj2Tiles.";
    }

    void Tiles3d::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        const auto input = opts["input"].as<std::string>();

        std::string output;
        if (opts.count("output"))
            output = opts["output"].as<std::string>();
        else
            output = (fs::path(input).parent_path() / "3dtiles").string();

        const bool overwrite = opts["overwrite"].as<bool>();
        const bool forceLocal = opts["local"].as<bool>();

        // Explicit --lat/--lon override sidecar auto-detection; --local forces local
        // mode; otherwise georeferencing is auto-detected from sidecars next to the input.
        std::optional<ddb::ModelGeoref> georef;
        if (opts.count("lat") && opts.count("lon"))
        {
            georef = ddb::ModelGeoref{opts["lat"].as<double>(), opts["lon"].as<double>(),
                                      opts["alt"].as<double>()};
        }

        const bool autoDetect = !forceLocal && !georef.has_value();

        std::cout << ddb::buildModel3DTiles(input, output, overwrite, georef, autoDetect) << std::endl;
    }

} // namespace cmd
