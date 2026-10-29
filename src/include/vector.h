/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VECTOR_H
#define VECTOR_H

#include <string>
#include <vector>

#include "build_budget.h"

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
     * The source is read by the raster helper (raster_helper.h): it builds both outputs in a
     * private staging folder next to them, and this function publishes the folder. Whether the
     * build is up to date is told by the outputs it finds (vec/source.gpkg and mvt/metadata.json,
     * or vec/local.json for a drawing without a coordinate system).
     *
     * Multi-layer sources (GPKG, KMZ, GPX) keep every layer that holds a
     * feature in BOTH outputs; empty layers are left out (all of them are
     * kept when every layer is empty).
     * Source SRS is reprojected to EPSG:4326 (GPKG) / EPSG:3857 (MVT).
     *
     * CAD drawings (.dxf, .dwg): a DWG is converted with LibreDWG dwg2dxf
     * (cad.h); the CRS comes from cad::resolveCadCrs (layer SRS, same-name
     * .prj, GEODATA, lon/lat extent). A drawing without a CRS is "local":
     * only vec/source.gpkg (drawing coordinates, no SRS) and vec/local.json
     * (units, extent) are written, no mvt/, and an older mvt/ is removed.
     *
     * The threads of the build (the thread pool of the MVT writer) take tokens of the CPU pool
     * of this process for as long as the helper works: the lower of the cap of @p options and
     * the DDB_BUILD_THREADS budget (BuildResourceBudget), as many as the pool gives.
     *
     * @throws BuildDepMissingException If sidecar files are missing (names are
     *                                  relative to the input's folder).
     * @throws AppException             On unrecoverable conversion errors,
     *                                  including a GDAL build without the
     *                                  MVT driver.
     */
    void buildVector(const std::string &input,
                     const std::string &baseOutputPath,
                     bool overwrite = false,
                     const BuildOptions &options = {});

    /**
     * Return the list of required sidecar dependencies for a vector input
     * (e.g. .shp → [".shx"]). Empty for self-contained formats.
     */
    std::vector<std::string> getVectorDependencies(const std::string &input);

} // namespace ddb

#endif // VECTOR_H
