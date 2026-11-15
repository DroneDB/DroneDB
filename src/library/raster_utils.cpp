/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_utils.h"
#include "raster_source.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_utils.h"
#include "exceptions.h"

#include <algorithm>

namespace ddb {

void requireGeoTransform(GDALDatasetH ds, double gt[6], const std::string &name) {
    if (GDALGetGeoTransform(ds, gt) != CE_None)
        throw GDALException("Raster has no geotransform: " + name);
}


GDALDatasetUniquePtr withNodataOverride(GDALDatasetH src, double nodata) {
    std::ostringstream value;
    if (std::isnan(nodata))
        value << "nan";
    else
        value << std::setprecision(17) << nodata;

    std::vector<std::string> args = {"-of", "VRT", "-a_nodata", value.str()};
    CPLStringList argv = cslFromStrings(args);
    const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(argv.List(), nullptr));
    if (!options)
        throw GDALException("Cannot override the NoData value: invalid options");
    auto view = adoptDataset(GDALTranslate("", src, options.get(), nullptr));
    if (!view)
        throw GDALException("Cannot override the NoData value of the raster");
    return view;
}

GeoPixelMap::GeoPixelMap(const double gt[6]) {
    if (!GDALInvGeoTransform(gt, inv))
        throw AppException("Cannot invert geotransform");
}

void GeoPixelMap::toPixelIndex(double x, double y, int &col, int &row) const {
    double px, py;
    toPixel(x, y, px, py);
    // Beyond what an int holds is outside any raster: it must not wrap into one
    col = static_cast<int>(std::max(-1e9, std::min(1e9, std::floor(px))));
    row = static_cast<int>(std::max(-1e9, std::min(1e9, std::floor(py))));
}

double tidyValue(double value, GDALDataType type) {
    if (!std::isfinite(value)) return value;
    const bool wide = type == GDT_UInt32 || type == GDT_Int32 || type == GDT_Float64 || type == GDT_Unknown;
    char text[40];
    std::snprintf(text, sizeof(text), wide ? "%.15g" : "%.7g", value);
    return std::strtod(text, nullptr);
}

bool nodataOutOfRange(GDALDataType type, double value) {
    if (std::isnan(value) || GDALDataTypeIsFloating(type)) return false;
    double lo = 0, hi = 0;
    switch (type) {
        case GDT_Byte: lo = 0; hi = 255; break;
        case GDT_Int8: lo = -128; hi = 127; break;
        case GDT_UInt16: lo = 0; hi = 65535; break;
        case GDT_Int16: lo = -32768; hi = 32767; break;
        case GDT_UInt32: lo = 0; hi = 4294967295.0; break;
        case GDT_Int32: lo = -2147483648.0; hi = 2147483647.0; break;
        default: return false;
    }
    return value < lo || value > hi || value != std::floor(value);
}

BandNodata readBandNodata(GDALRasterBandH band) {
    BandNodata nd;
    int has = 0;
    nd.value = GDALGetRasterNoDataValue(band, &has);
    nd.has = has != 0;
    return nd;
}


double alphaOpaqueValue(GDALRasterBandH alphaBand) {
    // A band with nothing to read reports an error that is not one here
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const double opaque =
        alphaOpaqueValue(*rasterSourceOf(GDALGetBandDataset(alphaBand)), GDALGetBandNumber(alphaBand));
    CPLPopErrorHandler();
    return opaque;
}

RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount) {
    return detectBandNodata(*rasterSourceOf(hDataset), bandCount);
}

void premaskNodata(const std::vector<float*>& bandPtrs,
                   size_t pixelCount,
                   int bandCount,
                   const RasterBandNodata& info,
                   float outputNodata,
                   const uint8_t* datasetMask) {
    for (size_t i = 0; i < pixelCount; i++) {
        bool masked = datasetMask != nullptr && datasetMask[i] == 0;

        if (!masked && info.alphaBandIdx >= 0 && bandPtrs[info.alphaBandIdx] && bandPtrs[info.alphaBandIdx][i] == 0.0f) {
            masked = true;
        }

        if (!masked) {
            for (int b = 0; b < bandCount; b++) {
                if (b == info.alphaBandIdx || !bandPtrs[b]) continue;
                if (isNodata(bandPtrs[b][i], info.bands[b])) {
                    masked = true;
                    break;
                }
            }
        }

        if (masked) {
            for (int b = 0; b < bandCount; b++) {
                if (bandPtrs[b]) bandPtrs[b][i] = outputNodata;
            }
        }
    }
}

}  // namespace ddb
