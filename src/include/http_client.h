/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

// The HTTP seam of the network code. RegistryClient, PushManager, ShareClient, the DSM
// download and the tile download talk to an IHttpClient and never to cpr: cpr is included by
// one implementation file (cpr_http_client.cpp), so no installed header needs it, and a test
// substitutes a fake that answers from a script.
//
// The client does not interpret what it receives: a 404 is a response, and only a request that
// got no HTTP response at all (refused connection, TLS failure, timeout) carries a transport
// error and status 0. What a status or a body means is RegistryClient's business.

#include <chrono>
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fs.h"

namespace ddb {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// The fields of a form (application/x-www-form-urlencoded), in the order they are sent
using HttpFields = std::vector<std::pair<std::string, std::string>>;

enum class HttpTransportError {
    None,            // the server answered (whatever the status)
    CouldNotConnect, // the host is unresolvable or refuses the connection
    Other            // TLS, timeout, interrupted transfer, ...
};

struct HttpResponse {
    long status = 0;  // 0: no HTTP response, see transport
    std::string body; // empty for IHttpClient::download (the body went to the sink)
    std::map<std::string, std::string> headers; // names in lower case
    HttpTransportError transport = HttpTransportError::None;
    std::string transportMessage; // the reason, "" when there is none

    bool hasTransportError() const { return transport != HttpTransportError::None; }
    bool ok() const { return status == 200; }

    // The value of a header, whatever the case of `name`; "" when the response has none
    std::string header(const std::string &name) const;
};

struct HttpProgress {
    size_t downloadTotal = 0;
    size_t downloadNow = 0;
    size_t uploadTotal = 0;
    size_t uploadNow = 0;
};
// Returns false to abort the transfer
using HttpProgressCallback = std::function<bool(const HttpProgress &)>;

// A part of a multipart/form-data body: a text field, or a file of the disk (sent under its own
// file name, in UTF-8: never through fs::path(std::string), see mio.h "Path encoding")
struct HttpPart {
    std::string name;
    std::string value; // text field
    fs::path file;     // when not empty, the part is this file and `value` is ignored

    static HttpPart text(std::string name, std::string value) { return {std::move(name), std::move(value), {}}; }
    static HttpPart fromFile(std::string name, fs::path file) { return {std::move(name), {}, std::move(file)}; }
};

// How long a transfer may take; 0 is no limit
struct HttpLimits {
    std::chrono::milliseconds total{0};   // the whole transfer: for a short exchange only, it aborts a large file on a slow link
    std::chrono::milliseconds connect{0}; // opening the connection
    std::chrono::milliseconds stall{0};   // a transfer that moves less than 1 byte per second, in either direction, for this long
};

struct HttpRequest {
    std::string url;
    HttpHeaders headers;
    HttpFields form;                 // post(), and download() of a POST
    std::vector<HttpPart> parts;     // multipart()
    bool verifySsl = true;
    HttpLimits limits;
    HttpProgressCallback progress;   // optional: called while the body is sent or received
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    // GET; the body is in the response
    virtual HttpResponse get(const HttpRequest &request) = 0;
    // POST of request.form; the body is in the response
    virtual HttpResponse post(const HttpRequest &request) = 0;
    // POST of request.parts as multipart/form-data
    virtual HttpResponse multipart(const HttpRequest &request) = 0;
    // GET of the url, or POST of request.form when it has fields, with the body written to
    // `sink` as it arrives (nothing is buffered). A sink that fails ends the transfer with a
    // transport error.
    virtual HttpResponse download(const HttpRequest &request, std::ostream &sink) = 0;
};

// The client libddb uses (cpr). One instance for the process.
IHttpClient &defaultHttpClient();

// The Registry's own spelling of a query value (RFC 3986: everything but letters, digits and
// "-._~" is %XX), the one the STAC links and the downloads use
std::string urlEncode(std::string_view text);

} // namespace ddb

#endif // HTTP_CLIENT_H
