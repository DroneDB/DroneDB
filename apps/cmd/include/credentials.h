/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CMD_CREDENTIALS_H
#define CMD_CREDENTIALS_H

#include <string>

#include "command.h"
#include "registry_auth.h"

namespace cmd
{

    // The CLI's answer when libddb needs credentials: ask the user on the terminal. The only
    // place that prompts for a login (libddb never does).
    class InteractiveCredentials : public ddb::CredentialProvider
    {
    public:
        AuthCredentials credentialsFor(const std::string &registryUrl) override;
    };

    // For a run that must not wait for anybody (--non-interactive): the credentials are not asked
    // for, the operation fails with `message`
    class RefusedCredentials : public ddb::CredentialProvider
    {
        std::string message;

    public:
        explicit RefusedCredentials(std::string reason) : message(std::move(reason)) {}
        AuthCredentials credentialsFor(const std::string &registryUrl) override;
    };

    // What a command that talks to a Registry gets from outside: who answers when libddb needs
    // credentials, and the collaborators of its client. The defaults are the real ones (the
    // terminal, the network); a test hands in fakes.
    struct RegistryServices
    {
        ddb::CredentialProvider *credentials = nullptr; // default: InteractiveCredentials
        ddb::RegistryDeps deps;
    };

    // The base of push, pull, clone and share
    class RegistryCommand : public Command
    {
        RegistryServices services;
        InteractiveCredentials terminal;

    protected:
        explicit RegistryCommand(RegistryServices registryServices = {}) : services(std::move(registryServices)) {}

        ddb::CredentialProvider &credentials() { return services.credentials ? *services.credentials : terminal; }
        const ddb::RegistryDeps &registryDeps() const { return services.deps; }
    };

} // namespace cmd

#endif // CMD_CREDENTIALS_H
