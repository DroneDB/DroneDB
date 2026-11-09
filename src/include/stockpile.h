/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STOCKPILE_H
#define STOCKPILE_H

#include <string>

namespace ddb {

/**
 * Auto-detect a stockpile footprint by clicking on an elevation raster.
 *
 * The pile is measured against the ground around it, a least-squares plane: on a
 * slope the base takes the tilt of the slope, so the pile is what rises above
 * the slope. Rings of growing radius around the click, up to the edge of the
 * search window, are fitted with a plane; the pile is the area of the click
 * that rises above the plane of a ring that lies on the ground all around it (a
 * ring on the pile itself descends outwards with its flank). The broad, nearly
 * level top of a large pile looks like ground too: when the pile found in a
 * ring is a hump on the top of a larger pile found in a larger ring (its ground
 * lies in that pile, raised above its base by at least the height of the hump,
 * which is at most half as high as the larger pile), the larger pile takes its
 * place, so a click on the top of a pile finds its foot. A pile on a raised
 * pad, a bench or a berm rises above it by more than it is raised: it stays the
 * pile. Large rings are measured on a coarser copy of the window (a ring that
 * reaches r meters sees cells of at most r / 128), so the search costs about
 * the same on any DEM. The base is then fitted again, at the resolution of the
 * DEM, to the ground just around the outline of the pile, and the volume is
 * what rises above it: the base of a "best_fit" volume of that outline. The
 * radius limits the search: the pile and a ring of ground around it (about a
 * quarter of its radius) must fit in it.
 *
 * The confidence weighs whether the outline is the foot of the pile (the ground
 * beyond it, out to about twice its reach, lies about on its base in at least a
 * quarter of the directions around it; below 0.5 when it falls away all around
 * by about the height of the pile, as around the top of a pile wider than the
 * window), the contrast of the pile (its peak against the noise of its ground,
 * on a log scale up to 300 times) and how compact it is.
 *
 * Pipeline: local window -> growing rings of ground, each fitted with a plane ->
 * smoothed rise above the plane -> threshold over the noise of the ground ->
 * 4-connected flood fill from the click point -> the largest pile the others
 * are humps on -> base refitted around the outline -> Moore-neighbour contour
 * tracing -> Douglas-Peucker simplification -> GeoJSON polygon in WGS84.
 *
 * Output JSON schema:
 * {
 *   "polygon":         GeoJSON Polygon (WGS84),
 *   "estimatedVolume": m^3,
 *   "confidence":      float in [0, 1],
 *   "baseElevation":   m,     // mean elevation of the base under the pile
 *   "basePlaneMethod": "auto", // the plane of the ground around the pile
 *   "searchRadius":    m,
 *   "sensitivityUsed": float in [0, 1],
 *   "pixelCount":      int
 * }
 *
 * @param rasterPath   Path to a single-band DEM (GeoTIFF, COG, VRT...)
 * @param lat          Click latitude (WGS84, degrees)
 * @param lon          Click longitude (WGS84, degrees)
 * @param radiusMeters Search radius around the click (meters, >0)
 * @param sensitivity  [0,1] - higher = finer detail, lower = smoother boundary
 *
 * Throws InvalidArgsException for bad user input, NoStockpileException (an
 * InvalidArgsException) when no pile is found, AppException for GDAL/IO errors.
 */
std::string detectStockpileJson(const std::string &rasterPath,
                                double lat, double lon,
                                double radiusMeters,
                                float sensitivity);

/**
 * Auto-detect ALL stockpile footprints by full-DEM scan.
 *
 * Pipeline: full raster -> terrain trend (at every cell, the least-squares
 * plane of the elevations around it, gaussian-weighted: a tilted surface is its
 * own trend) -> diff -> gaussian smoothing
 * -> adaptive global threshold -> two-pass connected components labeling
 * -> per-component contour tracing + Douglas-Peucker simplification
 * -> sort by estimated volume desc -> truncate to maxResults.
 *
 * Output JSON schema:
 * {
 *   "stockpiles": [
 *     {
 *       "id": int,
 *       "polygon": GeoJSON Polygon (WGS84),
 *       "estimatedVolume": m^3,
 *       "confidence": float in [0, 1],
 *       "baseElevation": m,
 *       "centroid": [lon, lat],
 *       "areaM2": m^2,
 *       "pixelCount": int
 *     }, ...
 *   ],
 *   "totalFound": int,
 *   "sensitivityUsed": float,
 *   "minAreaUsed": m^2
 * }
 *
 * @param rasterPath  Path to single-band DEM
 * @param sensitivity [0,1] - higher = finer detail
 * @param minAreaM2   Minimum component area in square meters (>=0)
 * @param maxResults  Maximum number of stockpiles returned (>0, server-capped to 500)
 */
std::string detectAllStockpilesJson(const std::string &rasterPath,
                                    float sensitivity,
                                    double minAreaM2,
                                    int maxResults);

} // namespace ddb

#endif // STOCKPILE_H
