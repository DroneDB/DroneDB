/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/credentials.h"

#include "exceptions.h"
#include "include/terminal.h"

namespace cmd
{

    AuthCredentials InteractiveCredentials::credentialsFor(const std::string &)
    {
        const auto username = terminal::getPrompt("Username: ");
        const auto password = terminal::getPass("Password: ");
        return AuthCredentials(username, password);
    }

    AuthCredentials RefusedCredentials::credentialsFor(const std::string &)
    {
        throw ddb::AuthException(message);
    }

} // namespace cmd
