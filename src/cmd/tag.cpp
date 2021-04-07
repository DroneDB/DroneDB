/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tag.h"

#include <iostream>

#include "../tagmanager.h"
#include "dbops.h"
#include "fs.h"

namespace cmd {

void Tag::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("tag [tag]")
    .add_options()
    ("t,tag", "New tag", cxxopts::value<std::string>()->default_value(""));
    // clang-format on
    opts.parse_positional({"tag"});
}

std::string Tag::description() {
    return "Gets or sets the dataset tag.";
}

void Tag::run(cxxopts::ParseResult &opts) {
    const auto tag = opts["tag"].as<std::string>();

    const auto currentPath = std::filesystem::current_path();

    const auto db = ddb::open(currentPath.string(), true);

    ddb::TagManager manager(fs::path(db->getOpenFile()).parent_path());

    if (tag.length() > 0) {
        manager.setTag(tag);
        std::cout << manager.getTag() << std::endl;
    } else {
        const auto res = manager.getTag();

        if (res.length() != 0) {
            std::cout << res << std::endl;
        }
    }
}

}  // namespace cmd
