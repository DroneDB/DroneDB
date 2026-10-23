/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "http_helpers.h"

#include <fstream>
#include <iterator>
#include <string>

#include "exceptions.h"
#include "mio.h"

namespace ddb::utils {

cpr::Response downloadToFile(const std::string& url,
                             const std::string& filePath,
                             bool throwOnError,
                             bool verifySsl) {
    auto session = cpr::Session();
    session.SetUrl(cpr::Url{url});
    session.SetVerifySsl(verifySsl);
    session.SetTimeout(15000);
    session.SetHeader(cpr::Header{{"Accept-Encoding", "gzip"}});

    if (fs::exists(io::fromUtf8(filePath)))
        fs::remove(io::fromUtf8(filePath));

    std::ofstream out(io::fromUtf8(filePath), std::ios::binary);

    auto response = session.Download(out);
    out.close(); // Ensure file is closed before checking size

    // Check if download was successful and file has content
    if (response.status_code != 200 || !fs::exists(io::fromUtf8(filePath)) || fs::file_size(io::fromUtf8(filePath)) == 0) {
        // Remove empty or failed download file
        if (fs::exists(io::fromUtf8(filePath)))
            fs::remove(io::fromUtf8(filePath));

        if (throwOnError)
            throw ddb::NetException("Failed to fetch data from " + url + " (status: " + std::to_string(response.status_code) + ")");
    }

    return response;
}

std::string readFile(const std::string& address, bool throwOnError, bool verifySsl) {
    // Check if address is a local path or an url
    if (fs::exists(io::fromUtf8(address))) {
        std::ifstream t(io::fromUtf8(address));
        std::string str((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        return str;
    }

    auto session = cpr::Session();
    session.SetUrl(cpr::Url{address});
    session.SetVerifySsl(verifySsl);
    session.SetTimeout(10000);

    auto response = session.Download(
        cpr::WriteCallback{[](const std::string_view&, intptr_t) -> bool { return true; }, 0});

    if (response.status_code != 200 && throwOnError) {
        throw ddb::NetException("Failed to fetch data from " + address);
    }

    return response.text;
}

cpr::Header authHeader(const std::string& token) {
    return cpr::Header{{"Authorization", "Bearer " + token}};
}

cpr::File uploadFile(const fs::path& file) {
    return cpr::File(io::toUtf8(file), io::toUtf8(file.filename()));
}

cpr::Header authCookie(const std::string& token) {
    return cpr::Header{{"Cookie", "jwtToken=" + token}};
}

}  // namespace ddb::utils
