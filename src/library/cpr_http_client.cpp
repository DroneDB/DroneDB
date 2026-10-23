/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cpr_http_client.h"

#include <algorithm>
#include <cctype>
#include <ostream>
#include <vector>

#include <cpr/cpr.h>

#include "mio.h"

namespace ddb {

std::string HttpResponse::header(const std::string &name) const {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = headers.find(key);
    return it == headers.end() ? std::string() : it->second;
}

namespace {

// What cpr reports as "the server could not be reached" (status 0 and an error)
HttpTransportError transportOf(const cpr::Error &error) {
    switch (error.code) {
        case cpr::ErrorCode::OK:
            return HttpTransportError::None;
        case cpr::ErrorCode::COULDNT_CONNECT:
        case cpr::ErrorCode::COULDNT_RESOLVE_HOST:
        case cpr::ErrorCode::COULDNT_RESOLVE_PROXY:
            return HttpTransportError::CouldNotConnect;
        default:
            return HttpTransportError::Other;
    }
}

HttpResponse toResponse(const cpr::Response &res) {
    HttpResponse out;
    out.status = res.status_code;
    out.body = res.text;
    for (const auto &[name, value] : res.header) {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out.headers[key] = value;
    }
    out.transport = transportOf(res.error);
    if (out.hasTransportError())
        out.transportMessage = res.error.message;
    return out;
}

// Applies the settings every request has: the url, the headers, whether the certificate is checked, the
// timeout of the transfer and the progress callback
void configure(cpr::Session &session, const HttpRequest &request) {
    session.SetUrl(cpr::Url{request.url});
    session.SetVerifySsl(cpr::VerifySsl(request.verifySsl));

    if (!request.headers.empty()) {
        cpr::Header headers;
        for (const auto &[name, value] : request.headers)
            headers[name] = value;
        session.SetHeader(headers);
    }

    if (request.timeout.count() > 0)
        session.SetTimeout(cpr::Timeout{static_cast<int32_t>(request.timeout.count())});

    if (request.progress) {
        const auto &progress = request.progress;
        session.SetProgressCallback(cpr::ProgressCallback(
            [&progress](size_t downloadTotal, size_t downloadNow, size_t uploadTotal, size_t uploadNow,
                        intptr_t) -> bool {
                return progress(HttpProgress{downloadTotal, downloadNow, uploadTotal, uploadNow});
            }));
    }
}

cpr::Payload toPayload(const HttpFields &form) {
    std::vector<cpr::Pair> pairs;
    pairs.reserve(form.size());
    for (const auto &[name, value] : form)
        pairs.emplace_back(name, value);
    return cpr::Payload(pairs.begin(), pairs.end());
}

}  // namespace

HttpResponse CprHttpClient::get(const HttpRequest &request) {
    cpr::Session session;
    configure(session, request);
    return toResponse(session.Get());
}

HttpResponse CprHttpClient::post(const HttpRequest &request) {
    cpr::Session session;
    configure(session, request);
    if (!request.form.empty())
        session.SetPayload(toPayload(request.form));
    return toResponse(session.Post());
}

HttpResponse CprHttpClient::multipart(const HttpRequest &request) {
    cpr::Session session;
    configure(session, request);

    cpr::Multipart multipart{};
    for (const auto &part : request.parts) {
        if (part.file.empty())
            multipart.parts.emplace_back(part.name, part.value);
        else
            // The file name goes in UTF-8: cpr would build an fs::path from the std::string, the
            // ANSI code page on Windows
            multipart.parts.emplace_back(
                part.name, cpr::File(io::toUtf8(part.file), io::toUtf8(part.file.filename())));
    }
    session.SetMultipart(multipart);
    return toResponse(session.Post());
}

HttpResponse CprHttpClient::download(const HttpRequest &request, std::ostream &sink) {
    cpr::Session session;
    configure(session, request);
    const cpr::WriteCallback write([&sink](const std::string_view &data, intptr_t) -> bool {
        sink.write(data.data(), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(sink);
    });

    if (request.form.empty())
        return toResponse(session.Download(write));

    session.SetPayload(toPayload(request.form));
    session.SetWriteCallback(write);
    return toResponse(session.Post());
}

IHttpClient &defaultHttpClient() {
    static CprHttpClient client;
    return client;
}

std::string urlEncode(std::string_view text) {
    return std::string(cpr::util::urlEncode(text));
}

}  // namespace ddb
