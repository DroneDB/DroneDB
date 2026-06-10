/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include "align.h"          // ddb::alignRaster, AlignOptions, AlignMode
#include "include/align.h"  // cmd::Align
#include "exceptions.h"

namespace cmd
{

    void Align::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
        .positional_help("[args]")
        .custom_help("align -i source.tif -r reference.tif -o aligned.tif")
        .add_options()
        ("i,input",     "Source GeoTIFF to align",                         cxxopts::value<std::string>())
        ("r,reference", "Reference GeoTIFF (ground truth, more accurate)", cxxopts::value<std::string>())
        ("o,output",    "Output aligned GeoTIFF (COG, reprojected to EPSG:3857)",  cxxopts::value<std::string>())
        ("m,mode",      "similarity (default) | translation",              cxxopts::value<std::string>()->default_value("similarity"))
        ("validate",    "Only validate inputs; do not align")
        ("no-seed",     "Disable phase-correlation seed (slower NCC search)");
        // clang-format on
    }

    std::string Align::description()
    {
        return "Align a GeoTIFF (ortho or DEM) to a reference GeoTIFF, correcting georeferencing offset.";
    }

    void Align::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input") || !opts.count("reference"))
            printHelp();

        auto source    = opts["input"].as<std::string>();
        auto reference = opts["reference"].as<std::string>();

        if (opts.count("validate"))
        {
            auto r = ddb::validateAlignRaster(source, reference);
            std::cout << ddb::alignValidationToJson(r) << std::endl;
            return;
        }

        if (!opts.count("output"))
            printHelp();
        auto output = opts["output"].as<std::string>();

        ddb::AlignOptions aopts;
        auto modeStr = opts["mode"].as<std::string>();
        if (modeStr == "translation")
            aopts.mode = ddb::AlignMode::Translation;
        else if (modeStr != "similarity")
            throw ddb::InvalidArgsException("Unknown mode '" + modeStr + "'. Use: similarity | translation");

        if (opts.count("no-seed"))
            aopts.usePhaseCorrelationSeed = false;

        auto result = ddb::alignRaster(source, reference, output, aopts);
        std::cout << ddb::alignResultToJson(result) << std::endl;
    }

}
