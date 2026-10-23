/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "http_helpers.h"

#include <fstream>
#include <string>

#include "exceptions.h"
#include "mio.h"

namespace ddb::utils {

HttpResponse downloadToFile(IHttpClient& client,
                            const std::string& url,
                            const std::string& filePath,
                            bool throwOnError,
                            bool verifySsl) {
    HttpRequest request;
    request.url = url;
    request.verifySsl = verifySsl;
    request.timeout = std::chrono::milliseconds(15000);
    request.headers = {{"Accept-Encoding", "gzip"}};

    if (fs::exists(io::fromUtf8(filePath)))
        fs::remove(io::fromUtf8(filePath));

    std::ofstream out(io::fromUtf8(filePath), std::ios::binary);

    auto response = client.download(request, out);
    out.close(); // Ensure file is closed before checking size

    // Check if download was successful and file has content
    if (response.status != 200 || !fs::exists(io::fromUtf8(filePath)) || fs::file_size(io::fromUtf8(filePath)) == 0) {
        // Remove empty or failed download file
        if (fs::exists(io::fromUtf8(filePath)))
            fs::remove(io::fromUtf8(filePath));

        if (throwOnError)
            throw ddb::NetException("Failed to fetch data from " + url + " (status: " + std::to_string(response.status) + ")");
    }

    return response;
}

HttpResponse downloadToFile(const std::string& url,
                            const std::string& filePath,
                            bool throwOnError,
                            bool verifySsl) {
    return downloadToFile(defaultHttpClient(), url, filePath, throwOnError, verifySsl);
}

}  // namespace ddb::utils
