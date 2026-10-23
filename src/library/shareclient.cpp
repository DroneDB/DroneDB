/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareclient.h"

#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{

    // The whole transfer of one file must end within this (it has always been the share's limit)
    constexpr std::chrono::milliseconds ShareUploadTimeout{10000};

    ShareClient::ShareClient(RegistryClient *client) : registry(client)
    {
    }

    void ShareClient::Init(const std::string &tag, const std::string &password,
                           const std::string &datasetName,
                           const std::string &datasetDescription)
    {
        // The password of the share stays out of the log
        LOGD << "Init('" << tag << "', '" << datasetName << "', '" << datasetDescription << "')";

        const auto res = registry->postForm("/share/init", {{"tag", tag}, {"password", password}},
                                            AuthScheme::Bearer);
        registry->expectOk(res);

        json j = json::parse(res.body);
        if (!j.contains("token"))
            registry->fail(res);
        this->token = j["token"];
        LOGD << "Token = " << this->token;
    }

    void ShareClient::Upload(const std::string &path, const fs::path &filePath,
                             const UploadCallback &cb)
    {
        if (token.empty())
            throw InvalidArgsException("Missing token, call Init first");

        std::string filename = io::toUtf8(filePath.filename());

        LOGD << "Uploading " << io::toUtf8(filePath);

        // Not tried again: an upload may have reached the server when the answer was lost
        const auto res = registry->postMultipart(
            "/share/upload/" + this->token,
            {HttpPart::fromFile("file", filePath), HttpPart::text("path", path)}, AuthScheme::Bearer,
            ShareUploadTimeout,
            [&cb, &filename](const HttpProgress &progress) -> bool
            {
                if (cb == nullptr)
                    return true;
                return cb(filename, progress.uploadNow, progress.uploadTotal);
            });
        registry->expectOk(res);

        json j = json::parse(res.body);

        if (!j.contains("hash"))
            registry->fail(res);
    }

    std::string ShareClient::Commit()
    {
        if (token.empty())
            throw InvalidArgsException("Missing token, call Init first");

        const auto res = registry->postForm("/share/commit/" + this->token, {}, AuthScheme::Bearer);
        registry->expectOk(res);

        json j = json::parse(res.body);
        if (!j.contains("url"))
            registry->fail(res);

        this->resultUrl = registry->getUrl(j["url"]);

        return std::string(this->resultUrl);
    }

    std::string ShareClient::getToken() const { return std::string(this->token); }
    std::string ShareClient::getResultUrl() const
    {
        return std::string(this->resultUrl);
    }

} // namespace ddb
