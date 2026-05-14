/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VECTOR_H
#define VECTOR_H

#include <string>
#include <vector>
#include "ddb_export.h"

namespace ddb
{

    /**
     * Build vector artifacts for a single source file.
     *
     * Produces TWO outputs under @p baseOutputPath:
     *   - <baseOutputPath>/mvt/{z}/{x}/{y}.pbf  (gzipped MVT tiles + metadata.json)
     *   - <baseOutputPath>/vec/source.gpkg      (GPKG with SPATIAL_INDEX=YES per layer)
     *
     * Both outputs are written atomically via a single sibling staging
     * directory; on partial failure originals are restored from a backup.
     * If BOTH `vec/` and `mvt/` already exist and @p overwrite is false the
     * build is skipped; if only one of the two exists they are BOTH
     * rebuilt so callers always observe a consistent pair.
     * Throws on failure; partial outputs are cleaned up.
     *
     * Multi-layer sources (GPKG, KMZ) preserve all layers in BOTH outputs.
     * Source SRS is reprojected to EPSG:4326 (GPKG) / EPSG:3857 (MVT).
     *
     * @throws BuildDepMissingException If sidecar files are missing or
     *                                  the GDAL build lacks the MVT driver.
     * @throws AppException             On unrecoverable conversion errors.
     */
    DDB_DLL void buildVector(const std::string &input,
                             const std::string &baseOutputPath,
                             bool overwrite = false);

    /**
     * Return the list of required sidecar dependencies for a vector input
     * (e.g. .shp → [".shx"]). Empty for self-contained formats.
     */
    DDB_DLL std::vector<std::string> getVectorDependencies(const std::string &input);

} // namespace ddb

#endif // VECTOR_H