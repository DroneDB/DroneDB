/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_CLIENT_H
#define REGISTRY_CLIENT_H

// The client of a Registry (DQ-11). It owns what every request to the server has in common: the
// base URL, the authentication (login, the token and its expiry, the credentials kept in the user
// profile), the SSL check, the mapping of an error response (RFC 9457 ProblemDetails or the
// legacy { "error" } body) and of a transport error to an exception, and the retries. The HTTP
// itself goes through an IHttpClient (http_client.h), so a test runs the client against a fake.
//
// What a client does with a dataset is not here: pull (registry_ops.h), push (PushManager), clone,
// delta application (delta_apply.h) and the share upload (ShareClient) are separate units over
// this class.

#include <chrono>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "authcredentials.h"
#include "constants.h"
#include "ddb_export.h"
#include "entry.h"
#include "fs.h"
#include "http_client.h"
#include "json.h"
#include "retrypolicy.h"

namespace ddb {

class AuthManager;

// How a request presents the token. The Registry accepts both; every endpoint keeps the one it
// has always been called with.
enum class AuthScheme {
    Cookie, // Cookie: jwtToken=<token> (the dataset reads)
    Bearer  // Authorization: Bearer <token> (push and share)
};

// What a client uses besides the URL. The defaults are the real ones; a test replaces them, so
// that nothing waits, nothing reads the user's profile and nothing reaches the network.
struct RegistryDeps {
    IHttpClient *http = nullptr;        // default: defaultHttpClient()
    AuthManager *credentials = nullptr; // where login saves and where a token refresh reads the
                                        // credentials; default: the user profile's
    RetryPolicy retry = RetryPolicy::network();
    std::function<void(std::chrono::milliseconds)> sleep;      // default: sleep the thread
    std::function<std::chrono::milliseconds()> retryClock;     // default: steady_clock
    std::function<std::time_t()> now;                          // default: std::time (token expiry)
};

class RegistryClient {
    std::string url;
    std::string authToken; // last valid token
    std::time_t tokenExpiration = 0;
    bool sslVerify = true;
    RegistryDeps deps;

    IHttpClient &http() const;
    AuthManager &credentialStore() const;
    std::time_t now() const;

    HttpRequest request(const std::string &path, const HttpHeaders &extraHeaders = {}) const;
    HttpHeaders authHeaders(AuthScheme scheme) const;
    // Runs `send` and, while its response is a transport error, a 5xx or a 429, runs it again
    HttpResponse retried(const std::function<HttpResponse()> &send) const;

public:
    // Throws URLException for an URL that is not http or https; "https://" is assumed when the
    // URL has no scheme, and an empty URL is the default Registry
    DDB_DLL explicit RegistryClient(const std::string &registryUrl = DEFAULT_REGISTRY, bool verifySsl = true,
                                    const RegistryDeps &dependencies = {});

    // The base URL (normalized) followed by `path`
    DDB_DLL std::string getUrl(const std::string &path = "") const;
    DDB_DLL bool getSslVerify() const { return sslVerify; }
    DDB_DLL void setSslVerify(bool verify) { sslVerify = verify; }

    // -- Authentication (registry_client.cpp) ------------------------------------------------

    // Logs in with the credentials saved for this Registry; InvalidArgsException when there are none
    DDB_DLL std::string login();
    // Logs in and, on success, saves the credentials for next time. AuthException when it fails
    // (the message names the transport error, the server's message or the status). Safe to retry:
    // a transport error, a 5xx or a 429 is tried again before it fails.
    DDB_DLL std::string login(const std::string &username, const std::string &password);
    // Logs in again, with the saved credentials, when the token has expired. No-op without a token.
    DDB_DLL void ensureTokenValidity();
    // The credentials saved for this Registry (empty when there are none)
    DDB_DLL AuthCredentials savedCredentials() const;
    // Deletes the saved credentials; false when there were none
    DDB_DLL bool logout();
    DDB_DLL std::string getAuthToken() const { return authToken; }
    DDB_DLL std::time_t getTokenExpiration() const { return tokenExpiration; }

    // -- Authenticated requests --------------------------------------------------------------
    // Each refreshes the token when it expired, sends the token, honours the SSL setting and
    // returns the response untouched: expectOk()/fail() turn an error response into an exception.

    // GET. Tried again on a transport error, a 5xx or a 429.
    DDB_DLL HttpResponse get(const std::string &path, AuthScheme scheme);
    // POST of a form. Tried again, like a GET, only when `idempotent` says the server cannot do
    // it twice (a read that happens to need a POST).
    DDB_DLL HttpResponse postForm(const std::string &path, const HttpFields &form, AuthScheme scheme,
                                  bool idempotent = false);
    // POST of a multipart body (an upload). Never tried again.
    DDB_DLL HttpResponse postMultipart(const std::string &path, const std::vector<HttpPart> &parts,
                                       AuthScheme scheme, std::chrono::milliseconds timeout = {},
                                       const HttpProgressCallback &progress = nullptr);
    // GET of `path`, or POST of `form` when it has fields, written to `sink`. Never tried again:
    // the sink has the start of the body.
    DDB_DLL HttpResponse download(const std::string &path, AuthScheme scheme, std::ostream &sink,
                                  const HttpProgressCallback &progress = nullptr,
                                  const HttpFields &form = {});

    // Nothing when the response is a 200; otherwise fail()
    DDB_DLL void expectOk(const HttpResponse &res) const;
    // Throws what the response says went wrong: RegistryException for a transport error, an
    // error response (the ProblemDetails/legacy message, or the body) and a 200 the caller found
    // malformed; AuthException for a 401 (a 403 is not one: the credentials were accepted, the
    // user may not do this, and asking for others would not help).
    [[noreturn]] DDB_DLL void fail(const HttpResponse &res) const;

    // -- Dataset endpoints (registry_datasets.cpp) -------------------------------------------

    DDB_DLL Entry getDatasetInfo(const std::string &organization, const std::string &dataset);

    DDB_DLL void downloadDdb(const std::string &organization, const std::string &dataset,
                             const std::string &folder);

    DDB_DLL json getStamp(const std::string &organization, const std::string &dataset);

    DDB_DLL void downloadFiles(const std::string &organization, const std::string &dataset,
                               const std::vector<std::string> &files, const std::string &folder,
                               std::ostream &out);

    DDB_DLL json getMetaDump(const std::string &organization, const std::string &dataset,
                             const std::vector<std::string> &ids);
};

} // namespace ddb

#endif // REGISTRY_CLIENT_H
