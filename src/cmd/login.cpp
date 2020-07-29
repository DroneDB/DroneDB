/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "login.h"
#include "registry.h"
#include "constants.h"
#include "utils.h"

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
    std::string username;
    std::string password;

    if (opts["username"].count() > 0){
        username = opts["username"].as<std::string>();
    }else{
        username = ddb::utils::getPrompt("Username: ");
    }

    if (opts["password"].count() > 0){
        password = opts["password"].as<std::string>();
    }else{
        password = ddb::utils::getPass("Password: ");
    }

    ddb::Registry reg(opts["host"].as<std::string>());
    std::string token = reg.login(username, password);
    if (token.length() > 0){
        std::cout << "Login succeeded for " << reg.getUrl() << std::endl;
    }
}

}


