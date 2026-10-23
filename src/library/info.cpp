/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "json.h"
#include "info.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb
{

    void info(const std::vector<std::string> &input, std::ostream &output,
              const std::string &format, bool recursive, int maxRecursionDepth, const std::string &geometry,
              bool withHash, bool stopOnError)
    {
        std::vector<fs::path> filePaths;

        if (recursive)
        {
            filePaths = getPathList(input, true, maxRecursionDepth);
        }
        else
        {
            filePaths = io::fromUtf8(input);
        }

        if (format == "json")
        {
            output << "[";
        }
        else if (format == "geojson")
        {
            output << R"<<<({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[)<<<";
        }
        else if (format == "text")
        {
            // Nothing
        }
        else
        {
            throw InvalidArgsException("Invalid format " + format);
        }

        bool first = true;

        for (auto &fp : filePaths)
        {
            LOGD << "Parsing entry " << io::toUtf8(fp);

            try
            {
                Entry e;
                parseEntry(fp, "/", e, withHash);
                // We override e.path because it's relative
                // But we want the absolute path (in the unix path format)
                e.path = "file://" + io::toGenericUtf8(fs::absolute(fp));

                if (format == "json")
                {
                    json j;
                    e.toJSON(j);
                    if (!first)
                        output << ",";
                    output << j.dump();
                }
                else if (format == "geojson")
                {
                    json j;
                    if (e.toGeoJSON(j, ddb::getBasicGeometryTypeFromName(geometry)))
                    {
                        if (!first)
                            output << ",";
                        output << j.dump();
                        j.clear();
                    }
                    else
                    {
                        LOGD << "No geometries in " << io::toUtf8(fp) << ", skipping from GeoJSON export";
                    }
                }
                else
                {
                    output << e.toString() << "\n";
                }

                first = false;
            }
            catch (const AppException &e)
            {
                LOGD << "Cannot parse " << io::toUtf8(fp) << ", skipping: " << e.what();
                if (stopOnError)
                    throw;
            }
        }

        if (format == "json")
        {
            output << "]";
        }
        else if (format == "geojson")
        {
            output << "]}";
        }
    }

}
