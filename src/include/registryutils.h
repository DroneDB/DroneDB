/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRYUTILS_H
#define REGISTRYUTILS_H

#include <string>
#include "http_client.h"
#include "registry_client.h"
#include "ddb_export.h"

namespace ddb
{

    struct TagComponents
    {
        std::string registryUrl;
        std::string organization;
        std::string dataset;

        std::string tagWithoutUrl() const;
        std::string fullTag() const;
        std::string tag() const;
    };

    class RegistryUtils
    {
    public:
        DDB_DLL static TagComponents parseTag(const std::string &tag, bool useInsecureRegistry = false);
        DDB_DLL static RegistryClient createFromTag(const std::string &tag, bool useInsecureRegistry = false, bool sslVerify = true, const RegistryDeps &deps = {});

        /// The message of a Registry error body, or "" when it has none (not JSON, no known
        /// field). Reads an RFC 9457 ProblemDetails body (detail, then title) and the legacy
        /// { "error" } body, so the client works with a Registry before and after its
        /// ProblemDetails switch (RQ-05).
        DDB_DLL static std::string errorMessage(const std::string &body);

        /// Whether an error body asks the client not to try again ("noRetry": true, which
        /// Registry adds to ProblemDetails and to the legacy body).
        DDB_DLL static bool noRetry(const std::string &body);

        /// Why a request got no HTTP response at all (status 0 and a transport error): "could
        /// not connect to <url>: <reason>" for a refused or unresolvable host, "request to <url>
        /// failed: <reason>" for the other transport errors (TLS, timeout, ...). "" when the
        /// server answered, whatever the status.
        DDB_DLL static std::string transportError(const HttpResponse &res, const std::string &url);
    };

}
#endif // REGISTRYUTILS_H
