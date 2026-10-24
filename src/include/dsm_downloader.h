/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DSM_DOWNLOADER_H
#define DSM_DOWNLOADER_H

// Downloads the DSM around a point from a DSM service (an OpenTopography URL template), through
// an injected IHttpClient: no cache, no GDAL, so a test answers from a fake.

#include <optional>
#include <string>

#include "ddb_export.h"
#include "fs.h"
#include "http_client.h"

// Fills the {west}, {south}, {east}, {north} and {key} placeholders of a DSM service URL
// template. Returns nothing when apiKey is empty: the service refuses keyless requests.
DDB_DLL std::optional<std::string> buildDsmServiceUrl(const std::string &urlTemplate,
                                                      const std::string &apiKey,
                                                      double west, double south,
                                                      double east, double north);

// The DSM service API key from DSM_SERVICE_KEY_ENV, read at call time and trimmed ("" when unset or blank)
DDB_DLL std::string dsmServiceApiKey();

namespace ddb
{

    // What to download: a square of 10 km around the point
    struct DsmDownload
    {
        double latitude = 0;
        double longitude = 0;
        std::string urlTemplate; // see buildDsmServiceUrl
        std::string apiKey;
        fs::path folder;         // the file is created here
    };

    class DsmDownloader
    {
        IHttpClient &client;

    public:
        explicit DsmDownloader(IHttpClient &client) : client(client) {}

        // Downloads the DSM into a new file of request.folder and returns its path. Throws
        // NetException when there is no key (nothing is requested), the transfer fails, or the
        // file is empty; the key never appears in a message.
        DDB_DLL std::string download(const DsmDownload &request) const;
    };

} // namespace ddb

#endif // DSM_DOWNLOADER_H
