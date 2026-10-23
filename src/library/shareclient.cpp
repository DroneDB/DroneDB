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
#include "http_helpers.h"
#include <cpr/cpr.h>

namespace ddb
{

    ShareClient::ShareClient(ddb::Registry *registry) : registry(registry)
    {
    }

    void ShareClient::Init(const std::string &tag, const std::string &password,
                           const std::string &datasetName,
                           const std::string &datasetDescription)
    {
        // if (tag.empty()) throw InvalidArgsException("Missing tag parameter");

        LOGD << "Init('" << tag << "', '" << password << "', '" << datasetName
             << "', '" << datasetDescription << "')";

        this->registry->ensureTokenValidity();

        cpr::Response res = cpr::Post(cpr::Url(this->registry->getUrl("/share/init")),
                                      utils::authHeader(this->registry->getAuthToken()),
                                      cpr::Payload{{"tag", tag}, {"password", password}},
                                      cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);

        json j = json::parse(res.text);
        if (!j.contains("token"))
            this->registry->handleError(res);
        this->token = j["token"];
        LOGD << "Token = " << this->token;
    }

    void ShareClient::Upload(const std::string &path, const fs::path &filePath,
                             const utils::UploadCallback &cb)
    {
        if (token.empty())
            throw InvalidArgsException("Missing token, call Init first");

        io::Path p = io::Path(filePath);
        size_t filesize = p.getSize();

        std::string filename = io::toUtf8(filePath.filename());

        // size_t gTotalBytes = p.getSize();
        // size_t gTxBytes = 0;

        LOGD << "Uploading " << io::toUtf8(filePath);

        // No retry loop here: the old one retried NetException only, which nothing on this
        // path throws (handleError raises AuthException/RegistryException and cpr transport
        // errors are not converted), so it never ran. Retries for transport errors, 5xx and
        // 429 belong in the RegistryClient + RetryPolicy seam (DQ-11).
        this->registry->ensureTokenValidity();

        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/share/upload/" + this->token)),
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::Multipart{{cpr::Part("file", utils::uploadFile(filePath)), cpr::Part("path", path)}},
                             cpr::Timeout{10000},
                             cpr::ProgressCallback([&cb, &filename](size_t, size_t, size_t uploadTotal, size_t uploadNow, intptr_t) -> bool
                                                   {
                if (cb == nullptr) return true;
                return cb(filename, uploadNow, uploadTotal); }),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);

        json j = json::parse(res.text);

        if (!j.contains("hash"))
            this->registry->handleError(res);
    }

    std::string ShareClient::Commit()
    {
        if (token.empty())
            throw InvalidArgsException("Missing token, call Init first");

        // No retry loop: see Upload (DQ-11)
        this->registry->ensureTokenValidity();

        auto res = cpr::Post(cpr::Url(this->registry->getUrl("/share/commit/" + this->token)),
                             utils::authHeader(this->registry->getAuthToken()),
                             cpr::VerifySsl(this->registry->getSslVerify()));

        if (res.status_code != 200)
            this->registry->handleError(res);

        json j = json::parse(res.text);
        if (!j.contains("url"))
            this->registry->handleError(res);

        this->resultUrl = this->registry->getUrl(j["url"]);

        return std::string(this->resultUrl);
    }

    std::string ShareClient::getToken() const { return std::string(this->token); }
    std::string ShareClient::getResultUrl() const
    {
        return std::string(this->resultUrl);
    }

} // namespace ddb
