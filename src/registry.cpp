/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "registry.h"

#include <mio.h>

#include "../vendor/miniz-cpp/zip_file.hpp"
#include "exceptions.h"
#include "json.h"
#include "net.h"
#include "url.h"
#include "userprofile.h"
#include "utils.h"

using homer6::Url;

namespace ddb {

Registry::Registry(const std::string &url) {
    std::string urlStr = url;
    if (urlStr.empty()) urlStr = std::string(DEFAULT_REGISTRY);

    Url u;

    // Always append https if no protocol is specified
    if (urlStr.find("https://") != 0 && urlStr.find("http://") != 0) {
        u.fromString("https://" + urlStr);
    } else {
        u.fromString(urlStr);
    }

    // Validate and set URL
    if (u.getScheme() != "https" && u.getScheme() != "http") {
        throw URLException("Registry URL can only be http/https");
    }

    const std::string port = u.getPort() != 80 && u.getPort() != 443
                                 ? ":" + std::to_string(u.getPort())
                                 : "";
    this->url = u.getScheme() + "://" + u.getHost() + port + u.getPath();

    LOGD << "Registry URL: " << this->url;
}

std::string Registry::getUrl(const std::string &path) const {
    return url + path;
}

std::string Registry::login() {
    const auto ac =
        UserProfile::get()->getAuthManager()->loadCredentials(this->url);

    if (ac.empty())
        throw InvalidArgsException("No stored credentials for registry at '" +
                                   this->url + "'");

    return login(ac.username, ac.password);
}

std::string Registry::login(const std::string &username,
                            const std::string &password) {
    net::Response res =
        net::POST(getUrl("/users/authenticate"))
            .formData({"username", username, "password", password})
            .send();

    json j = res.getJSON();

    if (res.status() == 200 && j.contains("token")) {
        const auto token = j["token"].get<std::string>();
        const auto expiration = j["expires"].get<time_t>();

        // Save for next time
        UserProfile::get()->getAuthManager()->saveCredentials(
            url, AuthCredentials(username, password));

        this->authToken = token;
        this->tokenExpiration = expiration;

        LOGD << "AuthToken = " << token;
        LOGD << "Expiration = " << expiration;

        return std::string(token);
    }
    if (j.contains("error")) {
        throw AuthException("Login failed: " + j["error"].get<std::string>());
    }

    throw AuthException("Login failed: host returned " +
                        std::to_string(res.status()));
}

DDB_DLL void Registry::ensureTokenValidity() {
    if (this->authToken.empty())
        throw InvalidArgsException("No auth token is present");

    const auto now = std::time(nullptr);

    LOGD << "Now = " << now << ", expration = " << this->tokenExpiration
         << ", diff = " << now - this->tokenExpiration;

    // If the token is still valid we have nothing to do
    if (now < this->tokenExpiration) {
        LOGD << "Token still valid";
        return;
    }

    LOGD << "Token expired: re-login";
    // Otherwise login
    this->login();
}

bool Registry::logout() {
    return UserProfile::get()->getAuthManager()->deleteCredentials(url);
}

DDB_DLL void Registry::clone(const std::string &organization,
                             const std::string &dataset,
                             const std::string &folder) {
    // Workflow
    // 2) Download zip in temp folder
    // 3) Create target folder
    // 3.1) If target folder already exists throw error
    // 4) Unzip in target folder
    // 5) Remove temp zip
    // 6) Update sync information

    this->ensureTokenValidity();

    const Url url(this->url);

    const auto downloadUrl = url.getHost() + "/orgs/" + organization + "/ds/" +
                             dataset + "/download";

    LOGD << "Downloading dataset '" << dataset << "' of organization '"
         << organization << "'";
    LOGD << "To folder: " << folder;

    LOGD << "Download url = " << downloadUrl;

    const auto tempFile =
        io::Path(fs::temp_directory_path() / std::to_string(time(nullptr)))
            .string() +
        ".tmp";

    LOGD << "Temp file = " << tempFile;

    auto res = net::GET(downloadUrl)
                   .authCookie(this->authToken)
                   .verifySSL(false)
                   .downloadToFile(tempFile);

    if (res.status() != 200) this->handleError(res);

    LOGD << "Dataset downloaded, now extracting it";
    std::filesystem::create_directory(folder);

    miniz_cpp::zip_file file;

    file.load(tempFile);
    file.extractall(folder);

    LOGD << "Extracted to destination folder";

    std::filesystem::remove(tempFile);

    LOGD << "Removed temp file";
}

std::string Registry::getAuthToken() { return std::string(this->authToken); }

time_t Registry::getTokenExpiration() { return this->tokenExpiration; }

void Registry::handleError(net::Response &res) {
    if (res.hasData()) {
        LOGD << "Request error: " << res.getText();

        auto j = res.getJSON();
        if (j.contains("error"))
            throw RegistryException("Error response from registry: " +
                                    j["error"].get<std::string>());
        throw RegistryException("Invalid response from registry: " +
                                res.getText());
    }

    LOGD << "Request error: " << res.status();

    throw RegistryException(
        "Invalid response from registry. Returned status: " +
        std::to_string(res.status()));
}

}  // namespace ddb
