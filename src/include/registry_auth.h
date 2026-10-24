/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_AUTH_H
#define REGISTRY_AUTH_H

// How an operation on a Registry gets logged in (DQ-11). The library never asks anybody for a
// password: when the credentials it has are missing or rejected, it asks a CredentialProvider,
// and the program that embeds it decides how (the CLI prompts on the terminal, a service has
// none).

#include <functional>
#include <string>

#include "authcredentials.h"
#include "registry_client.h"

namespace ddb {

class CredentialProvider {
public:
    virtual ~CredentialProvider() = default;

    // The credentials to log in to `registryUrl` with, asked when none are saved or the ones
    // tried were rejected. Empty credentials mean "none were given" (the user typed nothing).
    // Throws AuthException when the provider cannot ask (a program that is not interactive).
    virtual AuthCredentials credentialsFor(const std::string &registryUrl) = 0;
};

struct AuthenticationOptions {
    // A Registry may serve a dataset without a login (a public one). With this, an operation
    // with no saved credentials is tried as it is first, and the provider is asked only when the
    // server answers 401 (clone). Without it, credentials come before the first request.
    bool tryAnonymousFirst = false;
};

/**
 * Runs `fn` with a client of the Registry at `url` that is logged in. The one place that decides
 * how:
 *  1. the credentials saved for the Registry (`ddb login`, an earlier run) are used; without
 *     them the provider is asked (or, with tryAnonymousFirst, nothing is);
 *  2. when the login is refused, or `fn` ends with an AuthException (the server answered 401),
 *     the provider is asked once and `fn` runs again, with a new login.
 * A second refusal propagates. The credentials that worked are saved by the login itself. `fn`
 * must be safe to run twice (it is run again from the start, with a fresh token); errors other
 * than AuthException leave at once, so a 403 or a transport error never prompts.
 */
void withAuthenticatedRegistry(const std::string &url, bool sslVerify, CredentialProvider &provider,
                               const std::function<void(RegistryClient &)> &fn,
                               const AuthenticationOptions &options = {}, const RegistryDeps &deps = {});

} // namespace ddb

#endif // REGISTRY_AUTH_H
