/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HTTP_HELPERS_H
#define HTTP_HELPERS_H

// The cpr helpers of the registry, share and DSM code (DQ-05: out of utils.h, which pulled cpr
// into every file). DQ-11 replaces them with IHttpClient and takes cpr out of the public headers.

#include <cpr/cpr.h>

#include <functional>
#include <string>

#include "fs.h"

namespace ddb::utils {

cpr::Response downloadToFile(const std::string& url,
                             const std::string& filePath,
                             bool throwOnError = false,
                             bool verifySsl = true);

std::string readFile(const std::string& url, bool throwOnError = false, bool verifySsl = true);

typedef std::function<bool(std::string& fileName, size_t txBytes, size_t totalBytes)>
    UploadCallback;

cpr::Header authHeader(const std::string& token);

cpr::Header authCookie(const std::string& token);

// A file part of a multipart upload. cpr would name the part with fs::path(std::string), the
// ANSI code page on Windows: the UTF-8 file name is passed instead (mio.h, "Path encoding")
cpr::File uploadFile(const fs::path& file);

}  // namespace ddb::utils

#endif  // HTTP_HELPERS_H
