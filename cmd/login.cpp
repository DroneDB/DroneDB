/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "login.h"
#include "../classes/registry.h"
#include "../constants.h"

namespace cmd {

void Login::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("login")
    .add_options()
    ("host", "Registry host to authenticate to", cxxopts::value<std::string>()->default_value("index.dronedb.app"))
    ("u,username", "Username", cxxopts::value<std::string>())
    ("p,password", "Password", cxxopts::value<std::string>());

    opts.parse_positional({"host"});
}

std::string Login::description() {
    return "Authenticate with a registry.";
}

void Login::run(cxxopts::ParseResult &opts) {
    //std::string p = ddb::create(opts["directory"].as<std::string>());
    if (!opts["username"].count() || !opts["password"].count()){
        printHelp();
    }

    ddb::Registry reg(opts["host"].as<std::string>());
    std::string token = reg.login(opts["username"].as<std::string>(), opts["password"].as<std::string>());
    if (token.length() > 0){
        std::cout << "Login succeeded" << std::endl;
    }
}

}


