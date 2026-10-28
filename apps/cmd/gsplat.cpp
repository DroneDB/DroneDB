/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/gsplat.h"

#include <iostream>

#include "mio.h"
#include "splat_convert.h"
#include "splat_format.h"
#include "splat_training_export.h"
#include "stringutil.h"

namespace cmd
{
    void Gsplat::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("gsplat input.{" + ddb::str::join(ddb::splatExtensions(ddb::SplatStage::Convert), "|") + "} [output.spz]\n       gsplat --training-data <output folder> [--max-image-size <pixels>] [--max-points <count>] <ODM project folder>")
    .add_options()
    ("i,input", "Gaussian Splat file to convert (" + ddb::splatExtensionsText(ddb::SplatStage::Convert) + "), or with --training-data the ODM project folder", cxxopts::value<std::string>());
    cmd::addOutputOption(opts, "Output .spz file (default: the input with the .spz extension; a .spz input needs another path)");
    opts.add_options()
    ("training-data", "Write the training data of a Gaussian splat from the outputs of an ODM project (cameras.json, odm_report/shots.geojson, images/, odm_georeferencing/coords.txt and its point cloud) into this folder: a COLMAP model (sparse/0), the images (images/) and georef.json, which makes a splat trained from it and put next to it georeferenced", cxxopts::value<std::string>())
    ("max-image-size", "With --training-data: longest side in pixels of the exported images (default: the originals)", cxxopts::value<int>())
    ("max-points", "With --training-data: most points of the sparse model", cxxopts::value<size_t>()->default_value(std::to_string(ddb::SplatTrainingExportOptions{}.maxPoints)));
        // clang-format on
        opts.parse_positional({"input", "output"});
    }

    std::string Gsplat::description()
    {
        return "Convert a Gaussian Splat (" + ddb::splatExtensionsText(ddb::SplatStage::Convert) + ") to the compressed .spz format, or export the training data of a splat from an ODM project.";
    }

    namespace
    {
        // ddb gsplat --training-data <output folder> <ODM project folder>
        int exportTrainingData(cxxopts::ParseResult &opts)
        {
            ddb::SplatTrainingExportOptions options;
            if (opts.count("max-image-size"))
                options.maxImageSize = opts["max-image-size"].as<int>();
            options.maxPoints = opts["max-points"].as<size_t>();

            const auto outFolder = ddb::io::fromUtf8(opts["training-data"].as<std::string>());
            const auto result = ddb::exportSplatTrainingData(ddb::io::fromUtf8(opts["input"].as<std::string>()), outFolder, options);

            for (const auto &warning : result.warnings)
                std::cerr << "Warning: " << warning << std::endl;
            std::cerr << result.images << " images, " << result.points << " points" << std::endl;
            std::cout << ddb::io::toUtf8(outFolder) << std::endl;
            return EXIT_SUCCESS;
        }
    } // namespace

    int Gsplat::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            usageError();
        }

        if (opts.count("training-data"))
        {
            if (opts.count("output"))
                usageError("--training-data takes the output folder as its value, not an output file");
            return exportTrainingData(opts);
        }
        if (opts.count("max-image-size") || opts.count("max-points"))
            usageError("--max-image-size and --max-points go with --training-data");

        const auto input = opts["input"].as<std::string>();

        std::string output;
        if (opts.count("output"))
            output = opts["output"].as<std::string>();
        else
            output = ddb::io::toUtf8(ddb::io::fromUtf8(input).replace_extension(".spz"));

        // A .spz input with no output path defaults to itself: convertToSpz refuses an output that
        // is its input, with the message that asks for another path
        ddb::convertToSpz(input, output);
        std::cout << output << std::endl;
        return EXIT_SUCCESS;
    }

} // namespace cmd
