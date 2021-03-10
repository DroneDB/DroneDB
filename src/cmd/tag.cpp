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
    ("t,tag", "New tag", cxxopts::value<std::string>()->default_value(""))
    ("r,registry", "Registry", cxxopts::value<std::string>()->default_value(""));
    // clang-format on
    opts.parse_positional({"tag"});
}

std::string Tag::description() {
    return "Shows or sets the tag.\nShow: ddb tag.\nSet: ddb tag "
           "<org>/<dataset>\nor: ddb tag <server>/<org>/<dataset>";
}

void Tag::run(cxxopts::ParseResult &opts) {
    const auto tag = opts["tag"].as<std::string>();
    auto registry = opts["registry"].as<std::string>();

    const auto currentPath = std::filesystem::current_path();

    if (registry.length() == 0) registry = DEFAULT_REGISTRY;

    ddb::TagManager manager(currentPath);

    if (tag.length() > 0) {
        manager.setTag(tag, registry);
        std::cout << "[" << registry << "] " << tag;

    } else {
        const auto res = manager.getTag(registry);
        std::cout << "[" << registry << "] " << res;

    }

}

}  // namespace cmd