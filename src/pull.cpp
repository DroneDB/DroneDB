/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <authcredentials.h>
#include <ddb.h>
#include <mio.h>
#include <registryutils.h>
#include <tagmanager.h>
#include <userprofile.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

DDB_DLL void pull(const std::string& registry, const bool force) {
    const auto currentPath = fs::current_path().string();

    const auto db = open(currentPath, true);
   
    std::string registryUrl = registry;

    if (registry.empty()) {
        TagManager manager(fs::path(db->getOpenFile()).parent_path());
        auto tag = RegistryUtils::parseTag(manager.getTag());
        registryUrl = tag.registryUrl;
    } 

    const AuthCredentials ac =
        UserProfile::get()->getAuthManager()->loadCredentials(
            registryUrl);

    Registry reg(registryUrl);

    try {
        if (ac.empty()) {
            const auto username = utils::getPrompt("Username: ");
            const auto password = utils::getPass("Password: ");

            if (reg.login(username, password).length() <= 0)
                throw AuthException("Cannot authenticate with " +
                                         reg.getUrl());

            UserProfile::get()->getAuthManager()->saveCredentials(
                registryUrl, AuthCredentials(username, password));

            reg.pull(currentPath, force, std::cout);

        } else {
            if (reg.login(ac.username, ac.password).length() <= 0)
                throw AuthException("Cannot authenticate with " +
                                         reg.getUrl());

            reg.pull(currentPath, force, std::cout);
        }

    } catch (const AuthException&) {
        const auto username = utils::getPrompt("Username: ");
        const auto password = utils::getPass("Password: ");

        if (reg.login(username, password).length() > 0) {
            UserProfile::get()->getAuthManager()->saveCredentials(
                registryUrl, AuthCredentials(username, password));

            reg.pull(currentPath, force, std::cout);

        } else {
            throw AuthException("Cannot authenticate with " +
                                reg.getUrl());
        }
    } catch (const AppException& e) {
        throw e;
    }
}

}  // namespace ddb
