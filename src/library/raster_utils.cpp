/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_utils.h"

namespace ddb {

RasterBandNodata detectBandNodata(GDALDatasetH hDataset, int bandCount) {
    RasterBandNodata info;
    info.alphaBandIdx = -1;
    info.hasNodata.resize(bandCount, 0);
    info.nodataValues.resize(bandCount, 0.0);

    for (int b = 0; b < bandCount; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, b + 1);
        if (GDALGetRasterColorInterpretation(hBand) == GCI_AlphaBand) {
            info.alphaBandIdx = b;
        }
        info.nodataValues[b] = GDALGetRasterNoDataValue(hBand, &info.hasNodata[b]);
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

        if (info.alphaBandIdx >= 0 && bandPtrs[info.alphaBandIdx][i] == 0.0f) {
            masked = true;
        }

        if (!masked) {
            for (int b = 0; b < bandCount; b++) {
                if (b == info.alphaBandIdx) continue;
                if (info.hasNodata[b]) {
                    const double sampleValue = static_cast<double>(bandPtrs[b][i]);
                    const bool isNodata =
                        (std::isnan(info.nodataValues[b]) && std::isnan(sampleValue)) ||
                        (!std::isnan(info.nodataValues[b]) && sampleValue == info.nodataValues[b]);
                    if (isNodata) {
                        masked = true;
                        break;
                    }
                }
            }
        }

        if (masked) {
            for (int b = 0; b < bandCount; b++) {
                bandPtrs[b][i] = outputNodata;
            }
        }
    }
}

}  // namespace ddb
