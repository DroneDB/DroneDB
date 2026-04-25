/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_ANALYSIS_H
#define RASTER_ANALYSIS_H

#include <string>
#include "ddb_export.h"

namespace ddb {

/**
 * Get info about a single-band raster used for analysis (thermal image,
 * DEM/DSM/DTM, or any generic value raster).
 *
 * Output JSON schema (neutral field names):
 * {
 *   "width": int, "height": int, "bandCount": int,
 *   "dataType": "Float32"|"UInt16"|...,
 *   "valueMin": float, "valueMax": float,
 *   "unit": string          // from GDAL UNITTYPE, "" if unknown; "\u00B0C" if thermal
 *   "isThermal": bool,
 *   "isDirectValue": bool,   // true when values are already in target units
 *   "sensorId": string,      // detected sensor profile id (if any)
 *   "calibration": {...}     // only when isThermal && calibration valid
 * }
 */
DDB_DLL std::string getRasterValueInfoJson(const std::string &filePath);

/**
 * Get raster value at pixel (x, y).
 *
 * Output JSON schema:
 * {
 *   "value": float,
 *   "rawValue": float,
 *   "x": int, "y": int,
 *   "geoX": float, "geoY": float, "hasGeo": bool,
 *   "isThermal": bool
 * }
 */
DDB_DLL std::string getRasterPointJson(const std::string &filePath, int x, int y);

/**
 * Get statistics over pixel rectangle [x0,y0]-[x1,y1] (inclusive).
 *
 * Output JSON schema:
 * {
 *   "min": float, "max": float, "mean": float, "stddev": float, "median": float,
 *   "pixelCount": int,
 *   "bounds": { "x0": int, "y0": int, "x1": int, "y1": int },
 *   "unit": string,
 *   "isThermal": bool
 * }
 */
DDB_DLL std::string getRasterAreaStatsJson(const std::string &filePath,
                                           int x0, int y0, int x1, int y1);

/** Read GDAL UNITTYPE of the first band of a raster.
 *  Returns empty string if unset or file cannot be opened. */
DDB_DLL std::string readGdalUnitType(const std::string &filePath);

} // namespace ddb

#endif // RASTER_ANALYSIS_H
