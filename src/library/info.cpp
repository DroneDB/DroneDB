/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "json.h"
#include "info.h"
#include "exceptions.h"
#include "mio.h"

#include <optional>

namespace ddb
{

    namespace
    {
        // The GeoJSON feature collection wraps the features; entries without a geometry are left out
        constexpr const char *kFeatureCollectionOpen =
            R"<<<({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[)<<<";
        constexpr const char *kFeatureCollectionClose = "]}";
    } // namespace

    void info(const std::vector<std::string> &input, std::ostream &output, const InfoOptions &options)
    {
        const OutputFormat format = options.format;
        std::vector<fs::path> filePaths;

        if (options.recursive)
        {
            filePaths = getPathList(input, true, options.maxRecursionDepth);
        }
        else
        {
            filePaths = io::fromUtf8(input);
        }

        // Text is written entry by entry; the JSON formats are arrays of members
        std::optional<JsonArrayWriter> writer;
        if (format == OutputFormat::Json)
            writer.emplace(output);
        else if (format == OutputFormat::GeoJson)
            writer.emplace(output, kFeatureCollectionOpen, kFeatureCollectionClose);

        for (auto &fp : filePaths)
        {
            LOGD << "Parsing entry " << io::toUtf8(fp);

            try
            {
                Entry e;
                parseEntry(fp, "/", e, options.withHash);
                // We override e.path because it's relative
                // But we want the absolute path (in the unix path format)
                e.path = "file://" + io::toGenericUtf8(fs::absolute(fp));

                switch (format)
                {
                case OutputFormat::Json:
                    writer->write(e);
                    break;
                case OutputFormat::GeoJson:
                    if (const auto feature = e.toGeoJSON(ddb::getBasicGeometryTypeFromName(options.geometry)))
                        writer->write(*feature);
                    else
                        LOGD << "No geometries in " << io::toUtf8(fp) << ", skipping from GeoJSON export";
                    break;
                case OutputFormat::Text:
                    output << e.toString() << "\n";
                    break;
                }
            }
            catch (const AppException &e)
            {
                LOGD << "Cannot parse " << io::toUtf8(fp) << ", skipping: " << e.what();
                if (options.stopOnError)
                    throw;
            }
        }

        if (writer)
            writer->finish();
    }

}
