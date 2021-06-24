/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include <iostream>

#include "../build.h"
#include "dbops.h"
#include "fs.h"

namespace cmd {

void Build::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
        .positional_help("[args]")
        .custom_help("build path/to/file.laz --output out_dir/")
        .add_options()
        ("o,output", "Output folder", cxxopts::value<std::string>()->default_value(DEFAULT_BUILD_PATH))
        ("p,path", "File to process", cxxopts::value<std::string>());
;
    // clang-format on
    opts.parse_positional({"path"});
}

std::string Build::description() {
    return "Build all buildable files inside the index. Only LAZ files are "
           "supported so far.";
}

void Build::run(cxxopts::ParseResult &opts) {
    const auto output = opts["output"].as<std::string>();

    if (!opts.count("path")) {
        ddb::build_all(output);
    } else {
        const auto path = opts["path"].as<std::string>();
        ddb::build(path, output);
    }
}

}  // namespace cmd
