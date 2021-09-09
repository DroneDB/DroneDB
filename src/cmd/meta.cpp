/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "meta.h"
#include "exceptions.h"
#include "dbops.h"
#include "metamanager.h"

namespace cmd {

void Meta::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("meta [add|set|rm|get|unset] [args]")
    .add_options()
    ("c,command", "Command", cxxopts::value<std::string>())
    ("k,key", "Metadata key/ID", cxxopts::value<std::string>())
    ("p,path", "Path to associate metadata with", cxxopts::value<std::string>()->default_value("."))
    ("d,data", "Data string|number|JSON to set", cxxopts::value<std::string>()->default_value(""))
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));

    // clang-format on
    opts.parse_positional({"command", "key", "data"});
}

std::string Meta::description() {
    return "Manage database metadata";
}

void Meta::run(cxxopts::ParseResult &opts) {
    if (!opts.count("command") || !opts.count("key")) {
        printHelp();
    }

    const auto ddbPath = opts["working-dir"].as<std::string>();
    const auto data = opts["data"].as<std::string>();
    const auto command = opts["command"].as<std::string>();
    const auto path = opts["path"].as<std::string>();
    auto key = opts["key"].as<std::string>();

    const auto db = ddb::open(ddbPath, true);
    auto metaManager = ddb::MetaManager(db.get());

    if (command == "add" || command == "a"){
        // Little help for singular keys (annotation --> annotations)
        // This should not be in the API, it's just a convenience
        if (key.length() > 0 && key[key.length() - 1] != 's') {
            key = key + "s";
            std::cerr << "Note: saving metadata as \"" << key << "\" (plural)" << std::endl;
        }
        std::cout << metaManager.add(key, data, path) << std::endl;
    }else if (command == "set" || command == "s"){
        std::cout << metaManager.set(key, data, path) << std::endl;
    }else if (command == "rm" || command == "r" || command == "remove"){
        std::cout << metaManager.remove(key) << std::endl;
    }else if (command == "g" || command == "get"){
        std::cout << metaManager.get(key, path) << std::endl;
    }
}

}

