/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "chunkeduploadclient.h"

#include "dbops.h"
#include "fs.h"
#include "mio.h"
#include "registry.h"
#include "registryutils.h"
#include "userprofile.h"
#include "utils.h"

namespace ddb {

const int MAX_RETRIES = 10;

ChunkedUploadClient::ChunkedUploadClient(ddb::Registry* registry,
                                         ddb::ShareClient* shareClient) {
    this->registry = registry;
    this->shareClient = shareClient;
}

DDB_DLL int ChunkedUploadClient::StartSession(int chunks, size_t size) {
    const auto token = this->shareClient->getToken();

    if (token.empty())
        throw InvalidArgsException("Missing token, call Init first");

    if (chunks < 1) throw InvalidArgsException("Chunks cannot be less than 1");
    if (size <= 0) throw InvalidArgsException("Invalid size");

    // Commit
    int retryNum = 0;
    while (true) {
        try {
            this->registry->ensureTokenValidity();

            auto res =
                net::POST(this->registry->getUrl("/share/upload/" + token +
                                                 "/session"))
                    .multiPartFormData({"chunks", std::to_string(chunks)},
                                       {"size", std::to_string(size)})
                    .authToken(this->registry->getAuthToken())
                    .send();

            if (res.status() != 200) this->registry->handleError(res);

            auto j = res.getJSON();
            if (!j.contains("sessionId")) this->registry->handleError(res);

            return j["sessionId"].get<int>();

        } catch (const NetException& e) {
            if (++retryNum >= MAX_RETRIES) throw e;
            LOGD << e.what() << ", retrying start upload session (attempt "
                 << retryNum << ")";
            utils::sleep(1000 * retryNum);
        }
    }
}

/*
 * Add function to support streams (curl_mimepart)
 * Reference: https://curl.se/libcurl/c/curl_mime_data_cb.html
 *
 */
DDB_DLL void ChunkedUploadClient::UploadToSession(int index,
                                                  std::istream input) {
    return;
}

DDB_DLL void ChunkedUploadClient::CloseSession(const fs::path& filePath,
                                               std::string& path) {
    const auto token = this->shareClient->getToken();

    if (token.empty())
        throw InvalidArgsException("Missing token, call Init first");

    if (path.empty()) throw InvalidArgsException("Missing path");
    if (filePath.empty()) throw InvalidArgsException("Missing file path");

    // Commit
    int retryNum = 0;
    while (true) {
        try {
            this->registry->ensureTokenValidity();

            auto res = net::POST(this->registry->getUrl(
                                     "/share/upload/" + token + "session/" +
                                     std::to_string(sessionId) + "/close"))
                           .multiPartFormData({"path", path})
                           .authToken(this->registry->getAuthToken())
                           .send();

            if (res.status() != 200) this->registry->handleError(res);

            json j = res.getJSON();

            if (!j.contains("hash")) this->registry->handleError(res);

            std::string sha256 = Hash::fileSHA256(filePath.string());
            std::string filename = filePath.filename().string();

            if (sha256 != j["hash"])
                throw NetException(
                    filename +
                    " file got corrupted during upload (hash mismatch, "
                    "expected: " +
                    sha256 + ", got: " + j["hash"].get<std::string>() +
                    ". Try again.");

            break;  // Done

        } catch (const NetException& e) {
            if (++retryNum >= MAX_RETRIES) throw e;
            LOGD << e.what() << ", retrying close upload session (attempt "
                 << retryNum << ")";
            utils::sleep(1000 * retryNum);
        }
    }
}

int ChunkedUploadClient::getSessionId() const { return this->sessionId; }

}  // namespace ddb
