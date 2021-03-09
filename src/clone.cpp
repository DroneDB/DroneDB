/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <authcredentials.h>
#include <mio.h>
#include <registryutils.h>
#include <userprofile.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

void clone(const TagComponents& tag, const std::string& folder) {
    if (fs::exists(folder)) {
        std::cout << "Cannot clone in folder '" + folder +
                         "' because it already exists" << std::endl;
        return;
    }

    std::cout << "Cloning dataset '" << tag.organization << "/" << tag.dataset
              << "' from registry '" << tag.registryUrl << "' to folder '"
              << folder << "'" << std::endl;

    const AuthCredentials ac =
        UserProfile::get()->getAuthManager()->loadCredentials(tag.registryUrl);

    Registry reg(tag.registryUrl);

    try {
        if (ac.empty()) {

            const auto username = utils::getPrompt("Username: ");
            const auto password = utils::getPass("Password: ");

            if (reg.login(username, password).length() <= 0) 
                throw AuthException("Cannot authenticate with " + reg.getUrl());
            
            UserProfile::get()->getAuthManager()->saveCredentials(
                tag.registryUrl, AuthCredentials(username, password));

            reg.clone(tag.organization, tag.dataset, folder, std::cout);

        } else {

            if (reg.login(ac.username, ac.password).length() <= 0) 
                throw AuthException("Cannot authenticate with " + reg.getUrl());
            
            reg.clone(tag.organization, tag.dataset, folder, std::cout);
        }

    } catch (const AuthException&) {
        const auto username = utils::getPrompt("Username: ");
        const auto password = utils::getPass("Password: ");

        if (reg.login(username, password).length() > 0) {

            UserProfile::get()->getAuthManager()->saveCredentials(
                tag.registryUrl, AuthCredentials(username, password));

            reg.clone(tag.organization, tag.dataset, folder, std::cout);

        } else {
            throw AuthException("Cannot authenticate with " + reg.getUrl());
        }
    } catch (const AppException& e) {
        throw e;
    }
}

}  // namespace ddb
