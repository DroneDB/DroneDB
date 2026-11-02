/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CPR_HTTP_CLIENT_H
#define CPR_HTTP_CLIENT_H

#include "http_client.h"

namespace ddb {

// IHttpClient over cpr (libcurl). The only code that includes cpr is cpr_http_client.cpp, so this
// header does not: everything here is the interface.
class CprHttpClient final : public IHttpClient {
public:
    HttpResponse get(const HttpRequest &request) override;
    HttpResponse post(const HttpRequest &request) override;
    HttpResponse multipart(const HttpRequest &request) override;
    HttpResponse download(const HttpRequest &request, std::ostream &sink) override;
};

} // namespace ddb

#endif // CPR_HTTP_CLIENT_H
