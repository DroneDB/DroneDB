/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_REGION_H
#define RASTER_REGION_H

#include <string>
#include <vector>
#include <cstdint>
#include "ddb_export.h"

namespace ddb
{

    /**
     * Render a region of a raster as a compressed image buffer.
     *
     * @param inputPath   Path to the raster (any GDAL-readable format).
     * @param bbox        [minX, minY, maxX, maxY] in @p bboxSrs (4 doubles).
     * @param bboxSrs     Authority code, e.g. "EPSG:3857". Empty = "EPSG:4326".
     * @param width       Output width in pixels (1..4096).
     * @param height      Output height in pixels (1..4096).
     * @param format      MIME type: "image/png" | "image/jpeg" | "image/webp".
     *                    Empty = "image/png".
     * @param outBytes    Allocated by VSIGetMemFileBuffer (steal=TRUE);
     *                    caller frees with `VSIFree` (== `DDBVSIFree`).
     * @param outSize     Buffer size in bytes.
     */
    DDB_DLL void renderRasterRegion(const std::string &inputPath,
                                    const double bbox[4],
                                    const std::string &bboxSrs,
                                    int width, int height,
                                    const std::string &format,
                                    uint8_t **outBytes,
                                    int *outSize);

    /**
     * Query a raster at a geographic point. Returns JSON.
     *
     * @param inputPath  Path to raster.
     * @param x          X coord in @p srs.
     * @param y          Y coord in @p srs.
     * @param srs        Authority code; empty = "EPSG:4326".
     * @return JSON string with bands, lon, lat, pixel.
     */
    DDB_DLL std::string queryRasterPoint(const std::string &inputPath,
                                         double x, double y,
                                         const std::string &srs);

    /**
     * Render a spectral index (NDVI/NDRE/NDWI/EVI/SAVI) over a raster region
     * and produce a colorized RGB(A) image.
     *
     * @param inputPath   Path to the multi-band raster (must include the bands
     *                    referenced by the index; typical mapping: R=band 1,
     *                    G=band 2, B=band 3, RedEdge=band 4, NIR=band 5).
     * @param indexName   One of: NDVI, NDRE, NDWI, EVI, SAVI (case-insensitive).
     * @param bbox        [minX, minY, maxX, maxY] in @p bboxSrs.
     * @param bboxSrs     Authority code; empty = "EPSG:4326".
     * @param width       Output width in pixels (1..4096).
     * @param height      Output height in pixels (1..4096).
     * @param format      MIME type: image/png | image/jpeg | image/webp.
     * @param outBytes    Caller frees with DDBVSIFree.
     * @param outSize     Buffer size.
     */
    DDB_DLL void renderRasterIndex(const std::string &inputPath,
                                   const std::string &indexName,
                                   const double bbox[4],
                                   const std::string &bboxSrs,
                                   int width, int height,
                                   const std::string &format,
                                   uint8_t **outBytes,
                                   int *outSize);

} // namespace ddb

#endif // RASTER_REGION_H
