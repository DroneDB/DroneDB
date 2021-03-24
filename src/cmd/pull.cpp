/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "pull.h"

#include <authcredentials.h>
#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <tagmanager.h>
#include <url/Url.h>
#include <userprofile.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"

namespace cmd {
void Pull::setOptions(cxxopts::Options& opts) {
    // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("pull [-f|--force] [remote]")
            .add_options()
            ("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""))
            ("f,force", "Forces the operation", cxxopts::value<bool>()->default_value("false"));

    // clang-format on
    opts.parse_positional({"remote"});
}

std::string Pull::description() {
    return "Pulls changes from remote repository.";
}

void Pull::run(cxxopts::ParseResult& opts) {
    try {
        const auto force = opts["force"].as<bool>();

        auto remote = opts["remote"].as<std::string>();
        remote = remote.length() > 0 ? remote : DEFAULT_REGISTRY;

        const auto currentPath = fs::current_path().string();

        ddb::TagManager manager(currentPath);
        
        ddb::TagComponents tag = ddb::RegistryUtils::parseTag(manager.getTag());

        std::cout << "Pulling dataset '" << tag.organization << "/"
                  << tag.dataset << "' from registry '" << tag.registryUrl
                  << std::endl;

        const AuthCredentials ac =
            ddb::UserProfile::get()->getAuthManager()->loadCredentials(
                tag.registryUrl);

        ddb::Registry reg(tag.registryUrl);

        try {
            if (ac.empty()) {
                const auto username = ddb::utils::getPrompt("Username: ");
                const auto password = ddb::utils::getPass("Password: ");

                if (reg.login(username, password).length() <= 0)
                    throw ddb::AuthException("Cannot authenticate with " +
                                             reg.getUrl());

                ddb::UserProfile::get()->getAuthManager()->saveCredentials(
                    tag.registryUrl, AuthCredentials(username, password));

                reg.pull(currentPath, force, std::cout);

            } else {
                if (reg.login(ac.username, ac.password).length() <= 0)
                    throw ddb::AuthException("Cannot authenticate with " +
                                             reg.getUrl());

                reg.pull(currentPath, force, std::cout);
            }

        } catch (const ddb::AuthException&) {
            const auto username = ddb::utils::getPrompt("Username: ");
            const auto password = ddb::utils::getPass("Password: ");

            if (reg.login(username, password).length() > 0) {
                ddb::UserProfile::get()->getAuthManager()->saveCredentials(
                    tag.registryUrl, AuthCredentials(username, password));

                reg.pull(currentPath, force, std::cout);

            } else {
                throw ddb::AuthException("Cannot authenticate with " +
                                         reg.getUrl());
            }
        } catch (const ddb::AppException& e) {
            throw e;
        }
    } catch (ddb::InvalidArgsException) {
        printHelp();
    }
}

}  // namespace cmd
