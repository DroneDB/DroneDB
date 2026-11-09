/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_PROFILE_H
#define RASTER_PROFILE_H

#include <string>

#include "raster_analysis.h"

namespace ddb {

/**
 * Sample a single-band raster along a GeoJSON LineString (WGS84) and return a
 * JSON profile suitable for elevation/temperature charts.
 *
 * Output JSON schema:
 * {
 *   "samples": [
 *     { "distance": meters, "value": float|null, "lon": deg, "lat": deg }, ...
 *   ],
 *   "totalLength": meters,
 *   "min": float|null, "max": float|null, "mean": float|null,
 *   "unit": string,
 *   "sampleCount": int,     // requested samples
 *   "validCount": int,      // samples with valid data
 *   "isThermal": bool
 * }
 *
 * @param filePath Path to the raster (GeoTIFF DSM/DTM, thermal raster, ...)
 * @param geoJsonLineString GeoJSON geometry of type LineString in WGS84
 * @param samples Requested number of equispaced samples along the polyline
 *                (clamped to [2, 4096]; <2 falls back to 256)
 * @param options The NoData override and the formula of the values (raster_analysis.h). The values are
 *                physical (raw * scale + offset); the line is converted to pixels with the one
 *                geo-to-pixel helper of the raster (raster_utils.h GeoPixelMap).
 */
std::string getRasterProfileJson(const std::string &filePath,
                                 const std::string &geoJsonLineString,
                                 int samples, const RasterValueOptions &options = {});

} // namespace ddb

#endif // RASTER_PROFILE_H
