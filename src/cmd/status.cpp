/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "status.h"
#include "../status.h"
#include "dbops.h"

namespace cmd {

void Status::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("status [directory]")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
    // clang-format on

    // Nice to have
    // ("g,group", "Group by status", cxxopts::value<std::string>())
    // ("f,filter", "Filter entries", cxxopts::value<std::string>());
    opts.parse_positional({"working-dir"});
}

std::string Status::description() {
    return "Show files and directories index status compared to the filesystem";
}

void Status::run(cxxopts::ParseResult &opts) {

    const auto workingDir = opts["working-dir"].as<std::string>();
   
    const auto db = ddb::open(workingDir, true);

    const auto cb = [](ddb::FileStatus status, const std::string& string)
    {
        switch(status)
        {
	        case ddb::NotIndexed: 

                std::cout << "?\t";

                break;
        	
	        case ddb::Deleted: 

                std::cout << "!\t";

                break;
        	
	        case ddb::Modified: 

                std::cout << "M\t";

                break;
        	
	        default:

                std::cout << "?\t";
                
        }

        std::cout << string << std::endl;
    };
	
    statusIndex(db.get(), cb);
}

}


