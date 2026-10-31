/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "volume.h"
#include "cancellation.h"
#include "timeutil.h"
#include "dem.h"
#include "stringutil.h"
#include "mathutil.h"
#include "plane_fit.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "json.h"
#include "logger.h"

#include <gdal_alg.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ddb {

namespace {

enum class BaseMethod {
    LowestPerimeter,
    AveragePerimeter,
    BestFit,
    FlatPlane
};

BaseMethod parseMethod(const std::string &raw) {
    const std::string m = str::toLowerCopy(raw);
    if (m == "lowest_perimeter" || m.empty()) return BaseMethod::LowestPerimeter;
    if (m == "average_perimeter") return BaseMethod::AveragePerimeter;
    if (m == "best_fit") return BaseMethod::BestFit;
    if (m == "flat" || m == "flat_plane") return BaseMethod::FlatPlane;
    throw InvalidArgsException("Unknown base plane method: " + raw);
}

const char *methodName(BaseMethod m) {
    switch (m) {
        case BaseMethod::LowestPerimeter:  return "lowest_perimeter";
        case BaseMethod::AveragePerimeter: return "average_perimeter";
        case BaseMethod::BestFit:          return "best_fit";
        case BaseMethod::FlatPlane:        return "flat";
    }
    return "lowest_perimeter";
}

std::string nowIso8601() { return utcIso8601(currentUnixTimestamp()); }

// The part of the raster the polygon covers, in pixels
struct Window {
    int startCol = 0, startRow = 0;
    int width = 0, height = 0;

    size_t index(int r, int c) const { return static_cast<size_t>(r) * width + c; }

    // The map coordinates of the center of the pixel (row r, column c) of the window
    void cellCenter(const double *gt, int r, int c, double &mx, double &my) const {
        mx = gt[0] + (startCol + c + 0.5) * gt[1] + (startRow + r + 0.5) * gt[2];
        my = gt[3] + (startCol + c + 0.5) * gt[4] + (startRow + r + 0.5) * gt[5];
    }
};

// The polygon of the request (GeoJSON, WGS84); throws InvalidArgsException when it is not one
OGRGeometryUniquePtr parsePolygon(const std::string &polygonGeoJson) {
    OGRGeometryH hRaw = OGR_G_CreateGeometryFromJson(polygonGeoJson.c_str());
    if (!hRaw) throw InvalidArgsException("Cannot parse polygon GeoJSON");
    OGRGeometryUniquePtr geom(OGRGeometry::FromHandle(hRaw));
    const OGRwkbGeometryType gType = wkbFlatten(geom->getGeometryType());
    if (gType != wkbPolygon && gType != wkbMultiPolygon)
        throw InvalidArgsException("Volume geometry must be a Polygon or MultiPolygon");
    return geom;
}

// The window of the raster that the envelope of geom (already in the raster CRS) covers
Window polygonWindow(const DemContext &dem, const OGRGeometry &geom) {
    OGREnvelope env;
    geom.getEnvelope(&env);

    double invGt[6];
    if (!GDALInvGeoTransform(dem.gt, invGt))
        throw AppException("Cannot invert geotransform");

    // Map the four envelope corners to pixel space to stay correct even
    // for rasters with negative pixel heights.
    auto toPx = [&](double mx, double my, double &px, double &py) {
        px = invGt[0] + invGt[1] * mx + invGt[2] * my;
        py = invGt[3] + invGt[4] * mx + invGt[5] * my;
    };
    double px1, py1, px2, py2, px3, py3, px4, py4;
    toPx(env.MinX, env.MinY, px1, py1);
    toPx(env.MaxX, env.MinY, px2, py2);
    toPx(env.MinX, env.MaxY, px3, py3);
    toPx(env.MaxX, env.MaxY, px4, py4);
    const double minPx = std::min({px1, px2, px3, px4});
    const double maxPx = std::max({px1, px2, px3, px4});
    const double minPy = std::min({py1, py2, py3, py4});
    const double maxPy = std::max({py1, py2, py3, py4});

    Window w;
    w.startCol = std::max(0, static_cast<int>(std::floor(minPx)));
    w.startRow = std::max(0, static_cast<int>(std::floor(minPy)));
    const int endCol = std::min(dem.width, static_cast<int>(std::ceil(maxPx)));
    const int endRow = std::min(dem.height, static_cast<int>(std::ceil(maxPy)));
    w.width = endCol - w.startCol;
    w.height = endRow - w.startRow;

    if (w.width <= 0 || w.height <= 0)
        throw InvalidArgsException("Polygon does not overlap raster extent");
    return w;
}

// Builds a polygon mask by rasterizing the geometry onto an in-memory band (faster and more
// robust than per-pixel Contains tests): 1 inside, 0 outside
std::vector<uint8_t> rasterizeMask(const DemContext &dem, const Window &w, OGRGeometry &geom) {
    const double *gt = dem.gt;
    std::vector<uint8_t> mask(static_cast<size_t>(w.width) * w.height, 0);

    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (!memDrv) throw AppException("MEM driver unavailable");
    const auto maskOwner = adoptDataset(
        GDALCreate(memDrv, "", w.width, w.height, 1, GDT_Byte, nullptr));
    if (!maskOwner) throw AppException("Cannot create mask dataset");
    GDALDatasetH maskDs = datasetHandle(maskOwner);

    double maskGt[6] = {
        gt[0] + w.startCol * gt[1] + w.startRow * gt[2],
        gt[1], gt[2],
        gt[3] + w.startCol * gt[4] + w.startRow * gt[5],
        gt[4], gt[5]
    };
    if (GDALSetGeoTransform(maskDs, maskGt) != CE_None ||
        (!dem.projection.empty() && GDALSetProjection(maskDs, dem.projection.c_str()) != CE_None))
        throw AppException("Cannot georeference the mask dataset");

    OGRGeometryH hGeom = OGRGeometry::ToHandle(&geom);
    int bandList[1] = {1};
    double burn[1] = {1.0};
    // The rasterization stops when the call is canceled
    cancel::GdalProgress progress;
    if (GDALRasterizeGeometries(maskDs, 1, bandList, 1, &hGeom,
                                nullptr, nullptr, burn, nullptr,
                                progress.function(), progress.data()) != CE_None) {
        cancel::throwIfCanceled();
        throw AppException("Failed to rasterize polygon");
    }
    GDALRasterBandH maskBand = GDALGetRasterBand(maskDs, 1);
    if (GDALRasterIO(maskBand, GF_Read, 0, 0, w.width, w.height,
                     mask.data(), w.width, w.height, GDT_Byte, 0, 0) != CE_None)
        throw AppException("Cannot read mask data");
    return mask;
}

// The valid elevations on the edge of the mask, with their map coordinates. A pixel is on the
// perimeter if it is inside the mask but has at least one 4-neighbour outside (or is at the
// window edge).
PlanePoints collectPerimeter(const DemContext &dem, const Window &w, const std::vector<uint8_t> &mask,
                             const std::vector<float> &elev) {
    PlanePoints p;
    p.x.reserve(static_cast<size_t>(w.width) * 2 + w.height * 2);
    p.y.reserve(p.x.capacity());
    p.z.reserve(p.x.capacity());

    for (int r = 0; r < w.height; r++) {
        cancel::throwIfCanceled();
        for (int c = 0; c < w.width; c++) {
            if (!mask[w.index(r, c)]) continue;
            bool onEdge = (r == 0 || r == w.height - 1 || c == 0 || c == w.width - 1)
                || !mask[w.index(r - 1, c)]
                || !mask[w.index(r + 1, c)]
                || !mask[w.index(r, c - 1)]
                || !mask[w.index(r, c + 1)];
            if (!onEdge) continue;
            const float z = elev[w.index(r, c)];
            if (!dem.isValid(z)) continue;
            double mx, my;
            w.cellCenter(dem.gt, r, c, mx, my);
            p.add(mx, my, z);
        }
    }
    return p;
}

Plane computeBasePlane(BaseMethod method, double flatElevation, const PlanePoints &perim) {
    Plane basePlane;
    if (method == BaseMethod::FlatPlane) {
        basePlane.c = flatElevation;
        return basePlane;
    }

    if (perim.z.empty())
        throw AppException("No valid perimeter elevations; cannot estimate base plane");

    if (method == BaseMethod::LowestPerimeter) {
        basePlane.c = *std::min_element(perim.z.begin(), perim.z.end());
    } else if (method == BaseMethod::AveragePerimeter) {
        double s = 0.0;
        for (double z : perim.z) s += z;
        basePlane.c = s / static_cast<double>(perim.z.size());
    } else { // BestFit
        basePlane = bestFitPlane(perim);
    }
    return basePlane;
}

// What the pixels of the polygon add up to against the base plane
struct VolumeTotals {
    double cutVolume = 0.0;
    double fillVolume = 0.0;
    double area3d = 0.0;
    double baseElevationSum = 0.0;
    size_t pixelCount = 0;
};

VolumeTotals accumulateVolume(const DemContext &dem, const Window &w, const std::vector<uint8_t> &mask,
                              const std::vector<float> &elev, const Plane &basePlane,
                              const PixelSize &pixel) {
    const double pixelArea = pixel.x * pixel.y;
    VolumeTotals t;

    for (int r = 0; r < w.height; r++) {
        cancel::throwIfCanceled();
        for (int c = 0; c < w.width; c++) {
            if (!mask[w.index(r, c)]) continue;
            const float z = elev[w.index(r, c)];
            if (!dem.isValid(z)) continue;

            double mx, my;
            w.cellCenter(dem.gt, r, c, mx, my);
            const double bz = basePlane.at(mx, my);
            const double diff = static_cast<double>(z) - bz;

            if (diff > 0)      t.cutVolume  += diff * pixelArea;
            else if (diff < 0) t.fillVolume += (-diff) * pixelArea;

            // 3D area from local slope (central differences with edge fallback).
            auto safeGet = [&](int rr, int cc, float &out) {
                rr = std::clamp(rr, 0, w.height - 1);
                cc = std::clamp(cc, 0, w.width - 1);
                out = elev[w.index(rr, cc)];
                return dem.isValid(out);
            };
            float zxp = 0, zxm = 0, zyp = 0, zym = 0;
            const bool okXp = safeGet(r, c + 1, zxp);
            const bool okXm = safeGet(r, c - 1, zxm);
            const bool okYp = safeGet(r + 1, c, zyp);
            const bool okYm = safeGet(r - 1, c, zym);
            const double dzdx = (okXp && okXm)
                ? (zxp - zxm) / (2.0 * pixel.x)
                : 0.0;
            const double dzdy = (okYp && okYm)
                ? (zyp - zym) / (2.0 * pixel.y)
                : 0.0;
            const double slope2 = dzdx * dzdx + dzdy * dzdy;
            t.area3d += pixelArea * std::sqrt(1.0 + slope2);

            t.baseElevationSum += bz;
            t.pixelCount++;
        }
    }
    return t;
}

} // anonymous namespace

std::string calculateVolumeJson(const std::string &rasterPath,
                                const std::string &polygonGeoJson,
                                const std::string &baseMethod,
                                double flatElevation) {
    cancel::throwIfCanceled();
    const BaseMethod method = parseMethod(baseMethod);
    LOGD << "calculateVolume method=" << methodName(method) << " path=" << rasterPath;

    // --- Parse polygon (GeoJSON, WGS84) ---
    OGRGeometryUniquePtr geom = parsePolygon(polygonGeoJson);

    // --- Open raster ---
    const DemContext dem = openDem(rasterPath);

    // --- Reproject polygon WGS84 -> raster CRS (if needed) ---
    if (const auto wgsToRaster = dem.wgs84ToDem()) {
        if (OGR_G_Transform(OGRGeometry::ToHandle(geom.get()),
                            OGRCoordinateTransformation::ToHandle(wgsToRaster->get())) != OGRERR_NONE)
            throw AppException("Cannot reproject polygon into raster CRS");
    }

    // The pixel size is measured on the ground at the polygon, whatever the CRS of the raster: the
    // degrees of a geographic one, the feet of a foot-based one and the stretch of a Web Mercator one
    // all have to end up as meters, or the documented m^2 / m^3 outputs would be off.
    OGREnvelope rasterEnv;
    geom->getEnvelope(&rasterEnv);
    const PixelSize pixel = dem.pixelSizeMeters(0.5 * (rasterEnv.MinX + rasterEnv.MaxX),
                                                0.5 * (rasterEnv.MinY + rasterEnv.MaxY));

    // --- Polygon envelope in pixel space, the raster window and the polygon mask ---
    const Window window = polygonWindow(dem, *geom);
    const std::vector<float> elev = dem.readElevations(window.startCol, window.startRow, window.width, window.height);
    const std::vector<uint8_t> mask = rasterizeMask(dem, window, *geom);

    // --- Compute base plane from the perimeter elevations ---
    const Plane basePlane = computeBasePlane(method, flatElevation, collectPerimeter(dem, window, mask, elev));

    // --- Accumulate cut/fill/area ---
    const double pixelArea = pixel.x * pixel.y;
    const VolumeTotals totals = accumulateVolume(dem, window, mask, elev, basePlane, pixel);

    if (totals.pixelCount == 0)
        throw AppException("Polygon did not cover any valid raster pixels");

    const double area2d = static_cast<double>(totals.pixelCount) * pixelArea;
    const double baseElevationAvg = totals.baseElevationSum / static_cast<double>(totals.pixelCount);
    const double cutVolume = totals.cutVolume;
    const double fillVolume = totals.fillVolume;

    // --- Build JSON response (echo input polygon as GeoJSON in WGS84) ---
    json result;
    result["cutVolume"] = cutVolume;
    result["fillVolume"] = fillVolume;
    result["netVolume"] = cutVolume - fillVolume;
    result["area2d"] = area2d;
    result["area3d"] = totals.area3d;
    result["baseElevation"] = baseElevationAvg;
    result["basePlaneMethod"] = methodName(method);
    result["pixelSize"] = std::min(pixel.x, pixel.y);
    result["crs"] = dem.projection;
    result["pixelCount"] = static_cast<uint64_t>(totals.pixelCount);
    result["calculatedAt"] = nowIso8601();
    try {
        result["boundaryPolygon"] = json::parse(polygonGeoJson);
    } catch (const json::parse_error &e) {
        LOGD << "Failed to parse boundary polygon GeoJSON: " << e.what();
        result["boundaryPolygon"] = nullptr;
    }

    LOGD << "Volume: cut=" << cutVolume << " fill=" << fillVolume
         << " net=" << (cutVolume - fillVolume) << " pixels=" << totals.pixelCount;

    return result.dump();
}

} // namespace ddb
