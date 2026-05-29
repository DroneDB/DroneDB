/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VECTOR_QUERY_H
#define VECTOR_QUERY_H

#include <string>
#include "ddb_export.h"

namespace ddb
{
    /**
     * Query a vector dataset, returning features as a string.
     *
     * @param vectorPath   Path to vector (e.g. a GeoPackage from build/).
     * @param layerName    Empty = first layer.
     * @param bbox         [minX, minY, maxX, maxY] or nullptr for no filter.
     * @param bboxSrs      Authority code; empty = "EPSG:4326".
     * @param maxFeatures  Max features to return (0 = unbounded; caller validates).
     * @param startIndex   Pagination offset (0-based).
     * @param outputFormat "geojson" | "gml" | "json" (== geojson). Empty = "geojson".
     * @return Serialized features in requested format.
     */
    DDB_DLL std::string queryVector(const std::string &vectorPath,
                                    const std::string &layerName,
                                    const double *bbox,
                                    const std::string &bboxSrs,
                                    int maxFeatures,
                                    int startIndex,
                                    const std::string &outputFormat);

    /**
     * Describe a vector dataset: layers, geometry types, SRS, extent,
     * feature count and fields. Returns JSON.
     */
    DDB_DLL std::string describeVector(const std::string &vectorPath,
                                       const std::string &layerName);

} // namespace ddb

#endif // VECTOR_QUERY_H
