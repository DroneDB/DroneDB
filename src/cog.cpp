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
