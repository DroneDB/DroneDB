/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registry_auth.h"

#include "exceptions.h"

namespace ddb {

namespace {

// The credentials of the provider, which must hold something to log in with
AuthCredentials askProvider(CredentialProvider &provider, const RegistryClient &client) {
    const auto credentials = provider.credentialsFor(client.getUrl());
    if (credentials.empty())
        throw AuthException("Cannot authenticate with " + client.getUrl() + ": no credentials were given");
    return credentials;
}

void loginAndRun(RegistryClient &client, const AuthCredentials &credentials,
                 const std::function<void(RegistryClient &)> &fn) {
    client.login(credentials.username, credentials.password);
    fn(client);
}

}  // namespace

void withAuthenticatedRegistry(const std::string &url, bool sslVerify, CredentialProvider &provider,
                               const std::function<void(RegistryClient &)> &fn,
                               const AuthenticationOptions &options, const RegistryDeps &deps) {
    RegistryClient client(url, sslVerify, deps);

    const AuthCredentials saved = client.savedCredentials();

    // First attempt: what is saved; else what the provider gives, else (clone) no login at all
    const bool anonymous = saved.empty() && options.tryAnonymousFirst;
    const AuthCredentials first = anonymous ? AuthCredentials() : (saved.empty() ? askProvider(provider, client) : saved);

    try {
        if (anonymous)
            fn(client);
        else
            loginAndRun(client, first, fn);
    } catch (const AuthException &) {
        // Refused: the saved password changed, or the one typed was wrong, or the dataset is
        // private. One more try with what the provider gives now.
        loginAndRun(client, askProvider(provider, client), fn);
    }
}

}  // namespace ddb
