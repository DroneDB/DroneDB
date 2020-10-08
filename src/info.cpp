/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "info.h"
#include "exceptions.h"

namespace ddb {

void info(const std::vector<std::string> &input, std::ostream &output,
          const std::string &format, bool recursive, int maxRecursionDepth, const std::string &geometry,
          bool withHash, bool stopOnError){
    std::vector<fs::path> filePaths;

    if (recursive){
        filePaths = getPathList(input, true, maxRecursionDepth);
    }else{
        filePaths = std::vector<fs::path>(input.begin(), input.end());
    }

    if (format == "json"){
        output << "[";
    }else if (format == "geojson"){
        output << R"<<<({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[)<<<";
    }else if (format == "text"){
        // Nothing
    }else{
        throw InvalidArgsException("Invalid format " + format);
    }


    bool first = true;

    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.string();

        Entry e;
        if (parseEntry(fp, "/", e, withHash, stopOnError)){
            // We override e.path because it's relative
            // But we want the absolute path (in the native path format)
            e.path = "file://" + fs::absolute(fp).string();

            if (format == "json"){
                json j;
                e.toJSON(j);
                if (!first) output << ",";
                output << j.dump();
            }else if (format == "geojson"){
                json j;
                if (e.toGeoJSON(j, ddb::getBasicGeometryTypeFromName(geometry))){
                    if (!first) output << ",";
                    output << j.dump();
                    j.clear();
                }else{
                    LOGD << "No geometries in " << fp.string() << ", skipping from GeoJSON export";
                }
            }else{
                output << e.toString() << "\n";
            }

            first = false;
        }else{
            LOGD << "Cannot parse " << fp.string() << ", skipping";
        }
    }

    if (format == "json"){
        output << "]";
    }else if (format == "geojson"){
        output << "]}";
    }
}

}
