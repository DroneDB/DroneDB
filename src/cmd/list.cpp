/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "list.h"

#include <dbops.h>

#include "exceptions.h"
#include "basicgeometry.h"

namespace cmd {

void List::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("list *.JPG")
    .add_options()
    ("d,directory", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("i,input", "File(s) to list", cxxopts::value<std::string>());

    opts.parse_positional({"input"});
}

std::string List::description() {
    return "List indexed files and directories";
}

void List::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input")) {
        printHelp();
    }
	
    const auto ddbPath = opts["directory"].as<std::string>();
    const auto input = opts["input"].as<std::string>();

    try {

        auto db = ddb::open(std::string(ddbPath), true);
           
        ddb::listIndex(db.get(), input);
        
    } catch(ddb::InvalidArgsException){
        printHelp();
    }
}

}


