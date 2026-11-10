/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "contour.h"
#include "cancellation.h"
#include "dem.h"
#include "raster_stats.h"
#include "thermal.h"
#include "vegetation.h"
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
#include <cstddef>
#include <cstdio>
#include <functional>
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

// Reports the progress of a generation to ContourOptions::progress and turns its request to stop into a
// cancel of the call (the checkpoints then throw), as the raster export does. It never throws: GDAL
// calls it through its progress callback.
class ContourSession {
public:
    explicit ContourSession(const std::function<bool(double, const char *)> &progress) : progress_(progress) {}

    // Statistics, then the GDAL generator, then the lines read back, each in its share of [0,1]
    static constexpr double kLevelsFrom = 0.05;
    static constexpr double kLinesFrom = 0.70;

    void report(double fraction, const char *phase) {
        if (progress_ && progress_(std::clamp(fraction, 0.0, 1.0), phase))
            callToken_.cancel("Contour generation canceled by the progress callback");
    }

private:
    const std::function<bool(double, const char *)> &progress_;
    cancel::CallToken callToken_;
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

// A number for a GDAL option with every digit of the double: std::to_string writes 6 decimals, so a NoData
// value of 1e-7 became "0.000000" and a level interval of 1e-7 zero
std::string optionNumber(double value) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.17g", value);
    return text;
}

// The band of a DEM as the contour generator reads it when the band has an alpha band or a dataset mask: the
// raw samples (Float64) with NaN wherever a pixel has no value in the band-value model (FR-XC-24): NoData
// (the override, else the value of the band), masked or not finite. The generator leaves NaN pixels out, as it
// does NoData, but it knows no mask: without this view a transparent area of a mosaic drew contours. It reads
// the band in blocks of rows as the generator asks for them (no copy of the raster in memory). The DEM must
// outlive the view.
class NoValueAsNanDataset : public GDALDataset {
public:
    explicit NoValueAsNanDataset(const DemContext &dem) {
        nRasterXSize = dem.width;
        nRasterYSize = dem.height;
        std::copy(dem.gt, dem.gt + 6, gt_);
        SetBand(1, new Band(this, dem));
    }

#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 12, 0)
    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override {
        gt = GDALGeoTransform(gt_);
        return CE_None;
    }
#else
    CPLErr GetGeoTransform(double *gt) override {
        std::copy(gt_, gt_ + 6, gt);
        return CE_None;
    }
#endif

private:
    class Band : public GDALRasterBand {
    public:
        Band(NoValueAsNanDataset *ds, const DemContext &dem) : dem_(dem) {
            poDS = ds;
            nBand = 1;
            eDataType = GDT_Float64;
            nRasterXSize = dem.width;
            nRasterYSize = dem.height;
            nBlockXSize = dem.width;
            // About 8 MB of samples per block
            nBlockYSize = std::max(1, std::min(256, static_cast<int>((1 << 20) / std::max(1, dem.width))));
            floatSamples_ = GDALGetRasterDataType(dem.band) == GDT_Float32;
        }

    protected:
        CPLErr IReadBlock(int, int nBlockYOff, void *pImage) override {
            const int y = nBlockYOff * nBlockYSize;
            const int rows = std::min(nBlockYSize, nRasterYSize - y);
            if (rows <= 0) return CE_None;
            const size_t count = static_cast<size_t>(rows) * nRasterXSize;
            double *samples = static_cast<double *>(pImage);
            if (GDALRasterIO(dem_.band, GF_Read, 0, y, nRasterXSize, rows, samples, nRasterXSize, rows,
                             GDT_Float64, 0, 0) != CE_None)
                return CE_Failure;
            std::vector<uint8_t> mask;
            if (dem_.maskBand != nullptr) {
                mask.resize(count);
                if (GDALRasterIO(dem_.maskBand, GF_Read, 0, y, nRasterXSize, rows, mask.data(), nRasterXSize, rows,
                                 GDT_Byte, 0, 0) != CE_None)
                    return CE_Failure;
            }
            const double nan = std::numeric_limits<double>::quiet_NaN();
            for (size_t i = 0; i < count; i++) {
                const double v = samples[i];
                // A Float32 band holds its NoData value rounded to float (GDAL's rule, and DemContext::readRaw's)
                const bool nodata = floatSamples_ ? isNodata(static_cast<float>(v), dem_.nodata)
                                                  : isNodata(v, dem_.nodata);
                if (!std::isfinite(v) || nodata || (!mask.empty() && mask[i] == 0))
                    samples[i] = nan;
            }
            // Rows of the last block below the raster
            std::fill(samples + count, samples + static_cast<size_t>(nBlockYSize) * nBlockXSize, nan);
            return CE_None;
        }

    private:
        const DemContext &dem_;
        bool floatSamples_ = false;
    };

    double gt_[6] = {0, 1, 0, 0, 0, 1};
};

// Runs the GDAL contour generator over the band into a new in-memory layer in the CRS of the raster
ContourLayer generateContourLayer(const DemContext &dem, double interval, const ContourOptions &options,
                                  ContourSession &session) {
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
    // The generator reads the raw samples of the band: the levels (physical values, FR-RAS-04) are
    // converted to raw ones, and the elevations it writes back to physical ones in collectFeatures
    const double rawScale = dem.hasScale && dem.scale != 0.0 ? std::fabs(dem.scale) : 1.0;
    opts.SetNameValue("LEVEL_INTERVAL", optionNumber(interval / rawScale).c_str());
    opts.SetNameValue("LEVEL_BASE", optionNumber(dem.toRaw(options.baseOffset)).c_str());

    // A band with an alpha band or a dataset mask is read through a view in which every pixel without a
    // value is NaN (the generator knows no mask); any other band directly, with its NoData value
    GDALDatasetUniquePtr masked;
    GDALRasterBandH source = dem.band;
    if (dem.maskBand != nullptr) {
        masked.reset(new NoValueAsNanDataset(dem));
        source = GDALGetRasterBand(GDALDataset::ToHandle(masked.get()), 1);
    } else if (dem.nodata.has) {
        opts.SetNameValue("NODATA", optionNumber(dem.nodata.value).c_str());
    }

    // The generation stops when the call is canceled
    cancel::GdalProgress progress([&session](double fraction) {
        session.report(ContourSession::kLevelsFrom +
                           fraction * (ContourSession::kLinesFrom - ContourSession::kLevelsFrom),
                       "levels");
    });
    const CPLErr cErr = GDALContourGenerateEx(source, hLayer, opts.List(),
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
    // The lines were cut at options.maxFeatures: the layer had more
    bool truncated = false;
};

ContourFeatures collectFeatures(OGRLayerH hLayer, const DemContext &dem, const ContourOptions &options,
                                ContourSession &session) {
    // --- Build coordinate transform back to WGS84 -------------------------
    const auto rasterToWgs = dem.demToWgs84();

    // --- Iterate features, simplify, transform, serialize -----------------
    ContourFeatures out;
    json &features = out.features;

    OGR_L_ResetReading(hLayer);
    // A layer in memory knows its count without a pass
    const GIntBig total = std::max<GIntBig>(1, OGR_L_GetFeatureCount(hLayer, TRUE));
    const size_t cap = static_cast<size_t>(std::max(1, options.maxFeatures));
    GIntBig read = 0;
    while (true) {
        if (read % 256 == 0)
            session.report(ContourSession::kLinesFrom +
                               (1.0 - ContourSession::kLinesFrom) * static_cast<double>(read) / static_cast<double>(total),
                           "lines");
        cancel::throwIfCanceled();
        const FeaturePtr feature(OGR_L_GetNextFeature(hLayer));
        if (!feature) break;
        OGRFeatureH hFeat = feature.get();
        read++;

        const double rawElev = OGR_F_GetFieldAsDouble(hFeat, elevFieldIdx);
        // raw * scale + offset carries the noise of the arithmetic (12.000000000000002): 15 digits keep the level
        const double elev = dem.hasScale ? tidyValue(dem.toPhysical(rawElev), GDT_Float64) : rawElev;

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

        // A line that does not fit is not kept: the result is cut here and says so (FR-RAS-11)
        if (features.size() >= cap) {
            LOGD << "Contour generation hit feature cap";
            out.truncated = true;
            break;
        }

        const size_t before = features.size();
        appendGeometryAsFeature(features, workGeom, elev);
        if (features.size() > before) {
            out.kept++;
            out.minElev = std::min(out.minElev, elev);
            out.maxElev = std::max(out.maxElev, elev);
        }

        // The lines of one geometry may pass the cap: the result holds exactly `cap` lines
        if (features.size() > cap) {
            features.erase(features.begin() + static_cast<std::ptrdiff_t>(cap), features.end());
            out.truncated = true;
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
    ContourSession session(options.progress);
    session.report(0.0, "statistics");

    // A thermal formula gives the levels in its unit (below). Any other formula of the options (the index a layer
    // is styled with) is ignored: the contours are those of the values of the band.
    // A formula that does not exist is refused.
    if (!options.formula.empty() && !isThermalFormulaId(options.formula) &&
        VegetationEngine::instance().getFormula(options.formula) == nullptr)
        throw RefusedArgumentException("Unknown formula: " + options.formula);

    // --- Open raster ------------------------------------------------------
    DemContext dem = openDem(rasterPath, options.bandIndex, BandValueOptions{options.nodata});
    // A thermal formula gives the levels in its unit (D-022): the band model reads the values in it, so the
    // statistics, the levels and the elevations written back are all in that unit
    if (isThermalFormulaId(options.formula)) {
        const ThermalFormulaConversion converted = thermalFormulaConversion(dem.unit, options.formula);
        const double scale = dem.hasScale ? dem.scale : 1.0;
        const double offset = dem.hasScale ? dem.offset : 0.0;
        dem.scale = scale * converted.conversion.scale;
        dem.offset = offset * converted.conversion.scale + converted.conversion.offset;
        dem.hasScale = true;
        dem.unit = converted.unit;
    }
    const std::string &unit = dem.unit;

    // Band statistics drive interval-from-count and JSON metadata: the exact statistics of the pixels that
    // have a value, in physical values (RasterStatsProvider, FR-RAS-07; a pass that stops when the call is canceled)
    const BandStatistics stats =
        RasterStatsProvider::instance().get(rasterPath, dem.handle(), options.bandIndex, options.nodata);
    double bMin = dem.toPhysical(stats.min);
    double bMax = dem.toPhysical(stats.max);
    if (bMin > bMax) std::swap(bMin, bMax);

    const double interval = resolveInterval(options, bMin, bMax);

    // --- Build in-memory OGR layer to collect contours, and read it back -------
    session.report(ContourSession::kLevelsFrom, "levels");
    const ContourLayer contours = generateContourLayer(dem, interval, options, session);
    ContourFeatures collected = collectFeatures(contours.layer, dem, options, session);
    const size_t featureCount = collected.features.size();

    // --- Build output JSON -----------------------------------------------
    json result;
    result["type"] = "FeatureCollection";
    // Moved, not copied: a result of 100000 lines is not held twice
    result["features"] = std::move(collected.features);
    result["interval"] = interval;
    result["baseOffset"] = options.baseOffset;
    if (collected.kept > 0) {
        result["min"] = collected.minElev;
        result["max"] = collected.maxElev;
    } else {
        result["min"] = nullptr;
        result["max"] = nullptr;
    }
    result["featureCount"] = static_cast<int>(featureCount);
    result["unit"] = unit;
    result["rasterMin"] = bMin;
    result["rasterMax"] = bMax;
    // new in 2.4: the lines were cut at the cap (FR-RAS-11)
    result["truncated"] = collected.truncated;
    result["featureCap"] = std::max(1, options.maxFeatures);

    session.report(1.0, "lines");

    return result.dump();
}

} // namespace ddb
