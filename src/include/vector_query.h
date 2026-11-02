/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VECTOR_QUERY_H
#define VECTOR_QUERY_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddb
{
    /** Which features of a vector layer queryVector() / countVectorFeatures() select. */
    struct VectorQuery
    {
        std::string layerName;                      // empty = first layer
        std::optional<std::array<double, 4>> bbox;  // [minX, minY, maxX, maxY]; none = no filter
        std::string bboxSrs;                        // empty = "EPSG:4326"
        std::vector<int64_t> fids;                  // feature ids; empty = every feature
        std::string where;                          // attribute filter: an SQL expression over the
                                                    // layer's fields (SQLite syntax for GeoPackage),
                                                    // built by the caller; empty = none
        int maxFeatures = 0;                        // 0 = unbounded (query only)
        int startIndex = 0;                         // 0-based offset (query only)
        std::string outputFormat;                   // "geojson" (default, also "json") | "gml" (query only)
    };

    /**
     * Features of a vector layer, reprojected to EPSG:4326 when the layer has an SRS.
     * Feature ids are the source FIDs (GML: gml:id="<layer>.<fid>"), so the same feature
     * keeps its id in every page and in id lookups. With fids set, a GeoPackage reads
     * them by primary key.
     */
    std::string queryVector(const std::string &vectorPath, const VectorQuery &query);

    /** Number of features matching query's layer, bbox, fids and where (paging is ignored). */
    int64_t countVectorFeatures(const std::string &vectorPath, const VectorQuery &query);
    /**
     * Describe a vector dataset: layers, geometry types, SRS, extent,
     * feature count and fields. Returns JSON.
     */
    std::string describeVector(const std::string &vectorPath,
                               const std::string &layerName);

} // namespace ddb

#endif // VECTOR_QUERY_H
