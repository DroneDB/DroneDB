/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HTTP_HELPERS_H
#define HTTP_HELPERS_H

// Small helpers over an IHttpClient (DQ-11: cpr is gone from this header; the DQ-05 helpers of
// the registry and share code moved into RegistryClient, readFile had no caller).

#include <string>

#include "http_client.h"

namespace ddb::utils {

// Downloads `url` to `filePath` (replacing it). A failed or empty download leaves no file and, with
// throwOnError, throws NetException; the response is returned either way.
HttpResponse downloadToFile(IHttpClient& client,
                            const std::string& url,
                            const std::string& filePath,
                            bool throwOnError = false,
                            bool verifySsl = true);

// The same with the library's own client (defaultHttpClient)
HttpResponse downloadToFile(const std::string& url,
                            const std::string& filePath,
                            bool throwOnError = false,
                            bool verifySsl = true);

}  // namespace ddb::utils

#endif  // HTTP_HELPERS_H
