/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "share.h"
#include "constants.h"

namespace cmd {

void Share::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("share")
    .add_options()

    ("i,input", "Files and directories to share", cxxopts::value<std::vector<std::string>>())
    ("t,tag", "Tag to use (organization/dataset or server[:port]/organization/dataset)", cxxopts::value<std::string>()->default_value(DEFAULT_REGISTRY "/public/<randomID>"))
    ("p,password", "Optional password to protect dataset", cxxopts::value<std::string>()->default_value(""));

    opts.parse_positional({"input"});
}

std::string Share::description() {
    return "Share files and folders to a registry";
}

void Share::run(cxxopts::ParseResult &opts) {
//    std::string username;
//    std::string password;

//    if (opts["username"].count() > 0){
//        username = opts["username"].as<std::string>();
//    }else{
//        username = ddb::utils::getPrompt("Username: ");
//    }

//    if (opts["password"].count() > 0){
//        password = opts["password"].as<std::string>();
//    }else{
//        password = ddb::utils::getPass("Password: ");
//    }

//    ddb::Registry reg(opts["host"].as<std::string>());
//    std::string token = reg.login(username, password);
//    if (token.length() > 0){
//        std::cout << "Login succeeded for " << reg.getUrl() << std::endl;
//    }
}

}


