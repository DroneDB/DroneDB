/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "cog.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"

namespace ddb{

void buildCog(const std::string &inputGTiff, const std::string &outputCog){
    GDALDatasetH hSrcDataset = GDALOpen(inputGTiff.c_str(), GA_ReadOnly);

    if (!hSrcDataset)
        throw GDALException("Cannot open " + inputGTiff + " for reading");

    char** targs = nullptr;
    targs = CSLAddString(targs, "-of");
    targs = CSLAddString(targs, "COG");

    // We can compress to JPG if these are 8bit bands (3 or 4)
    const int numBands = GDALGetRasterCount(hSrcDataset);
    if (numBands == 3 || numBands == 4){
        bool all8Bit = true;

        for (int n = 0; n < numBands; n++){
            GDALRasterBandH b = GDALGetRasterBand(hSrcDataset, n + 1);
            if (GDALGetRasterDataType(b) != GDT_Byte){
                all8Bit = false;
            }
        }
        if (all8Bit){
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, "COMPRESS=JPEG");
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, "QUALITY=90");
        }
    }else{
        // LZW by default
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=LZW");
    }

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);
    GDALDatasetH hNewDataset = GDALTranslate(outputCog.c_str(),
                                     hSrcDataset,
                                     psOptions,
                                     nullptr);
    GDALTranslateOptionsFree(psOptions);
    GDALClose(hNewDataset);
    GDALClose(hSrcDataset);
}

}
