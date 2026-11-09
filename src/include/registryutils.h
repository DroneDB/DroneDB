/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRYUTILS_H
#define REGISTRYUTILS_H

#include <string>
#include "http_client.h"
#include "registry_client.h"

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
        /// The URL of a Registry in its one spelling, "scheme://host[:port]": the port only when it
        /// is not the one of the scheme, and the path (without a trailing slash) with `keepPath`.
        /// A URL without a scheme gets https, or http with useInsecureRegistry; an empty one is
        /// the default Registry. Throws an InvalidArgsException for a scheme other than http or
        /// https ("ftp://x" is not "https://ftp://x"), for a text that is not a URL and for one
        /// without a host. The client and the tag parser both read their URL through it.
        static std::string normalizeUrl(const std::string &url, bool useInsecureRegistry = false,
                                        bool keepPath = false);

        static TagComponents parseTag(const std::string &tag, bool useInsecureRegistry = false);

        /**
         * The target of a share: a tag as parseTag reads it, or a server only.
         *
         * A server only is "<server>//" (the form of `ddb share -s <server>`, and the default
         * tag "hub.dronedb.app//"; "//" alone is the default Registry) or a URL with a scheme
         * and no path ("http://host:5200", "http://host:5200/"). It gives registryUrl =
         * normalizeUrl(server) and an empty organization and dataset, so tagWithoutUrl() is ""
         * and the Registry makes a dataset with a random name in the personal organization of
         * the user. Anything else goes through parseTag, with its errors.
         *
         * Only the share takes a server only: for push, pull and clone parseTag refuses it, a
         * tag without a dataset.
         */
        static TagComponents parseShareTarget(const std::string &tag, bool useInsecureRegistry = false);

        static RegistryClient createFromTag(const std::string &tag, bool useInsecureRegistry = false, bool sslVerify = true, const RegistryDeps &deps = {});

        /// The message of a Registry error body, or "" when it has none (not JSON, no known
        /// field). Reads an RFC 9457 ProblemDetails body (detail, then title) and the legacy
        /// { "error" } body, so the client works with a Registry before and after its
        /// ProblemDetails switch.
        static std::string errorMessage(const std::string &body);

        /// Whether an error body asks the client not to try again ("noRetry": true, which
        /// Registry adds to ProblemDetails and to the legacy body).
        static bool noRetry(const std::string &body);

        /// Why a request got no HTTP response at all (status 0 and a transport error): "could
        /// not connect to <url>: <reason>" for a refused or unresolvable host, "request to <url>
        /// failed: <reason>" for the other transport errors (TLS, timeout, ...). "" when the
        /// server answered, whatever the status.
        static std::string transportError(const HttpResponse &res, const std::string &url);
    };

}
#endif // REGISTRYUTILS_H
