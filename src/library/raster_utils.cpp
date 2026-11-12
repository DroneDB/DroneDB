/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_utils.h"

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

namespace {

// The largest finite value of `alpha` in a grid of `grid` x `grid` windows,
// one in the block under each grid point (at most kAlphaSampleWindow pixels a side, centred on the
// point and kept in that block, so a window decodes one block: one row of a strip-organised
// raster), read at full resolution (a window read at its own size never takes an overview). 0 when
// none of them holds a value above 0; NaN when a read fails.
constexpr int kAlphaSampleWindow = 256;

double sampledFullResolutionMaximum(GDALRasterBandH alpha, int grid) {
    const int w = GDALGetRasterBandXSize(alpha), h = GDALGetRasterBandYSize(alpha);
    int bw = 0, bh = 0;
    GDALGetBlockSize(alpha, &bw, &bh);
    if (bw <= 0 || bh <= 0) bw = bh = kAlphaSampleWindow;
    std::vector<double> buffer;
    double maximum = 0.0;
    for (int gy = 0; gy < grid; gy++)
        for (int gx = 0; gx < grid; gx++) {
            const int cx = static_cast<int>((gx + 0.5) * w / grid);
            const int cy = static_cast<int>((gy + 0.5) * h / grid);
            // The block under the grid point, cut at the raster's edge
            const int bx0 = cx / bw * bw, by0 = cy / bh * bh;
            const int bx1 = std::min(bx0 + bw, w), by1 = std::min(by0 + bh, h);
            const int ww = std::min(bx1 - bx0, kAlphaSampleWindow), wh = std::min(by1 - by0, kAlphaSampleWindow);
            const int x0 = std::clamp(cx - ww / 2, bx0, bx1 - ww), y0 = std::clamp(cy - wh / 2, by0, by1 - wh);
            buffer.resize(static_cast<size_t>(ww) * wh);
            if (GDALRasterIO(alpha, GF_Read, x0, y0, ww, wh, buffer.data(), ww, wh, GDT_Float64, 0, 0) != CE_None)
                return std::nan("");
            for (const double v : buffer)
                if (std::isfinite(v) && v > maximum) maximum = v;
        }
    return maximum;
}

}  // namespace

double alphaOpaqueValue(GDALRasterBandH alphaBand) {
    // The maximum of the full-resolution pixels: a sample of 4 x 4 windows, then of 16 x 16 when it
    // has no value above 0 (data in a small part of the raster: a corridor along a road), then the
    // exact maximum of the band. Not
    // GDAL's approximate maximum: it reads the overviews, and the cubic overviews of a COG ring
    // above the opaque value at the edge of the data (266 for 255, which made 511 opaque).
    // A band with nothing to read (every pixel nodata) reports an error that is not one here
    CPLPushErrorHandler(CPLQuietErrorHandler);
    double minMax[2] = {0.0, 0.0};
    bool sampled = false;
    for (const int grid : {4, 16}) {
        minMax[1] = sampledFullResolutionMaximum(alphaBand, grid);
        sampled = std::isfinite(minMax[1]);
        if (!sampled || minMax[1] > 0.0) break;
    }
    if (!sampled || minMax[1] <= 0.0) sampled = GDALComputeRasterMinMax(alphaBand, FALSE, minMax) == CE_None;
    CPLPopErrorHandler();

    if (sampled && minMax[1] > 0.0) {
        // The all-ones value that holds the maximum: 2^bits - 1
        const double bits = std::ceil(std::log2(std::ceil(minMax[1]) + 1.0));
        return std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    }

    const GDALDataType type = GDALGetRasterDataType(alphaBand);
    if (GDALDataTypeIsFloating(type)) return 1.0;
    const int bits = GDALGetDataTypeSizeBits(type) - (GDALDataTypeIsSigned(type) ? 1 : 0);
    return std::ldexp(1.0, bits) - 1.0;
}

RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount) {
    RasterBandNodata info;
    info.alphaBandIdx = -1;
    info.bands.resize(bandCount);

    for (int b = 0; b < bandCount; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, b + 1);
        if (GDALGetRasterColorInterpretation(hBand) == GCI_AlphaBand) {
            info.alphaBandIdx = b;
        }
        info.bands[b] = readBandNodata(hBand);
    }

    // A mask kept for the whole dataset; a nodata mask is the nodata compare already
    if (info.alphaBandIdx < 0 && bandCount > 0) {
        const int flags = GDALGetMaskFlags(GDALGetRasterBand(hDataset, 1));
        info.datasetMask = (flags & GMF_PER_DATASET) != 0 && (flags & GMF_NODATA) == 0;
    }

    return info;
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
