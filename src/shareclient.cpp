/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareclient.h"

#include "dbops.h"
#include "fs.h"
#include "mio.h"
#include "registry.h"
#include "registryutils.h"
#include "userprofile.h"
#include "utils.h"

namespace ddb {

const int MAX_RETRIES = 10;

ShareClient::ShareClient(ddb::Registry* registry) : registry(registry) {
    maxUploadSize = 0;
}

void ShareClient::Init(const std::string& tag, const std::string& password,
                       const std::string& datasetName,
                       const std::string& datasetDescription) {
    // if (tag.empty()) throw InvalidArgsException("Missing tag parameter");

    LOGD << "Init('" << tag << "', '" << password << "', '" << datasetName
         << "', '" << datasetDescription << "')";

    this->registry->ensureTokenValidity();

    net::Response res = net::POST(this->registry->getUrl("/share/init"))
                            .formData({"tag", tag, "password", password})
                            .authToken(this->registry->getAuthToken())
                            .send();

    if (res.status() != 200) this->registry->handleError(res);

    json j = res.getJSON();
    if (!j.contains("token")) this->registry->handleError(res);
    this->token = j["token"];
    LOGD << "Token = " << this->token;

    this->maxUploadSize = j.contains("maxUploadChunkSize")
                              ? j["maxUploadChunkSize"].get<size_t>()
                              : static_cast<size_t>(LONG_MAX);
    LOGD << "MaxUploadChunkSize = " << maxUploadSize;
}

void ShareClient::Upload(const std::string& path, const fs::path& filePath,
                         const UploadCallback& cb) {
    if (token.empty())
        throw InvalidArgsException("Missing token, call Init first");

    io::Path p = io::Path(filePath);
    size_t filesize = p.getSize();

    std::string filename = filePath.filename().string();

    size_t gTotalBytes = p.getSize();
    size_t gTxBytes = 0;

    LOGD << "Uploading " << p.string();

    int retryNum = 0;

    while (true) {
        try {
            this->registry->ensureTokenValidity();

            net::Response res =
                net::POST(
                    this->registry->getUrl("/share/upload/" + this->token))
                    .multiPartFormData({"file", filePath.string()},
                                       {"path", path})
                    .authToken(this->registry->getAuthToken())
                    .progressCb([&cb, &filename, &filesize, &gTotalBytes,
                                 &gTxBytes](size_t txBytes, size_t totalBytes) {
                        if (cb == nullptr) return true;

                        gTxBytes += txBytes;

                        // Safe cap
                        gTxBytes = std::min(filesize, txBytes);

                        return cb(filename, gTxBytes, gTotalBytes);
                    })
                    //.maximumUploadSpeed(1024*1024)
                    .send();

            if (res.status() != 200) this->registry->handleError(res);

            json j = res.getJSON();

            if (!j.contains("hash")) this->registry->handleError(res);

            break;  // Done
        } catch (const NetException& e) {
            if (++retryNum >= MAX_RETRIES) throw e;

            LOGD << e.what() << ", retrying upload of " << filename
                 << " (attempt " << retryNum << ")";
            utils::sleep(1000 * retryNum);
        }
    }
}

std::string ShareClient::Commit() {
    if (token.empty())
        throw InvalidArgsException("Missing token, call Init first");

    // Commit
    int retryNum = 0;
    while (true) {
        try {
            this->registry->ensureTokenValidity();

            auto res = net::POST(this->registry->getUrl("/share/commit/" +
                                                        this->token))
                           .authToken(this->registry->getAuthToken())
                           .send();

            if (res.status() != 200) this->registry->handleError(res);

            auto j = res.getJSON();
            if (!j.contains("url")) this->registry->handleError(res);

            this->resultUrl = this->registry->getUrl(j["url"]);

            return std::string(this->resultUrl);  // Done
        } catch (const NetException& e) {
            if (++retryNum >= MAX_RETRIES) throw e;
            LOGD << e.what() << ", retrying commit (attempt " << retryNum
                 << ")";
            utils::sleep(1000 * retryNum);
        }
    }
}

std::string ShareClient::getToken() const { return std::string(this->token); }
std::string ShareClient::getResultUrl() const {
    return std::string(this->resultUrl);
}
size_t ShareClient::getMaxUploadSize() const { return this->maxUploadSize; }

}  // namespace ddb
