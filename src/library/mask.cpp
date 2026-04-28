/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "mask.h"
#include "logger.h"
#include "exceptions.h"
#include "fs.h"

namespace ddb
{

    void maskBorders(const std::string &input,
                     const std::string &output,
                     int nearDist,
                     bool white,
                     const std::string &color)
    {

        if (fs::exists(output)) {
            LOGD << "Output file " << output << " already exists, deleting it";
            fs::remove(output);
        }

        GDALDatasetH hSrcDataset = GDALOpen(input.c_str(), GA_ReadOnly);
        if (!hSrcDataset)
            throw GDALException("Cannot open " + input + " for reading");

        char **targs = nullptr;

        // Set alpha band for transparency
        targs = CSLAddString(targs, "-setalpha");

        // Use floodfill algorithm - requires GDAL >= 3.8.
        // DroneDB pins GDAL via vcpkg, so this is always satisfied.
        // If building against a system GDAL, ensure version >= 3.8.
        targs = CSLAddString(targs, "-alg");
        targs = CSLAddString(targs, "floodfill");

        // Tolerance
        targs = CSLAddString(targs, "-near");
        targs = CSLAddString(targs, std::to_string(nearDist).c_str());

        // Color mode
        if (!color.empty()) {
            targs = CSLAddString(targs, "-color");
            targs = CSLAddString(targs, color.c_str());
        } else if (white) {
            targs = CSLAddString(targs, "-white");
        }

        // Output format and compression
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "GTiff");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=LZW");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "PREDICTOR=2");

        int bUsageError = FALSE;
        GDALNearblackOptions *psOptions = GDALNearblackOptionsNew(targs, nullptr);
        CSLDestroy(targs);

        if (!psOptions) {
            GDALClose(hSrcDataset);
            throw GDALException("Failed to create nearblack options");
        }

        GDALDatasetH hOutDataset = GDALNearblack(output.c_str(), nullptr, hSrcDataset, psOptions, &bUsageError);
        GDALNearblackOptionsFree(psOptions);

        if (bUsageError) {
            GDALClose(hSrcDataset);
            if (hOutDataset) GDALClose(hOutDataset);
            throw GDALException("GDALNearblack usage error");
        }

        if (!hOutDataset) {
            GDALClose(hSrcDataset);
            throw GDALException("GDALNearblack failed to create output: " + output);
        }

        GDALClose(hOutDataset);
        GDALClose(hSrcDataset);

        LOGD << "Masked borders: " << input << " -> " << output;
    }
}
