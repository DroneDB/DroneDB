/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_REGION_H
#define RASTER_REGION_H

#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include "ddb_export.h"

namespace ddb
{

    /** A rectangular area of a raster to render as an image. */
    struct RegionRequest
    {
        /// Path to the raster (any GDAL-readable format).
        std::string inputPath;
        /// [minX, minY, maxX, maxY] in @c bboxSrs.
        std::array<double, 4> bbox{};
        /// Authority code, e.g. "EPSG:3857". Empty = "EPSG:4326".
        std::string bboxSrs;
        /// Output size in pixels (1..4096 each).
        int width = 0;
        int height = 0;
        /// MIME type: "image/png" | "image/jpeg" | "image/webp" | "image/tiff" | "image/geotiff".
        /// Empty = "image/png".
        std::string format;
    };

    /** What renderRasterRegion can do besides the region. */
    struct RegionOptions
    {
        /// Target CRS authority code; empty means "same as bboxSrs". Pass the source CRS to avoid
        /// cross-projection resampling artefacts (useful for scientific multi-band data via OGC WCS).
        std::string outputCrs;
        /// 1-based band selection (e.g. {4, 1, 2} for a false-colour NIR/R/G composite). Empty =
        /// all bands in source order.
        std::vector<int> bands;
    };

    /**
     * Render a region of a raster as a compressed image buffer.
     *
     * @param request     The region, see @ref RegionRequest.
     * @param options     See @ref RegionOptions.
     * @param outBytes    Allocated by VSIGetMemFileBuffer (steal=TRUE);
     *                    caller frees with `VSIFree` (== `DDBVSIFree`).
     * @param outSize     Buffer size in bytes.
     */
    DDB_DLL void renderRasterRegion(const RegionRequest &request,
                                    const RegionOptions &options,
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
     * The index is computed and colorized by the formula engine the thumbnails and
     * the tiles use (VegetationEngine, vis_render.h), over the range [-1, 1] with
     * its ramp (red, yellow, green; not a colormap clients can pick). A pixel is
     * transparent in the output when a band the index reads has no value (nodata,
     * NaN), or when the alpha band, if the raster has one, is 0: alpha says "no
     * data here" for every band. A gap in a band the index does not read (the blue
     * band for NDVI) does not hide the pixel.
     *
     * @param request     The region: request.inputPath is the multi-band raster (it must
     *                    include the bands referenced by the index; typical mapping: R=band 1,
     *                    G=band 2, B=band 3, RedEdge=band 4, NIR=band 5), and the format is one
     *                    of image/png | image/jpeg | image/webp | image/tiff | image/geotiff.
     * @param indexName   One of: NDVI, NDRE, NDWI, EVI, SAVI (case-insensitive).
     * @param outBytes    Caller frees with DDBVSIFree.
     * @param outSize     Buffer size.
     */
    DDB_DLL void renderRasterIndex(const RegionRequest &request,
                                   const std::string &indexName,
                                   uint8_t **outBytes,
                                   int *outSize);

} // namespace ddb

#endif // RASTER_REGION_H
