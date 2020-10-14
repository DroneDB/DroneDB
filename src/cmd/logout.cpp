/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "logout.h"
#include "constants.h"
#include "registry.h"
#include "userprofile.h"

namespace cmd {

void Logout::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("logout")
    .add_options()
    ("server", "Registry server to logout from", cxxopts::value<std::string>()->default_value(DEFAULT_REGISTRY));

    opts.parse_positional({"server"});
}

std::string Logout::description() {
    return "Logout from all registries. To logout from a single registry, use the --server option.";
}

void Logout::run(cxxopts::ParseResult &opts) {
    if (opts["server"].count() > 0){
        ddb::Registry reg(opts["server"].as<std::string>());
        if (reg.logout()){
            std::cout << "Logged out from " << reg.getUrl() << std::endl;
        }
    }else{
        // Logout from all
        auto urls = ddb::UserProfile::get()->getAuthManager()->getAuthenticatedRegistryUrls();
        for (auto url : urls){
            ddb::Registry reg(url);
            if (reg.logout()){
                std::cout << "Logged out from " << reg.getUrl() << std::endl;
            }
        }
    }
}

}


