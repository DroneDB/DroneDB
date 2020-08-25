/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "info.h"
#include "exceptions.h"

namespace ddb {

void parseFiles(const std::vector<std::string> &input, std::ostream &output, ParseFilesOpts &opts){
    std::vector<fs::path> filePaths;

    if (opts.recursive){
        filePaths = getPathList(input, true, opts.maxRecursionDepth);
    }else{
        filePaths = std::vector<fs::path>(input.begin(), input.end());
    }

    if (opts.format == "json"){
        output << "[";
    }else if (opts.format == "geojson"){
        output << R"<<<({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[)<<<";
    }else if (opts.format == "text"){
        // Nothing
    }else{
        throw InvalidArgsException("Invalid format " + opts.format);
    }


    bool first = true;

    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.string();

        Entry e;
        if (parseEntry(fp, "/", e, opts.peOpts)){
            e.path = "file://" + e.path;

            if (opts.format == "json"){
                json j;
                e.toJSON(j);
                if (!first) output << ",";
                output << j.dump();
            }else if (opts.format == "geojson"){
                json j;
                if (e.toGeoJSON(j, opts.geometry)){
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

    if (opts.format == "json"){
        output << "]";
    }else if (opts.format == "geojson"){
        output << "]}";
    }
}

}
