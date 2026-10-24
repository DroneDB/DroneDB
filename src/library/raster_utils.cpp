/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_utils.h"

#include <string>

#include "exceptions.h"

namespace ddb {

void requireGeoTransform(GDALDatasetH ds, double gt[6]) {
    if (GDALGetGeoTransform(ds, gt) != CE_None)
        throw GDALException(std::string("Raster has no geotransform: ") + GDALGetDescription(ds));
}


BandNodata readBandNodata(GDALRasterBandH band) {
    BandNodata nd;
    int has = 0;
    nd.value = GDALGetRasterNoDataValue(band, &has);
    nd.has = has != 0;
    return nd;
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

    return info;
}

void premaskNodata(const std::vector<float*>& bandPtrs,
                   size_t pixelCount,
                   int bandCount,
                   const RasterBandNodata& info,
                   float outputNodata) {
    for (size_t i = 0; i < pixelCount; i++) {
        bool masked = false;

        if (info.alphaBandIdx >= 0 && bandPtrs[info.alphaBandIdx] && bandPtrs[info.alphaBandIdx][i] == 0.0f) {
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
