/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_profile.h"
#include "raster_analysis.h"
#include "thermal.h"
#include "sensorprofile.h"
#include "exceptions.h"
#include "logger.h"

#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <ogr_spatialref.h>
#include <ogr_geometry.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace ddb {

namespace {

constexpr int DEFAULT_SAMPLES = 256;
constexpr int MIN_SAMPLES = 2;
constexpr int MAX_SAMPLES = 4096;
constexpr double WGS84_RADIUS_M = 6378137.0;

// Haversine distance on WGS84 ellipsoid approximated as a sphere (meters).
double haversineMeters(double lon1, double lat1, double lon2, double lat2) {
    const double d2r = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * d2r;
    const double dLon = (lon2 - lon1) * d2r;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(lat1 * d2r) * std::cos(lat2 * d2r) *
                     std::sin(dLon / 2) * std::sin(dLon / 2);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return WGS84_RADIUS_M * c;
}

// Mirrors detectIsThermal() in raster_analysis.cpp. Keeps profile output
// consistent with the info/point/area endpoints without forcing the
// anonymous helper there to be exposed.
bool detectIsThermalProfile(const std::string &filePath) {
    try {
        auto &spm = SensorProfileManager::instance();
        auto det = spm.detectSensor(filePath);
        if (det.detected && det.sensorCategory == "thermal") return true;
    } catch (...) { /* ignore and fall through */ }

    return isThermalImage(filePath) || isDirectTemperatureRaster(filePath);
}

// Simple RAII helpers for OGR/GDAL handles used inside this translation unit.
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

} // anonymous namespace

std::string getRasterProfileJson(const std::string &filePath,
                                 const std::string &geoJsonLineString,
                                 int samples) {
    // Clamp / defaults.
    if (samples < MIN_SAMPLES) samples = DEFAULT_SAMPLES;
    if (samples > MAX_SAMPLES) samples = MAX_SAMPLES;

    // --- Parse GeoJSON LineString (expected WGS84 lon/lat) ---
    OGRGeometryH hRawGeom = OGR_G_CreateGeometryFromJson(geoJsonLineString.c_str());
    if (hRawGeom == nullptr)
        throw InvalidArgsException("Cannot parse GeoJSON geometry");

    OgrGeometryPtr geom(reinterpret_cast<OGRGeometry *>(hRawGeom));
    const OGRwkbGeometryType gType = wkbFlatten(geom->getGeometryType());
    if (gType != wkbLineString)
        throw InvalidArgsException("Profile geometry must be a LineString");

    OGRLineString *line = geom->toLineString();
    const int nVertices = line->getNumPoints();
    if (nVertices < 2)
        throw InvalidArgsException("LineString must have at least 2 points");

    // --- Open raster and inspect metadata ---
    GDALDatasetH hDs = GDALOpen(filePath.c_str(), GA_ReadOnly);
    if (!hDs)
        throw AppException("Cannot open raster: " + filePath);

    // Use a scope guard for GDALClose so we can early-return on errors.
    struct DsGuard {
        GDALDatasetH h;
        ~DsGuard() { if (h) GDALClose(h); }
    } dsGuard{hDs};

    double gt[6];
    if (GDALGetGeoTransform(hDs, gt) != CE_None)
        throw AppException("Raster has no geotransform: " + filePath);

    double invGt[6];
    if (!GDALInvGeoTransform(gt, invGt))
        throw AppException("Cannot invert geotransform");

    const int rasterW = GDALGetRasterXSize(hDs);
    const int rasterH = GDALGetRasterYSize(hDs);
    if (GDALGetRasterCount(hDs) < 1)
        throw AppException("Raster has no bands");

    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    const GDALDataType dt = GDALGetRasterDataType(hBand);

    const char *unitCStr = GDALGetRasterUnitType(hBand);
    std::string unit = unitCStr ? unitCStr : "";

    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(hBand, &hasNoData);

    const char *projRef = GDALGetProjectionRef(hDs);
    const std::string projection = projRef ? projRef : "";

    const bool isThermal = detectIsThermalProfile(filePath);
    ThermalCalibration cal;
    const bool needsThermalCalib = isThermal && dt == GDT_UInt16;
    if (needsThermalCalib) cal = extractThermalCalibration(filePath);

    if (unit.empty() && isThermal) unit = "\xC2\xB0\x43"; // UTF-8 "°C"

    // --- Setup coordinate transforms (WGS84 <-> raster CRS) ---
    SrsHandle wgs84(OSRNewSpatialReference(nullptr));
    OSRImportFromEPSG(wgs84.h, 4326);
    OSRSetAxisMappingStrategy(wgs84.h, OAMS_TRADITIONAL_GIS_ORDER);

    SrsHandle rasterSrs;
    bool rasterIsGeographic = false;
    if (!projection.empty()) {
        rasterSrs.h = OSRNewSpatialReference(nullptr);
        char *wktPtr = const_cast<char *>(projection.c_str());
        if (OSRImportFromWkt(rasterSrs.h, &wktPtr) != OGRERR_NONE) {
            OSRDestroySpatialReference(rasterSrs.h);
            rasterSrs.h = nullptr;
        } else {
            OSRSetAxisMappingStrategy(rasterSrs.h, OAMS_TRADITIONAL_GIS_ORDER);
            rasterIsGeographic = (OSRIsGeographic(rasterSrs.h) != 0);
        }
    }

    CtHandle wgsToRaster;
    if (rasterSrs.h) {
        wgsToRaster.h = OCTNewCoordinateTransformation(wgs84.h, rasterSrs.h);
    }

    // --- Transform vertices WGS84 -> raster map coords ---
    std::vector<double> mapX(nVertices);
    std::vector<double> mapY(nVertices);
    for (int i = 0; i < nVertices; i++) {
        double mx = line->getX(i); // lon
        double my = line->getY(i); // lat
        if (wgsToRaster.h) {
            if (!OCTTransform(wgsToRaster.h, 1, &mx, &my, nullptr))
                throw AppException("Cannot transform profile vertex to raster CRS");
        }
        mapX[i] = mx;
        mapY[i] = my;
    }

    // --- Segment lengths in meters + total length ---
    std::vector<double> segLenMeters(nVertices - 1, 0.0);
    double totalLengthMeters = 0.0;
    for (int i = 0; i < nVertices - 1; i++) {
        double len;
        if (!rasterSrs.h || rasterIsGeographic) {
            len = haversineMeters(line->getX(i), line->getY(i),
                                  line->getX(i + 1), line->getY(i + 1));
        } else {
            const double dx = mapX[i + 1] - mapX[i];
            const double dy = mapY[i + 1] - mapY[i];
            len = std::sqrt(dx * dx + dy * dy); // assume linear unit = meter
        }
        segLenMeters[i] = len;
        totalLengthMeters += len;
    }

    // Degenerate: all vertices coincide.
    if (totalLengthMeters <= 0.0)
        throw InvalidArgsException("LineString has zero length");

    // --- Value sampling helper ---
    auto isNoDataValue = [&](double v) {
        if (!std::isfinite(v)) return true;
        return hasNoData && std::abs(v - noData) < 1e-6;
    };

    auto applyThermal = [&](double v) -> double {
        if (!needsThermalCalib || !cal.valid) return v;
        return rawToTemperature(static_cast<uint16_t>(v), cal);
    };

    // --- Generate equispaced samples ---
    json samplesArr = json::array();
    double vMin = std::numeric_limits<double>::max();
    double vMax = std::numeric_limits<double>::lowest();
    double vSum = 0.0;
    int vCount = 0;

    for (int s = 0; s < samples; s++) {
        const double t = (samples == 1) ? 0.0
                                        : static_cast<double>(s) / (samples - 1);
        const double targetDist = t * totalLengthMeters;

        // Locate containing segment.
        int segIdx = nVertices - 2;
        double acc = 0.0;
        for (int i = 0; i < nVertices - 1; i++) {
            if (acc + segLenMeters[i] >= targetDist) {
                segIdx = i;
                break;
            }
            acc += segLenMeters[i];
        }

        double frac = (segLenMeters[segIdx] > 0)
                          ? (targetDist - acc) / segLenMeters[segIdx]
                          : 0.0;
        frac = std::clamp(frac, 0.0, 1.0);

        // Interpolate position in map coords and lon/lat for the sample.
        const double smx = mapX[segIdx] + frac * (mapX[segIdx + 1] - mapX[segIdx]);
        const double smy = mapY[segIdx] + frac * (mapY[segIdx + 1] - mapY[segIdx]);
        const double lon = line->getX(segIdx) +
                           frac * (line->getX(segIdx + 1) - line->getX(segIdx));
        const double lat = line->getY(segIdx) +
                           frac * (line->getY(segIdx + 1) - line->getY(segIdx));

        json sampleObj;
        sampleObj["distance"] = targetDist;
        sampleObj["lon"] = lon;
        sampleObj["lat"] = lat;

        // Map -> pixel (fractional).
        double px, py;
        GDALApplyGeoTransform(invGt, smx, smy, &px, &py);

        if (px < 0 || py < 0 || px >= rasterW || py >= rasterH) {
            sampleObj["value"] = nullptr;
            samplesArr.push_back(sampleObj);
            continue;
        }

        // Bilinear sampling at fractional pixel center (subtract 0.5 to get
        // the center of pixel (0,0) at coord (0.5, 0.5) convention).
        const double cx = std::clamp(px - 0.5, 0.0, static_cast<double>(rasterW - 1));
        const double cy = std::clamp(py - 0.5, 0.0, static_cast<double>(rasterH - 1));
        const int x0 = static_cast<int>(std::floor(cx));
        const int y0 = static_cast<int>(std::floor(cy));
        const int x1 = std::min(x0 + 1, rasterW - 1);
        const int y1 = std::min(y0 + 1, rasterH - 1);
        const double fx = cx - x0;
        const double fy = cy - y0;

        const int blockW = x1 - x0 + 1;
        const int blockH = y1 - y0 + 1;
        float block[4] = {0.f, 0.f, 0.f, 0.f};

        if (GDALRasterIO(hBand, GF_Read, x0, y0, blockW, blockH,
                         block, blockW, blockH, GDT_Float32, 0, 0) != CE_None) {
            sampleObj["value"] = nullptr;
            samplesArr.push_back(sampleObj);
            continue;
        }

        // Unpack (supports 1x1, 1x2, 2x1, 2x2 edge cases).
        const double v00 = block[0];
        const double v10 = (blockW > 1) ? block[1] : v00;
        const double v01 = (blockH > 1) ? block[blockW] : v00;
        const double v11 = (blockW > 1 && blockH > 1) ? block[blockW + 1]
                          : (blockW > 1 ? v10 : (blockH > 1 ? v01 : v00));

        if (isNoDataValue(v00) || isNoDataValue(v10) ||
            isNoDataValue(v01) || isNoDataValue(v11)) {
            sampleObj["value"] = nullptr;
            samplesArr.push_back(sampleObj);
            continue;
        }

        // Apply thermal calibration to raw UInt16 values before interpolating
        // so we operate in physical units (otherwise blending non-linear
        // Planck values would be wrong at edges).
        const double c00 = applyThermal(v00);
        const double c10 = applyThermal(v10);
        const double c01 = applyThermal(v01);
        const double c11 = applyThermal(v11);

        const double top = c00 + fx * (c10 - c00);
        const double bot = c01 + fx * (c11 - c01);
        const double value = top + fy * (bot - top);

        sampleObj["value"] = value;
        samplesArr.push_back(sampleObj);

        if (std::isfinite(value)) {
            vMin = std::min(vMin, value);
            vMax = std::max(vMax, value);
            vSum += value;
            vCount++;
        }
    }

    json result;
    result["samples"] = samplesArr;
    result["totalLength"] = totalLengthMeters;
    if (vCount > 0) {
        result["min"] = vMin;
        result["max"] = vMax;
        result["mean"] = vSum / vCount;
    } else {
        result["min"] = nullptr;
        result["max"] = nullptr;
        result["mean"] = nullptr;
    }
    result["unit"] = unit;
    result["sampleCount"] = samples;
    result["validCount"] = vCount;
    result["isThermal"] = isThermal;
    return result.dump();
}

} // namespace ddb
