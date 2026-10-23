/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registry_client.h"

#include <algorithm>
#include <cctype>
#include <thread>

#include "authmanager.h"
#include "exceptions.h"
#include "registryutils.h"
#include "userprofile.h"
#include "../../vendor/url/Url.h"

using homer6::Url;

namespace ddb {

namespace {

// A Retry-After given in seconds (the date form is not honoured: the policy's backoff applies)
std::chrono::milliseconds retryAfterOf(const HttpResponse &res) {
    const std::string value = res.header("retry-after");
    if (value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::chrono::milliseconds{0};
    return std::chrono::seconds(std::stoul(value));
}

// What is worth asking again: the server was not reached, it is busy (429, for as long as its
// Retry-After says) or failed (5xx). Not what it refused (a 4xx), and nothing it marked
// "noRetry" in its error body.
RetryDecision retryDecisionOf(const HttpResponse &res) {
    if (res.hasTransportError())
        return RetryDecision::again();
    if (RegistryUtils::noRetry(res.body))
        return RetryDecision::stop();
    if (res.status == 429)
        return RetryDecision::again(retryAfterOf(res));
    if (res.status >= 500)
        return RetryDecision::again();
    return RetryDecision::stop();
}

}  // namespace

RegistryClient::RegistryClient(const std::string &registryUrl, bool verifySsl, const RegistryDeps &dependencies)
    : sslVerify(verifySsl), deps(dependencies) {
    std::string urlStr = registryUrl;

    if (urlStr.empty())
        urlStr = std::string(DEFAULT_REGISTRY);

    Url u;

    // Always append https if no protocol is specified
    if (!urlStr.starts_with("https://") && !urlStr.starts_with("http://")) {
        u.fromString("https://" + urlStr);
    } else {
        u.fromString(urlStr);
    }

    // Validate and set URL
    if (u.getScheme() != "https" && u.getScheme() != "http") {
        throw URLException("Registry URL can only be http/https");
    }

    const std::string port =
        u.getPort() != 80 && u.getPort() != 443 ? ":" + std::to_string(u.getPort()) : "";
    this->url = u.getScheme() + "://" + u.getHost() + port + u.getPath();
}

std::string RegistryClient::getUrl(const std::string &path) const {
    return url + path;
}

IHttpClient &RegistryClient::http() const {
    return deps.http != nullptr ? *deps.http : defaultHttpClient();
}

AuthManager &RegistryClient::credentialStore() const {
    return deps.credentials != nullptr ? *deps.credentials : *UserProfile::get()->getAuthManager();
}

std::time_t RegistryClient::now() const {
    return deps.now ? deps.now() : std::time(nullptr);
}

HttpRequest RegistryClient::request(const std::string &path, const HttpHeaders &extraHeaders) const {
    HttpRequest req;
    req.url = getUrl(path);
    req.headers = extraHeaders;
    req.verifySsl = sslVerify;
    return req;
}

HttpHeaders RegistryClient::authHeaders(AuthScheme scheme) const {
    switch (scheme) {
        case AuthScheme::Bearer:
            return {{"Authorization", "Bearer " + authToken}};
        case AuthScheme::Cookie:
        default:
            return {{"Cookie", "jwtToken=" + authToken}};
    }
}

HttpResponse RegistryClient::retried(const std::function<HttpResponse()> &send) const {
    const auto sleep = [this](std::chrono::milliseconds delay) {
        if (deps.sleep)
            deps.sleep(delay);
        else
            ThreadRetrySleeper{}(delay);
    };
    const auto clock = [this]() { return deps.retryClock ? deps.retryClock() : SteadyRetryClock{}(); };
    return deps.retry.runIf(send, retryDecisionOf, "request to the registry", sleep, clock);
}

// -- Authentication ----------------------------------------------------------------------------

AuthCredentials RegistryClient::savedCredentials() const {
    return credentialStore().loadCredentials(this->url);
}

std::string RegistryClient::login() {
    const auto ac = savedCredentials();

    if (ac.empty())
        throw InvalidArgsException("No stored credentials for registry at '" + this->url + "'");

    return login(ac.username, ac.password);
}

std::string RegistryClient::login(const std::string &username, const std::string &password) {
    HttpRequest req = request("/users/authenticate");
    req.form = {{"username", username}, {"password", password}};

    // A login can be asked again: it creates nothing on the server
    const HttpResponse res = retried([&]() { return http().post(req); });

    if (res.ok()) {
        const json j = json::parse(res.body, nullptr, false);
        if (!j.is_object() || !j.contains("token") || !j["token"].is_string() || !j.contains("expires") ||
            !j["expires"].is_number())
            throw AuthException("Login failed: invalid response from the registry");

        const auto token = j["token"].get<std::string>();
        const auto expiration = j["expires"].get<std::time_t>();

        // Save for next time
        credentialStore().saveCredentials(url, AuthCredentials(username, password));

        this->authToken = token;
        this->tokenExpiration = expiration;

        return token;
    }
    if (const auto transport = RegistryUtils::transportError(res, this->url); !transport.empty())
        throw AuthException("Login failed: " + transport);
    if (const auto message = RegistryUtils::errorMessage(res.body); !message.empty())
        throw AuthException("Login failed: " + message);

    throw AuthException("Login failed: host returned " + std::to_string(res.status));
}

void RegistryClient::ensureTokenValidity() {
    if (this->authToken.empty())
        return;  // No credentials saved

    // If the token is still valid we have nothing to do
    if (now() < this->tokenExpiration)
        return;

    // Otherwise login
    this->login();
}

bool RegistryClient::logout() {
    return credentialStore().deleteCredentials(url);
}

// -- Authenticated requests --------------------------------------------------------------------

HttpResponse RegistryClient::get(const std::string &path, AuthScheme scheme) {
    ensureTokenValidity();
    const HttpRequest req = request(path, authHeaders(scheme));
    return retried([&]() { return http().get(req); });
}

HttpResponse RegistryClient::postForm(const std::string &path, const HttpFields &form, AuthScheme scheme,
                                      bool idempotent) {
    ensureTokenValidity();
    HttpRequest req = request(path, authHeaders(scheme));
    req.form = form;
    if (idempotent)
        return retried([&]() { return http().post(req); });
    return http().post(req);
}

HttpResponse RegistryClient::postMultipart(const std::string &path, const std::vector<HttpPart> &parts,
                                           AuthScheme scheme, std::chrono::milliseconds timeout,
                                           const HttpProgressCallback &progress) {
    ensureTokenValidity();
    HttpRequest req = request(path, authHeaders(scheme));
    req.parts = parts;
    req.timeout = timeout;
    req.progress = progress;
    return http().multipart(req);
}

HttpResponse RegistryClient::download(const std::string &path, AuthScheme scheme, std::ostream &sink,
                                      const HttpProgressCallback &progress, const HttpFields &form) {
    ensureTokenValidity();
    HttpRequest req = request(path, authHeaders(scheme));
    req.form = form;
    req.progress = progress;
    return http().download(req, sink);
}

// -- Errors ------------------------------------------------------------------------------------

void RegistryClient::expectOk(const HttpResponse &res) const {
    if (!res.ok())
        fail(res);
}

void RegistryClient::fail(const HttpResponse &res) const {
    // No HTTP response at all (status 0): the server was not reached
    if (const auto transport = RegistryUtils::transportError(res, this->url); !transport.empty())
        throw RegistryException("Registry request failed: " + transport);

    if (res.status == 401)
        throw AuthException("Unauthorized");

    if (res.status != 200) {
        if (const auto message = RegistryUtils::errorMessage(res.body); !message.empty())
            throw RegistryException("Error response from registry: " + message);
        throw RegistryException("Invalid response from registry: " + res.body);
    }

    throw RegistryException("Invalid response from registry. Returned status: " +
                            std::to_string(res.status));
}

}  // namespace ddb
