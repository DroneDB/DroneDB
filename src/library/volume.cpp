/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "volume.h"
#include "dem.h"
#include "stringutil.h"
#include "mathutil.h"
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

// Ax + By + C defines a plane in raster map coordinates.
struct Plane {
    double a = 0.0, b = 0.0, c = 0.0;
    double at(double x, double y) const { return a * x + b * y + c; }
};

// The sums of the normal equations of a least-squares plane z = ax + by + c over n points
struct PlaneSums {
    double xx = 0, xy = 0, x = 0, yy = 0, y = 0, xz = 0, yz = 0, z = 0;
    double n = 0;
};

// Solve the normal equations for a least-squares plane z = ax + by + c
// using a 3x3 Gaussian elimination. Good enough for the tiny matrices we
// build here (and avoids pulling Eigen into DroneDB for one call).
bool solvePlane(const PlaneSums &s, Plane &out) {
    double m[3][4] = {
        {s.xx, s.xy, s.x, s.xz},
        {s.xy, s.yy, s.y, s.yz},
        {s.x,  s.y,  s.n, s.z}
    };
    for (int i = 0; i < 3; i++) {
        int piv = i;
        for (int k = i + 1; k < 3; k++)
            if (std::fabs(m[k][i]) > std::fabs(m[piv][i])) piv = k;
        if (std::fabs(m[piv][i]) < 1e-12) return false;
        if (piv != i) std::swap(m[i], m[piv]);
        for (int k = i + 1; k < 3; k++) {
            const double f = m[k][i] / m[i][i];
            for (int j = i; j < 4; j++) m[k][j] -= f * m[i][j];
        }
    }
    const double c = m[2][3] / m[2][2];
    const double b = (m[1][3] - m[1][2] * c) / m[1][1];
    const double a = (m[0][3] - m[0][1] * b - m[0][2] * c) / m[0][0];
    out.a = a; out.b = b; out.c = c;
    return true;
}

std::string nowIso8601() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

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
    if (GDALRasterizeGeometries(maskDs, 1, bandList, 1, &hGeom,
                                nullptr, nullptr, burn, nullptr,
                                nullptr, nullptr) != CE_None)
        throw AppException("Failed to rasterize polygon");
    GDALRasterBandH maskBand = GDALGetRasterBand(maskDs, 1);
    if (GDALRasterIO(maskBand, GF_Read, 0, 0, w.width, w.height,
                     mask.data(), w.width, w.height, GDT_Byte, 0, 0) != CE_None)
        throw AppException("Cannot read mask data");
    return mask;
}

// The valid elevations on the edge of the mask, with their map coordinates. A pixel is on the
// perimeter if it is inside the mask but has at least one 4-neighbour outside (or is at the
// window edge).
struct Perimeter {
    std::vector<double> mx, my, z;
};

Perimeter collectPerimeter(const DemContext &dem, const Window &w, const std::vector<uint8_t> &mask,
                           const std::vector<float> &elev) {
    Perimeter p;
    p.mx.reserve(static_cast<size_t>(w.width) * 2 + w.height * 2);
    p.my.reserve(p.mx.capacity());
    p.z.reserve(p.mx.capacity());

    for (int r = 0; r < w.height; r++) {
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
            p.mx.push_back(mx);
            p.my.push_back(my);
            p.z.push_back(z);
        }
    }
    return p;
}

// The least-squares plane through the perimeter, in map coordinates. A degenerate perimeter
// (collinear points) gives the mean elevation.
Plane bestFitPlane(const Perimeter &perim) {
    Plane basePlane;
    // Normalize around centroid to improve conditioning.
    double cx = 0, cy = 0;
    for (size_t i = 0; i < perim.z.size(); i++) { cx += perim.mx[i]; cy += perim.my[i]; }
    cx /= perim.z.size(); cy /= perim.z.size();

    PlaneSums sums;
    sums.n = static_cast<double>(perim.z.size());
    for (size_t i = 0; i < perim.z.size(); i++) {
        const double x = perim.mx[i] - cx;
        const double y = perim.my[i] - cy;
        const double z = perim.z[i];
        sums.xx += x * x; sums.xy += x * y; sums.x += x;
        sums.yy += y * y; sums.y += y;
        sums.xz += x * z; sums.yz += y * z; sums.z += z;
    }
    Plane local{};
    if (!solvePlane(sums, local)) {
        // Degenerate (collinear points): fall back to the mean.
        basePlane.c = sums.z / sums.n;
    } else {
        // Rewrite plane in world coordinates: z = a(X-cx) + b(Y-cy) + c
        //                                     = aX + bY + (c - a*cx - b*cy)
        basePlane.a = local.a;
        basePlane.b = local.b;
        basePlane.c = local.c - local.a * cx - local.b * cy;
    }
    return basePlane;
}

Plane computeBasePlane(BaseMethod method, double flatElevation, const Perimeter &perim) {
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
    const BaseMethod method = parseMethod(baseMethod);
    LOGD << "calculateVolume method=" << methodName(method) << " path=" << rasterPath;

    // --- Parse polygon (GeoJSON, WGS84) ---
    OGRGeometryUniquePtr geom = parsePolygon(polygonGeoJson);

    // --- Open raster ---
    const DemContext dem = openDem(rasterPath);

    // --- Reproject polygon WGS84 -> raster CRS (if needed) ---
    // Capture polygon centroid latitude in WGS84 *before* any reprojection
    // so we can convert pixel sizes to meters when the raster CRS is
    // geographic (e.g. EPSG:4326).
    double polyCentroidLat = 0.0;
    {
        OGREnvelope wgsEnv;
        geom->getEnvelope(&wgsEnv);
        polyCentroidLat = 0.5 * (wgsEnv.MinY + wgsEnv.MaxY);
    }

    if (const auto wgsToRaster = dem.wgs84ToDem()) {
        if (OGR_G_Transform(OGRGeometry::ToHandle(geom.get()),
                            OGRCoordinateTransformation::ToHandle(wgsToRaster->get())) != OGRERR_NONE)
            throw AppException("Cannot reproject polygon into raster CRS");
    }

    // --- Polygon envelope in pixel space, the raster window and the polygon mask ---
    const Window window = polygonWindow(dem, *geom);
    const std::vector<float> elev = dem.readElevations(window.startCol, window.startRow, window.width, window.height);
    const std::vector<uint8_t> mask = rasterizeMask(dem, window, *geom);

    // --- Compute base plane from the perimeter elevations ---
    const Plane basePlane = computeBasePlane(method, flatElevation, collectPerimeter(dem, window, mask, elev));

    // --- Accumulate cut/fill/area ---
    // For projected CRS the geotransform is already in meters; for geographic
    // CRS (e.g. EPSG:4326) it is in degrees, so convert to meters using a
    // local WGS84 approximation (same as stockpile.cpp). Without this the
    // documented m^2 / m^3 outputs would be off by ~10^10.
    const PixelSize pixel = dem.pixelSizeMeters(polyCentroidLat);
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
