/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "ddb.h"
#include "info.h"
#include "../classes/exceptions.h"

namespace ddb {

using json = nlohmann::json;

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
        if (entry::parseEntry(fp, "/", e, opts.peOpts)){
            e.path = "file:///" + e.path;

            if (opts.format == "json"){
                json j;
                e.toJSON(j);
                if (!first) output << ",";
                output << j.dump();
            }else if (opts.format == "geojson"){
                json j;
                if (e.toGeoJSON(j)){
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
