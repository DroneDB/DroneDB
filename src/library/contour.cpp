/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "contour.h"
#include "cancellation.h"
#include "dem.h"
#include "exceptions.h"
#include "gdal_raii.h"
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

constexpr int idFieldIdx   = 0;
constexpr int elevFieldIdx = 1;

void validateOptions(const ContourOptions &options) {
    if (!options.interval.has_value() && !options.count.has_value())
        throw InvalidArgsException(
            "Either 'interval' or 'count' must be specified");

    if (options.minElev.has_value() && options.maxElev.has_value() &&
        *options.minElev >= *options.maxElev)
        throw InvalidArgsException("'minElev' must be less than 'maxElev'");

    if (options.simplifyTolerance < 0.0)
        throw InvalidArgsException("'simplifyTolerance' must be >= 0");
}

// The in-memory OGR layer the contours are generated into, with the dataset that owns it
struct ContourLayer {
    GDALDatasetUniquePtr dataset;
    OGRLayerH layer = nullptr;
};

// Adds a field to the layer, or throws: a layer without it would shift the fields the generator
// and the readers address by index
void addField(OGRLayerH layer, const char *name, OGRFieldType type) {
    OGRFieldDefnH defn = OGR_Fld_Create(name, type);
    const OGRErr created = OGR_L_CreateField(layer, defn, TRUE);
    OGR_Fld_Destroy(defn);
    if (created != OGRERR_NONE)
        throw GDALException(std::string("Cannot create the field '") + name + "' of the contour layer");
}

// Runs the GDAL contour generator over the band into a new in-memory layer in the CRS of the raster
ContourLayer generateContourLayer(const DemContext &dem, double interval, const ContourOptions &options) {
    const bool rasterHasSrs = dem.srs != nullptr;

    GDALDriverH memDrv = GDALGetDriverByName("Memory");
    if (!memDrv) throw GDALException("Memory OGR driver not available");

    ContourLayer out;
    out.dataset = adoptDataset(
        GDALCreate(memDrv, "contours_mem", 0, 0, 0, GDT_Unknown, nullptr));
    if (!out.dataset) throw GDALException("Cannot create in-memory dataset");
    GDALDatasetH hMemDs = datasetHandle(out.dataset);

    OGRLayerH hLayer = GDALDatasetCreateLayer(hMemDs, "contours",
                                              rasterHasSrs ? dem.srs.get() : nullptr,
                                              wkbLineString, nullptr);
    if (!hLayer) throw GDALException("Cannot create contour layer");
    out.layer = hLayer;

    // In this order: the generator writes into them by index (idFieldIdx, elevFieldIdx)
    addField(hLayer, "id", OFTInteger);
    addField(hLayer, "elev", OFTReal);

    // --- Run GDAL contour generator --------------------------------------

    CPLStringList opts;
    opts.SetNameValue("ELEV_FIELD", std::to_string(elevFieldIdx).c_str());
    opts.SetNameValue("ID_FIELD", std::to_string(idFieldIdx).c_str());
    opts.SetNameValue("LEVEL_INTERVAL", std::to_string(interval).c_str());
    opts.SetNameValue("LEVEL_BASE", std::to_string(options.baseOffset).c_str());
    if (dem.nodata.has) {
        opts.SetNameValue("NODATA", std::to_string(dem.nodata.value).c_str());
    }

    // The generation stops when the call is canceled (DQ-15)
    cancel::GdalProgress progress;
    const CPLErr cErr = GDALContourGenerateEx(dem.band, hLayer, opts.List(),
                                              progress.function(), progress.data());

    if (cErr != CE_None) {
        cancel::throwIfCanceled();
        throw GDALException("GDAL contour generation failed");
    }
    return out;
}

// The contour lines as GeoJSON features: filtered by elevation, simplified, in WGS84
struct ContourFeatures {
    json features = json::array();
    double minElev = std::numeric_limits<double>::infinity();
    double maxElev = -std::numeric_limits<double>::infinity();
    int kept = 0;
};

ContourFeatures collectFeatures(OGRLayerH hLayer, const DemContext &dem, const ContourOptions &options) {
    // --- Build coordinate transform back to WGS84 -------------------------
    const auto rasterToWgs = dem.demToWgs84();

    // --- Iterate features, simplify, transform, serialize -----------------
    ContourFeatures out;
    json &features = out.features;

    OGR_L_ResetReading(hLayer);
    while (true) {
        cancel::throwIfCanceled();
        const FeaturePtr feature(OGR_L_GetNextFeature(hLayer));
        if (!feature) break;
        OGRFeatureH hFeat = feature.get();

        const double elev = OGR_F_GetFieldAsDouble(hFeat, elevFieldIdx);

        if (options.minElev.has_value() && elev < *options.minElev) continue;
        if (options.maxElev.has_value() && elev > *options.maxElev) continue;

        OGRGeometryH hGeom = OGR_F_GetGeometryRef(hFeat);
        if (!hGeom) continue;

        // Optional simplification in raster CRS units.
        OGRGeometry *workGeom = OGRGeometry::FromHandle(hGeom);
        OGRGeometryUniquePtr ownedSimplified;
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
        OGRGeometryUniquePtr ownedTransformed;
        if (rasterToWgs) {
            ownedTransformed.reset(workGeom->clone());
            if (!ownedTransformed) continue;
            if (ownedTransformed->transform(rasterToWgs->get()) != OGRERR_NONE) {
                continue;
            }
            workGeom = ownedTransformed.get();
        }

        const size_t before = features.size();
        appendGeometryAsFeature(features, workGeom, elev);
        if (features.size() > before) {
            out.kept++;
            out.minElev = std::min(out.minElev, elev);
            out.maxElev = std::max(out.maxElev, elev);
        }

        if (out.kept >= MAX_LEVELS * 100) {
            LOGD << "Contour generation hit feature cap";
            break;
        }
    }
    return out;
}

} // anonymous namespace

std::string generateContoursJson(const std::string &rasterPath,
                                 const ContourOptions &options) {
    validateOptions(options);
    cancel::throwIfCanceled();

    // --- Open raster ------------------------------------------------------
    const DemContext dem = openDem(rasterPath, options.bandIndex);
    GDALRasterBandH hBand = dem.band;

    const char *unitCStr = GDALGetRasterUnitType(hBand);
    const std::string unit = unitCStr ? unitCStr : "";

    // Band statistics drive interval-from-count and JSON metadata.
    // (the statistics stored with the raster, else a pass over it that stops when the call is canceled)
    double bMin = 0.0, bMax = 0.0, bMean = 0.0, bStdDev = 0.0;
    CPLErr statErr = GDALGetRasterStatistics(hBand, FALSE, FALSE, &bMin, &bMax, &bMean, &bStdDev);
    if (statErr != CE_None) {
        cancel::GdalProgress progress;
        statErr = GDALComputeRasterStatistics(hBand, FALSE, &bMin, &bMax, &bMean, &bStdDev,
                                              progress.function(), progress.data());
    }
    if (statErr != CE_None) {
        cancel::throwIfCanceled();
        throw GDALException("Cannot compute raster statistics");
    }

    const double interval = resolveInterval(options, bMin, bMax);

    // --- Build in-memory OGR layer to collect contours, and read it back -------
    const ContourLayer contours = generateContourLayer(dem, interval, options);
    const ContourFeatures collected = collectFeatures(contours.layer, dem, options);
    const json &features = collected.features;

    // --- Build output JSON -----------------------------------------------
    json result;
    result["type"] = "FeatureCollection";
    result["features"] = features;
    result["interval"] = interval;
    result["baseOffset"] = options.baseOffset;
    if (collected.kept > 0) {
        result["min"] = collected.minElev;
        result["max"] = collected.maxElev;
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
