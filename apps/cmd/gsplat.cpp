/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/gsplat.h"

#include <iostream>

#include "mio.h"
#include "splat_convert.h"
#include "splat_format.h"
#include "stringutil.h"

namespace cmd
{
    void Gsplat::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("gsplat input.{" + ddb::str::join(ddb::splatExtensions(ddb::SplatStage::Convert), "|") + "} [output.spz]")
    .add_options()
    ("i,input", "Gaussian Splat file to convert (" + ddb::splatExtensionsText(ddb::SplatStage::Convert) + ")", cxxopts::value<std::string>());
    cmd::addOutputOption(opts, "Output .spz file (default: the input with the .spz extension; a .spz input needs another path)");
        // clang-format on
        opts.parse_positional({"input", "output"});
    }

    std::string Gsplat::description()
    {
        return "Convert a Gaussian Splat (" + ddb::splatExtensionsText(ddb::SplatStage::Convert) + ") to the compressed .spz format.";
    }

    int Gsplat::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            usageError();
        }

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
