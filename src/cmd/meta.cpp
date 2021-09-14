/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <string>
#include "meta.h"
#include "exceptions.h"
#include "dbops.h"
#include "metamanager.h"

namespace cmd {

void Meta::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("meta [add|set|rm|get|unset|ls] [args]")
    .add_options()
    ("c,command", "Command", cxxopts::value<std::string>())
    ("k,key", "Metadata key/ID", cxxopts::value<std::string>()->default_value(""))
    ("p,path", "Path to associate metadata with", cxxopts::value<std::string>()->default_value("."))
    ("d,data", "Data string|number|JSON to set", cxxopts::value<std::string>()->default_value(""))
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("f,format", "Output format (text|json)", cxxopts::value<std::string>()->default_value("text"));

    // clang-format on
    opts.parse_positional({"command", "key", "data"});
}

std::string Meta::description() {
    return "Manage database metadata";
}

void Meta::output(std::ostream &out, const json &j, const std::string &format){
    if (format == "json") out << j << std::endl;
    else if (format == "text") printJsonToText(out, j);
    else throw ddb::InvalidArgsException("Invalid format " + format);
}

void Meta::printJsonToText(std::ostream &out, const json &j)
{
    if (j.is_array()){
        size_t count = 0;

        for (const auto &it : j){
            for (const auto &item : it.items()){
                std::string k = item.key();
                if (k.length() > 0) k[0] = std::toupper(k[0]);
                std::string v = item.value().dump();
                if (item.value().is_string()) v = item.value().get<std::string>();

                out << k << ": " << v << std::endl;
            }
            if (++count < j.size()) out << "--------" << std::endl;
        }
    }else{
        for (const auto &item : j.items()){
            std::string k = item.key();
            if (k.length() > 0) k[0] = std::toupper(k[0]);
            std::string v = item.value().dump();
            if (item.value().is_string()) v = item.value().get<std::string>();

            out << k << ": " << v << std::endl;
        }
    }
}

void Meta::run(cxxopts::ParseResult &opts) {
    if (!opts.count("command")) {
        printHelp();
    }
    const auto command = opts["command"].as<std::string>();

    if (command != "ls" && command != "list" && command != "l" && !opts.count("key")){
        printHelp();
    }

    const auto ddbPath = opts["working-dir"].as<std::string>();
    const auto data = opts["data"].as<std::string>();
    const auto path = opts["path"].as<std::string>();
    const auto format = opts["format"].as<std::string>();
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
        output(std::cout, metaManager.add(key, data, path), format);
    }else if (command == "set" || command == "s"){
        output(std::cout, metaManager.set(key, data, path) , format);
    }else if (command == "rm" || command == "r" || command == "remove"){
        output(std::cout, metaManager.remove(key), format);
    }else if (command == "get" || command == "g"){
        output(std::cout, metaManager.get(key, path), format);
    }else if (command == "unset" || command == "u"){
        output(std::cout, metaManager.unset(key, path), format);
    }else if (command == "list" || command == "ls" || command == "l"){
        output(std::cout, metaManager.list(path), format);
    }
}

}

