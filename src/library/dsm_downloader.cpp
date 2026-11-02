/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dsm_downloader.h"

#include <chrono>
#include <cstdlib>
#include <mutex>

#include "constants.h"
#include "exceptions.h"
#include "geo.h"
#include "http_helpers.h"
#include "logger.h"
#include "mathutil.h"
#include "mio.h"
#include "stringutil.h"

using namespace ddb;

std::optional<std::string> buildDsmServiceUrl(const std::string &urlTemplate,
                                              const std::string &apiKey,
                                              double west, double south,
                                              double east, double north)
{
    if (apiKey.empty())
        return std::nullopt;

    std::string url = urlTemplate;
    str::replaceAll(url, "{west}", std::to_string(west));
    str::replaceAll(url, "{south}", std::to_string(south));
    str::replaceAll(url, "{east}", std::to_string(east));
    str::replaceAll(url, "{north}", std::to_string(north));
    str::replaceAll(url, "{key}", apiKey);
    return url;
}

std::string dsmServiceApiKey()
{
    const char *value = std::getenv(DSM_SERVICE_KEY_ENV);
    std::string key = value != nullptr ? std::string(value) : std::string();
    // A key pasted with a trailing newline or spaces would end up in the URL
    str::trim(key);
    return key;
}

std::string DsmDownloader::download(const DsmDownload &request) const
{
    // Estimate bounds around point by a certain radius
    double radius = 5000.0; // meters

    UTMZone z = getUTMZone(request.latitude, request.longitude);
    Projected2D p = toUTM(request.latitude, request.longitude, z);

    Geographic2D max = fromUTM(Projected2D(p.x + radius, p.y + radius), z);
    Geographic2D min = fromUTM(Projected2D(p.x - radius, p.y - radius), z);

    const auto url = buildDsmServiceUrl(request.urlTemplate, request.apiKey,
                                        min.longitude, min.latitude, max.longitude, max.latitude);
    if (!url)
    {
        // Offline fallback: getAltitude() catches the NetException and returns 0. Warn once,
        // not for every image whose relative altitude is estimated.
        static std::once_flag warned;
        std::call_once(warned, [] {
            LOGW << "DSM download skipped: set " DSM_SERVICE_KEY_ENV
                    " to an OpenTopography API key to estimate relative altitudes from a world DSM";
        });
        throw NetException("No DSM service API key (" DSM_SERVICE_KEY_ENV " is not set)");
    }

    // Try to download
    std::string filename = std::to_string(static_cast<int>(p.x)) + "_" +
                           std::to_string(static_cast<int>(p.y)) + "_" +
                           std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".tif";
    std::string filePath = io::toUtf8(request.folder / io::fromUtf8(filename));

    // The key stays out of the log, and out of the error message that getAltitude() logs
    const std::string redactedUrl =
        *buildDsmServiceUrl(request.urlTemplate, "<redacted>", min.longitude, min.latitude, max.longitude, max.latitude);
    LOGD << "Downloading DSM from " << redactedUrl << " ...";

    try
    {
        utils::downloadToFile(client_, *url, filePath, true);
    }
    catch (const NetException &e)
    {
        std::string message = e.what();
        str::replaceAll(message, *url, redactedUrl);
        throw NetException(message);
    }

    // Verify the downloaded file exists and has content
    if (!fs::exists(io::fromUtf8(filePath)) || fs::file_size(io::fromUtf8(filePath)) == 0) {
        throw NetException("Downloaded DSM file is empty or does not exist: " + filePath);
    }

    return filePath;
}
