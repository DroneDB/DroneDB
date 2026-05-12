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

        const int srcWidth = GDALGetRasterXSize(hSrcDataset);
        const int srcHeight = GDALGetRasterYSize(hSrcDataset);
        const int srcBands = GDALGetRasterCount(hSrcDataset);
        // Rough estimate of the uncompressed output size including the alpha band
        // we are about to add. Byte rasters dominate ortho workflows; for wider
        // types this is still a conservative lower bound that drives the BIGTIFF
        // decision below.
        const uint64_t estimatedBytes =
            static_cast<uint64_t>(srcWidth) * static_cast<uint64_t>(srcHeight) *
            static_cast<uint64_t>(srcBands + 1);
        // Classic TIFF caps at 4 GiB. Log when we expect to cross that line so
        // the BIGTIFF=IF_SAFER promotion is auditable from --debug output.
        const bool expectBigTiff = estimatedBytes > (uint64_t(4) << 30);
        LOGD << "Mask source: " << srcWidth << "x" << srcHeight
             << " bands=" << srcBands
             << " estimated output ~" << (estimatedBytes >> 20) << " MiB"
             << (expectBigTiff ? " (BIGTIFF expected)" : "");

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

        // Output format and compression.
        // BIGTIFF=IF_SAFER auto-promotes to BigTIFF when the predicted size
        // approaches the 4 GiB classic-TIFF limit (keeps small outputs classic).
        // TILED + 512px blocks avoid the giant-strip scratch buffers that cause
        // TIFFAppendToStrip failures on large orthos and match the COG pipeline.
        // NUM_THREADS=ALL_CPUS speeds up LZW encoding on large rasters.
        // Note: we intentionally do NOT generate overviews here; nearblack output
        // is treated as an intermediate. Overview generation is left to the
        // build/COG pipeline (see buildCog).
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "GTiff");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=LZW");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "PREDICTOR=2");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BIGTIFF=IF_SAFER");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "TILED=YES");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BLOCKXSIZE=512");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BLOCKYSIZE=512");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "NUM_THREADS=ALL_CPUS");

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
