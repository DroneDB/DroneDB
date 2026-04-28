/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

#include "contour.h"
#include "include/contour.h"
#include "exceptions.h"

namespace cmd
{

    void Contour::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
        .positional_help("[args]")
        .custom_help("contour [options] input.tif")
        .add_options()
        ("input", "Input single-band raster (DEM/DSM/DTM)", cxxopts::value<std::string>())
        ("o,output", "Output GeoJSON file (default: stdout)", cxxopts::value<std::string>())
        ("i,interval", "Vertical interval between contour levels (raster units)",
            cxxopts::value<double>()->default_value("0"))
        ("n,count", "Target number of contour levels (used when --interval is 0)",
            cxxopts::value<int>()->default_value("0"))
        ("b,base", "Reference base elevation",
            cxxopts::value<double>()->default_value("0"))
        ("min", "Drop contours below this elevation",
            cxxopts::value<double>()->default_value("nan"))
        ("max", "Drop contours above this elevation",
            cxxopts::value<double>()->default_value("nan"))
        ("s,simplify", "Geometry simplification tolerance (raster CRS units)",
            cxxopts::value<double>()->default_value("0"))
        ("band", "1-based raster band index",
            cxxopts::value<int>()->default_value("1"));
        // clang-format on
        opts.parse_positional({"input"});
    }

    std::string Contour::description()
    {
        return "Generate contour lines (GeoJSON) from a DEM/DSM/DTM raster.";
    }

    void Contour::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            printHelp();
        }

        const auto input = opts["input"].as<std::string>();
        const auto interval = opts["interval"].as<double>();
        const auto count = opts["count"].as<int>();
        const auto base = opts["base"].as<double>();
        const auto minElev = opts["min"].as<double>();
        const auto maxElev = opts["max"].as<double>();
        const auto simplify = opts["simplify"].as<double>();
        const auto band = opts["band"].as<int>();

        if (interval <= 0.0 && count <= 0)
        {
            std::cerr << "Either --interval or --count must be > 0" << std::endl;
            printHelp();
        }

        ddb::ContourOptions o;
        if (interval > 0.0) o.interval = interval;
        if (count > 0) o.count = count;
        o.baseOffset = base;
        if (std::isfinite(minElev)) o.minElev = minElev;
        if (std::isfinite(maxElev)) o.maxElev = maxElev;
        o.simplifyTolerance = std::max(0.0, simplify);
        o.bandIndex = (band > 0) ? band : 1;

        const std::string json = ddb::generateContoursJson(input, o);

        if (opts.count("output"))
        {
            const auto outPath = opts["output"].as<std::string>();
            std::ofstream out(outPath);
            if (!out)
                throw ddb::AppException("Cannot open output file: " + outPath);
            out << json;
        }
        else
        {
            std::cout << json << std::endl;
        }
    }

}
