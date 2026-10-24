/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The dataset endpoints of RegistryClient: what pull and clone read from a Registry.
#include <chrono>
#include <fstream>

#include "exceptions.h"
#include "http_client.h"
#include "mio.h"
#include "mzip.h"
#include "randomutil.h"
#include "registry_client.h"
#include "stringutil.h"

namespace ddb {

namespace {

std::string datasetPath(const std::string &organization, const std::string &dataset) {
    return "/orgs/" + organization + "/ds/" + dataset;
}

// The progress line of a download: bytes so far and the speed since the last line, at most once
// a second
HttpProgressCallback downloadProgress(std::ostream &out) {
    auto start = std::chrono::system_clock::now();
    size_t prevBytes = 0;

    return [start, prevBytes, &out](const HttpProgress &progress) mutable -> bool {
        const size_t txBytes = progress.downloadNow;
        if (txBytes == prevBytes)
            return true;

        const auto now = std::chrono::system_clock::now();

        const std::chrono::duration<double> dT = now - start;

        if (dT.count() < 1)
            return true;

        const auto dData = txBytes - prevBytes;
        const auto speed = static_cast<std::uintmax_t>(static_cast<double>(dData) / dT.count());

        out << "Downloading " << io::bytesToHuman(txBytes) << " @ " << io::bytesToHuman(speed)
            << "/s\t\t\r";
        out.flush();

        prevBytes = txBytes;
        start = now;

        return true;
    };
}

// A download that failed wrote the body of the error to its file, not to the response: the
// message the Registry gave is read from there (the first 64 KB are enough for any error body)
// and the file, which holds no data, is removed
HttpResponse withErrorBodyOf(HttpResponse res, const fs::path &file) {
    if (res.ok() || res.hasTransportError())
        return res;

    std::ifstream in(file, std::ios::binary);
    std::string body(64 * 1024, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<size_t>(in.gcount()));
    res.body = body;
    in.close();
    io::assureIsRemoved(file);
    return res;
}

}  // namespace

Entry RegistryClient::getDatasetInfo(const std::string &organization, const std::string &dataset) {
    const auto res = get(datasetPath(organization, dataset), AuthScheme::Cookie);

    if (res.status == 404)
        throw RegistryNotFoundException("Dataset not found");

    expectOk(res);

    const json j = json::parse(res.body);

    if (j.empty())
        throw RegistryException("Invalid empty response from registry");

    return j[0].get<Entry>();
}

void RegistryClient::downloadDdb(const std::string &organization, const std::string &dataset,
                                 const std::string &folder) {
    const auto res = get(datasetPath(organization, dataset) + "/ddb", AuthScheme::Cookie);
    expectOk(res);

    // The body is the zip: a binary buffer, read with its size
    zip::extractAllFromBuffer(res.body.c_str(), res.body.size(), folder);
}

json RegistryClient::getStamp(const std::string &organization, const std::string &dataset) {
    const auto res = get(datasetPath(organization, dataset) + "/stamp", AuthScheme::Cookie);
    expectOk(res);

    // Quick sanity check (a discarded parse is not an object)
    json stamp = json::parse(res.body, nullptr, false);
    if (stamp.is_object() && stamp.contains("checksum")) {
        return stamp;
    } else {
        throw RegistryException("Invalid stamp: " + res.body);
    }
}

void RegistryClient::downloadFiles(const std::string &organization, const std::string &dataset,
                                   const std::vector<std::string> &files, const std::string &folder,
                                   std::ostream &out) {
    if (files.empty())
        return;

    auto downloadPath = datasetPath(organization, dataset) + "/download";

    if (files.size() == 1) {
        // A query value: a name with &, #, %, + or a space would end, cut or change the value
        downloadPath += "?path=" + urlEncode(files[0]);

        const auto destPath = io::fromUtf8(folder) / io::fromUtf8(files[0]);

        io::createDirectories(destPath.parent_path());

        HttpResponse res;
        {
            std::ofstream of(destPath, std::ios::binary);
            res = download(downloadPath, AuthScheme::Cookie, of, downloadProgress(out));
        }

        expectOk(withErrorBodyOf(res, destPath));
    } else {
        const auto tempFile = (io::fromUtf8(folder) / (randomString(8) + ".tmp"));
        io::assureFolderExists(tempFile.parent_path());

        // Joins path list
        const auto paths = str::join(files);

        HttpResponse res;
        {
            std::ofstream of(tempFile, std::ios::binary);
            res = download(downloadPath, AuthScheme::Cookie, of, nullptr, {{"path", paths}});
        }

        expectOk(withErrorBodyOf(res, tempFile));

        try {
            zip::extractAll(io::toUtf8(tempFile), folder);
            io::assureIsRemoved(tempFile);
        } catch (const std::runtime_error &e) {
            io::assureIsRemoved(tempFile);
            throw AppException(e.what());
        }
    }
}

json RegistryClient::getMetaDump(const std::string &organization, const std::string &dataset,
                                 const std::vector<std::string> &ids) {
    // A read: the dump of metadata is asked for with a POST because the ids do not fit a URL
    const auto res = postForm(datasetPath(organization, dataset) + "/meta/dump", {{"ids", json(ids).dump()}},
                              AuthScheme::Cookie, true);
    expectOk(res);

    // Quick sanity check (a discarded parse is not an array)
    json metaDump = json::parse(res.body, nullptr, false);
    if (metaDump.is_array()) {
        return metaDump;
    } else {
        throw RegistryException("Invalid meta dump: " + res.body);
    }
}

}  // namespace ddb
