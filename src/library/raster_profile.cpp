/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_profile.h"
#include "dem.h"
#include "json.h"
#include "mathutil.h"
#include "raster_analysis.h"
#include "thermal.h"
#include "sensorprofile.h"
#include "exceptions.h"
#include "gdal_raii.h"
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
#include <optional>
#include <vector>

namespace ddb {

namespace {

constexpr int DEFAULT_SAMPLES = 256;
constexpr int MIN_SAMPLES = 2;
constexpr int MAX_SAMPLES = 4096;
constexpr double WGS84_RADIUS_M = 6378137.0;

// Haversine distance on WGS84 ellipsoid approximated as a sphere (meters).
double haversineMeters(double lon1, double lat1, double lon2, double lat2) {
    const double dLat = (lat2 - lat1) * ddb::kDegToRad;
    const double dLon = (lon2 - lon1) * ddb::kDegToRad;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(lat1 * ddb::kDegToRad) * std::cos(lat2 * ddb::kDegToRad) *
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
    } catch (const std::exception &e) {
        LOGD << "SensorProfileManager failed in detectIsThermalProfile: " << e.what();
    }

    return isThermalImage(filePath) || isDirectTemperatureRaster(filePath);
}

// The profile line (GeoJSON, WGS84 lon/lat); throws InvalidArgsException when it is not a
// LineString of at least 2 points
OGRGeometryUniquePtr parseProfileLine(const std::string &geoJsonLineString) {
    OGRGeometryH hRawGeom = OGR_G_CreateGeometryFromJson(geoJsonLineString.c_str());
    if (hRawGeom == nullptr)
        throw InvalidArgsException("Cannot parse GeoJSON geometry");

    OGRGeometryUniquePtr geom(OGRGeometry::FromHandle(hRawGeom));
    const OGRwkbGeometryType gType = wkbFlatten(geom->getGeometryType());
    if (gType != wkbLineString)
        throw InvalidArgsException("Profile geometry must be a LineString");

    if (geom->toLineString()->getNumPoints() < 2)
        throw InvalidArgsException("LineString must have at least 2 points");
    return geom;
}

// The line in raster map coordinates, with the length of each segment in meters
struct ProfilePath {
    std::vector<double> mapX, mapY;
    std::vector<double> segLenMeters;
    double totalLengthMeters = 0.0;
};

ProfilePath buildPath(const DemContext &dem, const OGRLineString &line) {
    const int nVertices = line.getNumPoints();
    ProfilePath path;

    // --- Setup coordinate transforms (WGS84 <-> raster CRS) ---
    const auto wgsToRaster = dem.wgs84ToDem();

    // --- Transform vertices WGS84 -> raster map coords ---
    path.mapX.resize(nVertices);
    path.mapY.resize(nVertices);
    for (int i = 0; i < nVertices; i++) {
        double mx = line.getX(i); // lon
        double my = line.getY(i); // lat
        if (wgsToRaster) {
            if (!wgsToRaster->tryTransform(1, &mx, &my))
                throw AppException("Cannot transform profile vertex to raster CRS");
        }
        path.mapX[i] = mx;
        path.mapY[i] = my;
    }

    // --- Segment lengths in meters + total length ---
    path.segLenMeters.assign(nVertices - 1, 0.0);
    for (int i = 0; i < nVertices - 1; i++) {
        double len;
        if (!dem.srs || dem.isGeographic) {
            len = haversineMeters(line.getX(i), line.getY(i),
                                  line.getX(i + 1), line.getY(i + 1));
        } else {
            const double dx = path.mapX[i + 1] - path.mapX[i];
            const double dy = path.mapY[i + 1] - path.mapY[i];
            len = std::sqrt(dx * dx + dy * dy); // assume linear unit = meter
        }
        path.segLenMeters[i] = len;
        path.totalLengthMeters += len;
    }

    // Degenerate: all vertices coincide.
    if (path.totalLengthMeters <= 0.0)
        throw InvalidArgsException("LineString has zero length");
    return path;
}

// The value of the raster at a point of the profile: bilinear, in physical units for a thermal
// raster
class ProfileSampler {
    const DemContext &dem;
    double invGt[6];
    std::optional<ThermalCalibration> thermal;

    double applyThermal(double v) const {
        if (!thermal) return v;
        return rawToTemperature(static_cast<uint16_t>(v), *thermal);
    }

    bool isNoDataValue(float v) const {
        return !std::isfinite(v) || isNodata(v, dem.nodata);
    }

public:
    // Throws when the geotransform of the raster cannot be inverted
    explicit ProfileSampler(const DemContext &raster) : dem(raster) {
        if (!GDALInvGeoTransform(dem.gt, invGt))
            throw AppException("Cannot invert geotransform");
    }

    // The raster holds raw UInt16 values of a thermal sensor: report them as temperatures
    void calibrate(const ThermalCalibration &cal) {
        if (cal.valid) thermal = cal;
    }

    // Nothing when the point is outside the raster, the read fails or a sample is nodata
    std::optional<double> valueAt(double smx, double smy) const {
        const int rasterW = dem.width;
        const int rasterH = dem.height;

        // Map -> pixel (fractional).
        double px, py;
        GDALApplyGeoTransform(invGt, smx, smy, &px, &py);

        if (px < 0 || py < 0 || px >= rasterW || py >= rasterH)
            return std::nullopt;

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

        if (GDALRasterIO(dem.band, GF_Read, x0, y0, blockW, blockH,
                         block, blockW, blockH, GDT_Float32, 0, 0) != CE_None)
            return std::nullopt;

        // Unpack (supports 1x1, 1x2, 2x1, 2x2 edge cases).
        const float v00 = block[0];
        const float v10 = (blockW > 1) ? block[1] : v00;
        const float v01 = (blockH > 1) ? block[blockW] : v00;
        const float v11 = (blockW > 1 && blockH > 1) ? block[blockW + 1]
                          : (blockW > 1 ? v10 : (blockH > 1 ? v01 : v00));

        if (isNoDataValue(v00) || isNoDataValue(v10) ||
            isNoDataValue(v01) || isNoDataValue(v11))
            return std::nullopt;

        // Apply thermal calibration to raw UInt16 values before interpolating
        // so we operate in physical units (otherwise blending non-linear
        // Planck values would be wrong at edges).
        const double c00 = applyThermal(v00);
        const double c10 = applyThermal(v10);
        const double c01 = applyThermal(v01);
        const double c11 = applyThermal(v11);

        const double top = c00 + fx * (c10 - c00);
        const double bot = c01 + fx * (c11 - c01);
        return top + fy * (bot - top);
    }
};

} // anonymous namespace

std::string getRasterProfileJson(const std::string &filePath,
                                 const std::string &geoJsonLineString,
                                 int samples) {
    // Clamp / defaults.
    if (samples < MIN_SAMPLES) samples = DEFAULT_SAMPLES;
    if (samples > MAX_SAMPLES) samples = MAX_SAMPLES;

    // --- Parse GeoJSON LineString (expected WGS84 lon/lat) ---
    const OGRGeometryUniquePtr geom = parseProfileLine(geoJsonLineString);
    const OGRLineString *line = geom->toLineString();
    const int nVertices = line->getNumPoints();

    // --- Open raster and inspect metadata ---
    const DemContext dem = openDem(filePath);
    ProfileSampler sampler(dem);

    GDALRasterBandH hBand = dem.band;
    const GDALDataType dt = GDALGetRasterDataType(hBand);

    const char *unitCStr = GDALGetRasterUnitType(hBand);
    std::string unit = unitCStr ? unitCStr : "";

    const bool isThermal = detectIsThermalProfile(filePath);
    ThermalCalibration cal;
    const bool needsThermalCalib = isThermal && dt == GDT_UInt16;
    if (needsThermalCalib) {
        cal = extractThermalCalibration(filePath);
        sampler.calibrate(cal);
    }

    if (unit.empty() && isThermal) unit = "\xC2\xB0\x43"; // UTF-8 "°C"

    const ProfilePath path = buildPath(dem, *line);
    const std::vector<double> &mapX = path.mapX;
    const std::vector<double> &mapY = path.mapY;
    const std::vector<double> &segLenMeters = path.segLenMeters;
    const double totalLengthMeters = path.totalLengthMeters;

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

        const std::optional<double> value = sampler.valueAt(smx, smy);
        if (!value) {
            sampleObj["value"] = nullptr;
            samplesArr.push_back(sampleObj);
            continue;
        }

        sampleObj["value"] = *value;
        samplesArr.push_back(sampleObj);

        if (std::isfinite(*value)) {
            vMin = std::min(vMin, *value);
            vMax = std::max(vMax, *value);
            vSum += *value;
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
