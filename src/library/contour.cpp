/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "contour.h"
#include "exceptions.h"
#include "json.h"
#include "logger.h"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <ogr_spatialref.h>
#include <ogr_geometry.h>
#include <cpl_conv.h>
#include <cpl_string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace ddb {

namespace {

// Hard limits to keep the operation bounded server-side.
constexpr int    MAX_LEVELS               = 1000;
constexpr int    DEFAULT_COUNT            = 20;
constexpr double MIN_INTERVAL             = 1e-9;

// --- RAII helpers ---------------------------------------------------------

struct DatasetGuard {
    GDALDatasetH h = nullptr;
    ~DatasetGuard() { if (h) GDALClose(h); }
};

struct SrsHandle {
    OGRSpatialReferenceH h = nullptr;
    SrsHandle() = default;
    explicit SrsHandle(OGRSpatialReferenceH handle) : h(handle) {}
    ~SrsHandle() { if (h) OSRDestroySpatialReference(h); }
    SrsHandle(const SrsHandle &) = delete;
    SrsHandle &operator=(const SrsHandle &) = delete;
};

struct CtHandle {
    OGRCoordinateTransformationH h = nullptr;
    ~CtHandle() { if (h) OCTDestroyCoordinateTransformation(h); }
};

// Convert an OGRGeometry to GeoJSON coordinates (LineString or
// MultiLineString) and append the resulting Feature(s) to `out`.
void appendGeometryAsFeature(json &out, OGRGeometry *geom, double elev) {
    if (!geom) return;

    auto pushLine = [&](OGRLineString *ls) {
        if (!ls) return;
        json coords = json::array();
        const int n = ls->getNumPoints();
        for (int i = 0; i < n; i++) {
            coords.push_back({ ls->getX(i), ls->getY(i) });
        }
        if (coords.size() < 2) return;

        json feature;
        feature["type"] = "Feature";
        feature["properties"] = { {"elev", elev} };
        feature["geometry"] = {
            {"type", "LineString"},
            {"coordinates", coords}
        };
        out.push_back(feature);
    };

    const OGRwkbGeometryType t = wkbFlatten(geom->getGeometryType());
    if (t == wkbLineString) {
        pushLine(geom->toLineString());
    } else if (t == wkbMultiLineString) {
        OGRMultiLineString *mls = geom->toMultiLineString();
        for (int i = 0; i < mls->getNumGeometries(); i++) {
            pushLine(mls->getGeometryRef(i)->toLineString());
        }
    } else if (t == wkbGeometryCollection) {
        OGRGeometryCollection *gc = geom->toGeometryCollection();
        for (int i = 0; i < gc->getNumGeometries(); i++) {
            appendGeometryAsFeature(out, gc->getGeometryRef(i), elev);
        }
    }
}

// Resolve which interval to use. Returns positive value or throws.
double resolveInterval(const ContourOptions &opts,
                       double bandMin, double bandMax) {
    if (opts.interval.has_value()) {
        if (*opts.interval < MIN_INTERVAL)
            throw InvalidArgsException("Contour interval must be > 0");
        return *opts.interval;
    }

    int count = opts.count.value_or(DEFAULT_COUNT);
    if (count <= 0)
        throw InvalidArgsException("Contour count must be > 0");
    if (count > MAX_LEVELS) count = MAX_LEVELS;

    if (!std::isfinite(bandMin) || !std::isfinite(bandMax) || bandMax <= bandMin)
        throw GDALException("Cannot derive interval: invalid raster statistics");

    const double range = bandMax - bandMin;
    return range / static_cast<double>(count);
}

} // anonymous namespace

std::string generateContoursJson(const std::string &rasterPath,
                                 const ContourOptions &options) {
    if (!options.interval.has_value() && !options.count.has_value())
        throw InvalidArgsException(
            "Either 'interval' or 'count' must be specified");

    if (options.minElev.has_value() && options.maxElev.has_value() &&
        *options.minElev >= *options.maxElev)
        throw InvalidArgsException("'minElev' must be less than 'maxElev'");

    if (options.simplifyTolerance < 0.0)
        throw InvalidArgsException("'simplifyTolerance' must be >= 0");

    // --- Open raster ------------------------------------------------------
    DatasetGuard dsGuard;
    dsGuard.h = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!dsGuard.h)
        throw GDALException("Cannot open raster: " + rasterPath);

    double gt[6];
    if (GDALGetGeoTransform(dsGuard.h, gt) != CE_None)
        throw GDALException("Raster has no geotransform: " + rasterPath);

    const int bandCount = GDALGetRasterCount(dsGuard.h);
    if (options.bandIndex < 1 || options.bandIndex > bandCount)
        throw InvalidArgsException("Invalid band index: " +
                                   std::to_string(options.bandIndex));

    GDALRasterBandH hBand = GDALGetRasterBand(dsGuard.h, options.bandIndex);
    if (!hBand) throw GDALException("Cannot access raster band");

    const char *unitCStr = GDALGetRasterUnitType(hBand);
    const std::string unit = unitCStr ? unitCStr : "";

    // Band statistics drive interval-from-count and JSON metadata.
    double bMin = 0.0, bMax = 0.0, bMean = 0.0, bStdDev = 0.0;
    const CPLErr statErr = GDALGetRasterStatistics(hBand, FALSE, TRUE,
                                                   &bMin, &bMax, &bMean, &bStdDev);
    if (statErr != CE_None)
        throw GDALException("Cannot compute raster statistics");

    const double interval = resolveInterval(options, bMin, bMax);

    // --- Build in-memory OGR layer to collect contours --------------------
    SrsHandle rasterSrs;
    bool rasterHasSrs = false;
    {
        const char *projRef = GDALGetProjectionRef(dsGuard.h);
        if (projRef && projRef[0] != '\0') {
            rasterSrs.h = OSRNewSpatialReference(nullptr);
            char *wktPtr = const_cast<char *>(projRef);
            if (OSRImportFromWkt(rasterSrs.h, &wktPtr) != OGRERR_NONE) {
                OSRDestroySpatialReference(rasterSrs.h);
                rasterSrs.h = nullptr;
            } else {
                OSRSetAxisMappingStrategy(rasterSrs.h, OAMS_TRADITIONAL_GIS_ORDER);
                rasterHasSrs = true;
            }
        }
    }

    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (!memDrv) throw GDALException("MEM OGR driver not available");

    GDALDatasetH hMemDs = GDALCreate(memDrv, "contours_mem", 0, 0, 0, GDT_Unknown, nullptr);
    if (!hMemDs) throw GDALException("Cannot create in-memory dataset");
    DatasetGuard memGuard{hMemDs};

    OGRLayerH hLayer = GDALDatasetCreateLayer(hMemDs, "contours",
                                              rasterHasSrs ? rasterSrs.h : nullptr,
                                              wkbLineString, nullptr);
    if (!hLayer) throw GDALException("Cannot create contour layer");

    constexpr int idFieldIdx   = 0;
    constexpr int elevFieldIdx = 1;

    OGRFieldDefnH fId = OGR_Fld_Create("id", OFTInteger);
    OGR_L_CreateField(hLayer, fId, TRUE);
    OGR_Fld_Destroy(fId);

    OGRFieldDefnH fElev = OGR_Fld_Create("elev", OFTReal);
    OGR_L_CreateField(hLayer, fElev, TRUE);
    OGR_Fld_Destroy(fElev);

    // --- Run GDAL contour generator --------------------------------------
    int hasNoData = FALSE;
    const double noData = GDALGetRasterNoDataValue(hBand, &hasNoData);

    char **opts = nullptr;
    opts = CSLSetNameValue(opts, "ELEV_FIELD",
                           std::to_string(elevFieldIdx).c_str());
    opts = CSLSetNameValue(opts, "ID_FIELD",
                           std::to_string(idFieldIdx).c_str());
    opts = CSLSetNameValue(opts, "LEVEL_INTERVAL",
                           std::to_string(interval).c_str());
    opts = CSLSetNameValue(opts, "LEVEL_BASE",
                           std::to_string(options.baseOffset).c_str());
    if (hasNoData) {
        opts = CSLSetNameValue(opts, "NODATA",
                               std::to_string(noData).c_str());
    }

    const CPLErr cErr = GDALContourGenerateEx(hBand, hLayer, opts,
                                              nullptr, nullptr);
    CSLDestroy(opts);

    if (cErr != CE_None)
        throw GDALException("GDAL contour generation failed");

    // --- Build coordinate transform back to WGS84 -------------------------
    SrsHandle wgs84(OSRNewSpatialReference(nullptr));
    OSRImportFromEPSG(wgs84.h, 4326);
    OSRSetAxisMappingStrategy(wgs84.h, OAMS_TRADITIONAL_GIS_ORDER);

    CtHandle rasterToWgs;
    if (rasterHasSrs) {
        rasterToWgs.h = OCTNewCoordinateTransformation(rasterSrs.h, wgs84.h);
        if (!rasterToWgs.h)
            throw GDALException("Cannot build raster -> WGS84 transform");
    }

    // --- Iterate features, simplify, transform, serialize -----------------
    json features = json::array();
    double outMinElev = std::numeric_limits<double>::infinity();
    double outMaxElev = -std::numeric_limits<double>::infinity();
    int kept = 0;

    OGR_L_ResetReading(hLayer);
    while (true) {
        OGRFeatureH hFeat = OGR_L_GetNextFeature(hLayer);
        if (!hFeat) break;

        struct FeatGuard {
            OGRFeatureH h;
            ~FeatGuard() { if (h) OGR_F_Destroy(h); }
        } featGuard{hFeat};

        const double elev = OGR_F_GetFieldAsDouble(hFeat, elevFieldIdx);

        if (options.minElev.has_value() && elev < *options.minElev) continue;
        if (options.maxElev.has_value() && elev > *options.maxElev) continue;

        OGRGeometryH hGeom = OGR_F_GetGeometryRef(hFeat);
        if (!hGeom) continue;

        // Optional simplification in raster CRS units.
        OGRGeometry *workGeom = reinterpret_cast<OGRGeometry *>(hGeom);
        std::unique_ptr<OGRGeometry, void(*)(OGRGeometry *)> ownedSimplified(
            nullptr,
            [](OGRGeometry *g) { if (g) OGRGeometryFactory::destroyGeometry(g); });
        if (options.simplifyTolerance > 0.0) {
            OGRGeometry *simp = workGeom->SimplifyPreserveTopology(
                options.simplifyTolerance);
            if (simp) {
                ownedSimplified.reset(simp);
                workGeom = simp;
            }
        }

        // Reproject in-place to WGS84 (operate on a clone so we don't
        // mutate features still owned by the layer).
        std::unique_ptr<OGRGeometry, void(*)(OGRGeometry *)> ownedTransformed(
            nullptr,
            [](OGRGeometry *g) { if (g) OGRGeometryFactory::destroyGeometry(g); });
        if (rasterToWgs.h) {
            OGRGeometry *clone = workGeom->clone();
            if (!clone) continue;
            if (clone->transform(reinterpret_cast<OGRCoordinateTransformation *>(
                    rasterToWgs.h)) != OGRERR_NONE) {
                OGRGeometryFactory::destroyGeometry(clone);
                continue;
            }
            ownedTransformed.reset(clone);
            workGeom = clone;
        }

        const size_t before = features.size();
        appendGeometryAsFeature(features, workGeom, elev);
        if (features.size() > before) {
            kept++;
            outMinElev = std::min(outMinElev, elev);
            outMaxElev = std::max(outMaxElev, elev);
        }

        if (kept >= MAX_LEVELS * 100) {
            LOGD << "Contour generation hit feature cap";
            break;
        }
    }

    // --- Build output JSON -----------------------------------------------
    json result;
    result["type"] = "FeatureCollection";
    result["features"] = features;
    result["interval"] = interval;
    result["baseOffset"] = options.baseOffset;
    if (kept > 0) {
        result["min"] = outMinElev;
        result["max"] = outMaxElev;
    } else {
        result["min"] = nullptr;
        result["max"] = nullptr;
    }
    result["featureCount"] = static_cast<int>(features.size());
    result["unit"] = unit;
    result["rasterMin"] = bMin;
    result["rasterMax"] = bMax;

    return result.dump();
}

} // namespace ddb
