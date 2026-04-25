/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "volume.h"
#include "exceptions.h"
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

// Keep the implementation self-contained: small RAII wrappers mirroring
// those in raster_profile.cpp. Duplication is intentional — the helpers
// are 3 lines each and cross-TU sharing would add a header only for them.

struct OgrGeometryDeleter {
    void operator()(OGRGeometry *g) const {
        if (g) OGRGeometryFactory::destroyGeometry(g);
    }
};
using OgrGeometryPtr = std::unique_ptr<OGRGeometry, OgrGeometryDeleter>;

struct SrsHandle {
    OGRSpatialReferenceH h = nullptr;
    explicit SrsHandle(OGRSpatialReferenceH handle = nullptr) : h(handle) {}
    ~SrsHandle() { if (h) OSRDestroySpatialReference(h); }
    SrsHandle(const SrsHandle &) = delete;
    SrsHandle &operator=(const SrsHandle &) = delete;
};

struct CtHandle {
    OGRCoordinateTransformationH h = nullptr;
    ~CtHandle() { if (h) OCTDestroyCoordinateTransformation(h); }
};

struct DsGuard {
    GDALDatasetH h = nullptr;
    ~DsGuard() { if (h) GDALClose(h); }
};

enum class BaseMethod {
    LowestPerimeter,
    AveragePerimeter,
    BestFit,
    FlatPlane
};

BaseMethod parseMethod(const std::string &raw) {
    std::string m;
    m.reserve(raw.size());
    for (char c : raw) m.push_back(static_cast<char>(std::tolower((unsigned char)c)));
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

// Solve the normal equations for a least-squares plane z = ax + by + c
// using a 3x3 Gaussian elimination. Good enough for the tiny matrices we
// build here (and avoids pulling Eigen into DroneDB for one call).
bool solvePlane(double S_xx, double S_xy, double S_x,
                double S_yy, double S_y, double n,
                double S_xz, double S_yz, double S_z,
                Plane &out) {
    double m[3][4] = {
        {S_xx, S_xy, S_x, S_xz},
        {S_xy, S_yy, S_y, S_yz},
        {S_x,  S_y,  n,   S_z}
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

} // anonymous namespace

std::string calculateVolumeJson(const std::string &rasterPath,
                                const std::string &polygonGeoJson,
                                const std::string &baseMethod,
                                double flatElevation) {
    const BaseMethod method = parseMethod(baseMethod);
    LOGD << "calculateVolume method=" << methodName(method) << " path=" << rasterPath;

    // --- Parse polygon (GeoJSON, WGS84) ---
    OGRGeometryH hRaw = OGR_G_CreateGeometryFromJson(polygonGeoJson.c_str());
    if (!hRaw) throw InvalidArgsException("Cannot parse polygon GeoJSON");
    OgrGeometryPtr geom(reinterpret_cast<OGRGeometry *>(hRaw));
    const OGRwkbGeometryType gType = wkbFlatten(geom->getGeometryType());
    if (gType != wkbPolygon && gType != wkbMultiPolygon)
        throw InvalidArgsException("Volume geometry must be a Polygon or MultiPolygon");

    // --- Open raster ---
    GDALDatasetH hDs = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!hDs) throw AppException("Cannot open raster: " + rasterPath);
    DsGuard dsGuard{hDs};

    double gt[6];
    if (GDALGetGeoTransform(hDs, gt) != CE_None)
        throw AppException("Raster has no geotransform: " + rasterPath);

    const int rasterW = GDALGetRasterXSize(hDs);
    const int rasterH = GDALGetRasterYSize(hDs);
    if (GDALGetRasterCount(hDs) < 1)
        throw AppException("Raster has no bands");

    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(hBand, &hasNoData);

    const char *projRef = GDALGetProjectionRef(hDs);
    const std::string projection = projRef ? projRef : "";

    // --- Reproject polygon WGS84 -> raster CRS (if needed) ---
    SrsHandle wgs84(OSRNewSpatialReference(nullptr));
    OSRImportFromEPSG(wgs84.h, 4326);
    OSRSetAxisMappingStrategy(wgs84.h, OAMS_TRADITIONAL_GIS_ORDER);

    SrsHandle rasterSrs;
    if (!projection.empty()) {
        rasterSrs.h = OSRNewSpatialReference(nullptr);
        char *wktPtr = const_cast<char *>(projection.c_str());
        if (OSRImportFromWkt(rasterSrs.h, &wktPtr) != OGRERR_NONE) {
            OSRDestroySpatialReference(rasterSrs.h);
            rasterSrs.h = nullptr;
        } else {
            OSRSetAxisMappingStrategy(rasterSrs.h, OAMS_TRADITIONAL_GIS_ORDER);
        }
    }

    if (rasterSrs.h) {
        CtHandle wgsToRaster;
        wgsToRaster.h = OCTNewCoordinateTransformation(wgs84.h, rasterSrs.h);
        if (wgsToRaster.h) {
            OGR_G_Transform(reinterpret_cast<OGRGeometryH>(geom.get()), wgsToRaster.h);
        }
    }

    // --- Polygon envelope in pixel space ---
    OGREnvelope env;
    geom->getEnvelope(&env);

    double invGt[6];
    if (!GDALInvGeoTransform(gt, invGt))
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

    const int startCol = std::max(0, static_cast<int>(std::floor(minPx)));
    const int startRow = std::max(0, static_cast<int>(std::floor(minPy)));
    const int endCol = std::min(rasterW, static_cast<int>(std::ceil(maxPx)));
    const int endRow = std::min(rasterH, static_cast<int>(std::ceil(maxPy)));
    const int width = endCol - startCol;
    const int height = endRow - startRow;

    if (width <= 0 || height <= 0)
        throw InvalidArgsException("Polygon does not overlap raster extent");

    // --- Read the raster window ---
    std::vector<float> elev(static_cast<size_t>(width) * height);
    if (GDALRasterIO(hBand, GF_Read, startCol, startRow, width, height,
                     elev.data(), width, height, GDT_Float32, 0, 0) != CE_None)
        throw AppException("Cannot read raster data");

    // --- Build a polygon mask by rasterizing the geometry onto an in-memory
    //     band (faster and more robust than per-pixel Contains tests). ---
    std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0);

    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (!memDrv) throw AppException("MEM driver unavailable");
    GDALDatasetH maskDs = GDALCreate(memDrv, "", width, height, 1, GDT_Byte, nullptr);
    if (!maskDs) throw AppException("Cannot create mask dataset");
    DsGuard maskGuard{maskDs};

    double maskGt[6] = {
        gt[0] + startCol * gt[1] + startRow * gt[2],
        gt[1], gt[2],
        gt[3] + startCol * gt[4] + startRow * gt[5],
        gt[4], gt[5]
    };
    GDALSetGeoTransform(maskDs, maskGt);
    if (!projection.empty()) GDALSetProjection(maskDs, projection.c_str());

    OGRGeometryH hGeom = reinterpret_cast<OGRGeometryH>(geom.get());
    int bandList[1] = {1};
    double burn[1] = {1.0};
    if (GDALRasterizeGeometries(maskDs, 1, bandList, 1, &hGeom,
                                nullptr, nullptr, burn, nullptr,
                                nullptr, nullptr) != CE_None)
        throw AppException("Failed to rasterize polygon");
    GDALRasterBandH maskBand = GDALGetRasterBand(maskDs, 1);
    if (GDALRasterIO(maskBand, GF_Read, 0, 0, width, height,
                     mask.data(), width, height, GDT_Byte, 0, 0) != CE_None)
        throw AppException("Cannot read mask data");

    const auto isValid = [&](float v) {
        if (!std::isfinite(v)) return false;
        if (hasNoData && std::fabs(static_cast<double>(v) - noData) < 1e-9) return false;
        return true;
    };

    // --- Collect perimeter elevations for base plane computation ---
    // A pixel is on the perimeter if it is inside the mask but has at least
    // one 4-neighbour outside (or is at the window edge).
    std::vector<double> perimMx, perimMy, perimZ;
    perimMx.reserve(static_cast<size_t>(width) * 2 + height * 2);
    perimMy.reserve(perimMx.capacity());
    perimZ.reserve(perimMx.capacity());

    auto indexAt = [width](int r, int c) { return static_cast<size_t>(r) * width + c; };

    for (int r = 0; r < height; r++) {
        for (int c = 0; c < width; c++) {
            if (!mask[indexAt(r, c)]) continue;
            bool onEdge = (r == 0 || r == height - 1 || c == 0 || c == width - 1)
                || !mask[indexAt(r - 1, c)]
                || !mask[indexAt(r + 1, c)]
                || !mask[indexAt(r, c - 1)]
                || !mask[indexAt(r, c + 1)];
            if (!onEdge) continue;
            const float z = elev[indexAt(r, c)];
            if (!isValid(z)) continue;
            const double mx = gt[0] + (startCol + c + 0.5) * gt[1]
                              + (startRow + r + 0.5) * gt[2];
            const double my = gt[3] + (startCol + c + 0.5) * gt[4]
                              + (startRow + r + 0.5) * gt[5];
            perimMx.push_back(mx);
            perimMy.push_back(my);
            perimZ.push_back(z);
        }
    }

    // --- Compute base plane ---
    Plane basePlane;
    if (method == BaseMethod::FlatPlane) {
        basePlane.c = flatElevation;
    } else {
        if (perimZ.empty())
            throw AppException("No valid perimeter elevations; cannot estimate base plane");

        if (method == BaseMethod::LowestPerimeter) {
            basePlane.c = *std::min_element(perimZ.begin(), perimZ.end());
        } else if (method == BaseMethod::AveragePerimeter) {
            double s = 0.0;
            for (double z : perimZ) s += z;
            basePlane.c = s / static_cast<double>(perimZ.size());
        } else { // BestFit
            // Normalize around centroid to improve conditioning.
            double cx = 0, cy = 0;
            for (size_t i = 0; i < perimZ.size(); i++) { cx += perimMx[i]; cy += perimMy[i]; }
            cx /= perimZ.size(); cy /= perimZ.size();

            double S_xx = 0, S_xy = 0, S_x = 0,
                   S_yy = 0, S_y = 0,
                   S_xz = 0, S_yz = 0, S_z = 0;
            const double n = static_cast<double>(perimZ.size());
            for (size_t i = 0; i < perimZ.size(); i++) {
                const double x = perimMx[i] - cx;
                const double y = perimMy[i] - cy;
                const double z = perimZ[i];
                S_xx += x * x; S_xy += x * y; S_x += x;
                S_yy += y * y; S_y += y;
                S_xz += x * z; S_yz += y * z; S_z += z;
            }
            Plane local{};
            if (!solvePlane(S_xx, S_xy, S_x, S_yy, S_y, n, S_xz, S_yz, S_z, local)) {
                // Degenerate (collinear points): fall back to the mean.
                basePlane.c = S_z / n;
            } else {
                // Rewrite plane in world coordinates: z = a(X-cx) + b(Y-cy) + c
                //                                     = aX + bY + (c - a*cx - b*cy)
                basePlane.a = local.a;
                basePlane.b = local.b;
                basePlane.c = local.c - local.a * cx - local.b * cy;
            }
        }
    }

    // --- Accumulate cut/fill/area ---
    const double pixelWidth = std::fabs(gt[1]);
    const double pixelHeight = std::fabs(gt[5]);
    const double pixelArea = pixelWidth * pixelHeight;

    double cutVolume = 0.0;
    double fillVolume = 0.0;
    double area3d = 0.0;
    double baseElevationSum = 0.0;
    size_t pixelCount = 0;

    for (int r = 0; r < height; r++) {
        for (int c = 0; c < width; c++) {
            if (!mask[indexAt(r, c)]) continue;
            const float z = elev[indexAt(r, c)];
            if (!isValid(z)) continue;

            const double mx = gt[0] + (startCol + c + 0.5) * gt[1]
                              + (startRow + r + 0.5) * gt[2];
            const double my = gt[3] + (startCol + c + 0.5) * gt[4]
                              + (startRow + r + 0.5) * gt[5];
            const double bz = basePlane.at(mx, my);
            const double diff = static_cast<double>(z) - bz;

            if (diff > 0)      cutVolume  += diff * pixelArea;
            else if (diff < 0) fillVolume += (-diff) * pixelArea;

            // 3D area from local slope (central differences with edge fallback).
            auto safeGet = [&](int rr, int cc, float &out) {
                rr = std::clamp(rr, 0, height - 1);
                cc = std::clamp(cc, 0, width - 1);
                out = elev[indexAt(rr, cc)];
                return isValid(out);
            };
            float zxp = 0, zxm = 0, zyp = 0, zym = 0;
            const bool okXp = safeGet(r, c + 1, zxp);
            const bool okXm = safeGet(r, c - 1, zxm);
            const bool okYp = safeGet(r + 1, c, zyp);
            const bool okYm = safeGet(r - 1, c, zym);
            const double dzdx = (okXp && okXm)
                ? (zxp - zxm) / (2.0 * pixelWidth)
                : 0.0;
            const double dzdy = (okYp && okYm)
                ? (zyp - zym) / (2.0 * pixelHeight)
                : 0.0;
            const double slope2 = dzdx * dzdx + dzdy * dzdy;
            area3d += pixelArea * std::sqrt(1.0 + slope2);

            baseElevationSum += bz;
            pixelCount++;
        }
    }

    if (pixelCount == 0)
        throw AppException("Polygon did not cover any valid raster pixels");

    const double area2d = static_cast<double>(pixelCount) * pixelArea;
    const double baseElevationAvg = baseElevationSum / static_cast<double>(pixelCount);

    // --- Build JSON response (echo input polygon as GeoJSON in WGS84) ---
    json result;
    result["cutVolume"] = cutVolume;
    result["fillVolume"] = fillVolume;
    result["netVolume"] = cutVolume - fillVolume;
    result["area2d"] = area2d;
    result["area3d"] = area3d;
    result["baseElevation"] = baseElevationAvg;
    result["basePlaneMethod"] = methodName(method);
    result["pixelSize"] = std::min(pixelWidth, pixelHeight);
    result["crs"] = projection;
    result["pixelCount"] = static_cast<uint64_t>(pixelCount);
    result["calculatedAt"] = nowIso8601();
    try {
        result["boundaryPolygon"] = json::parse(polygonGeoJson);
    } catch (...) {
        result["boundaryPolygon"] = nullptr;
    }

    LOGD << "Volume: cut=" << cutVolume << " fill=" << fillVolume
         << " net=" << (cutVolume - fillVolume) << " pixels=" << pixelCount;

    return result.dump();
}

} // namespace ddb
