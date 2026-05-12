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

        // Clean possible stale external mask from previous runs.
        const std::string externalMask = output + ".msk";
        if (fs::exists(externalMask)) {
            LOGD << "External mask file " << externalMask << " already exists, deleting it";
            fs::remove(externalMask);
        }

        GDALDatasetH hSrcDataset = GDALOpen(input.c_str(), GA_ReadOnly);
        if (!hSrcDataset)
            throw GDALException("Cannot open " + input + " for reading");

        const int srcWidth = GDALGetRasterXSize(hSrcDataset);
        const int srcHeight = GDALGetRasterYSize(hSrcDataset);
        const int srcBands = GDALGetRasterCount(hSrcDataset);

        const uint64_t pixelCount =
            static_cast<uint64_t>(srcWidth) * static_cast<uint64_t>(srcHeight);

        uint64_t estimatedRasterBytes = 0;
        int maxBytesPerSample = 0;
        bool hasUnknownDataTypeSize = false;

        for (int i = 1; i <= srcBands; ++i) {
            GDALRasterBandH hBand = GDALGetRasterBand(hSrcDataset, i);
            if (!hBand) {
                hasUnknownDataTypeSize = true;
                continue;
            }

            const GDALDataType eType = GDALGetRasterDataType(hBand);
            const int bytesPerSample = GDALGetDataTypeSizeBytes(eType);

            if (bytesPerSample <= 0) {
                hasUnknownDataTypeSize = true;
                continue;
            }

            estimatedRasterBytes += pixelCount * static_cast<uint64_t>(bytesPerSample);
            maxBytesPerSample = std::max(maxBytesPerSample, bytesPerSample);
        }

        // With -setmask the transparency is a dataset mask, not an added alpha band.
        // Internal GeoTIFF masks are conceptually 1 bit/pixel; this is only a rough
        // uncompressed-size estimate
        const uint64_t estimatedMaskBytes = (pixelCount + 7) / 8;
        const uint64_t estimatedBytes = estimatedRasterBytes + estimatedMaskBytes;

        // Informational only. The actual BigTIFF choice is controlled by the GTiff
        // creation option BIGTIFF=IF_SAFER below.
        const bool logBigTiffLikely = estimatedBytes > (uint64_t(4) << 30);

        LOGD << "Mask source: " << srcWidth << "x" << srcHeight
            << " bands=" << srcBands
            << " maxBytesPerSample=" << maxBytesPerSample
            << " estimated uncompressed raster+1bit-mask ~"
            << (estimatedBytes >> 20) << " MiB"
            << (hasUnknownDataTypeSize ? " (data type size unknown for at least one band)" : "")
            << (logBigTiffLikely ? " (BigTIFF may be needed; final decision uses BIGTIFF=IF_SAFER)" : "");

        char **targs = nullptr;

        // IMPORTANT: use mask, not alpha.
        // Alpha would create a 4th Byte band and make JPEG/YCBCR less straightforward.
        targs = CSLAddString(targs, "-setmask");

        targs = CSLAddString(targs, "-alg");
        targs = CSLAddString(targs, "floodfill");
        // Faster alternative but doesn't handle concave collars
        // targs = CSLAddString(targs, "twopasses");

        targs = CSLAddString(targs, "-near");
        targs = CSLAddString(targs, std::to_string(nearDist).c_str());

        if (!color.empty()) {
            targs = CSLAddString(targs, "-color");
            targs = CSLAddString(targs, color.c_str());
        } else if (white) {
            targs = CSLAddString(targs, "-white");
        }

        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "GTiff");

        // Match the source style: RGB JPEG-in-TIFF with YCbCr.
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "COMPRESS=JPEG");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "PHOTOMETRIC=YCBCR");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "JPEG_QUALITY=75");

        // BIGTIFF=IF_SAFER lets the GTiff driver decide whether BigTIFF is safer.
        // The estimate logged above is informational only and does not drive this
        // decision.
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BIGTIFF=IF_SAFER");
        targs = CSLAddString(targs, "-co");

        // TILED + 512px blocks avoid giant-strip scratch buffers and match the COG
        // pipeline. We do not generate overviews here; nearblack output is treated as
        // an intermediate and overview generation is left to the build/COG pipeline.
        targs = CSLAddString(targs, "TILED=YES");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BLOCKXSIZE=512");
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BLOCKYSIZE=512");

        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "NUM_THREADS=ALL_CPUS");

        int bUsageError = FALSE;

        // Force internal GeoTIFF mask instead of .msk sidecar, especially for GDAL < 3.9.
        const char *prevInternalMask =
            CPLGetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK", nullptr);
        const std::string prevInternalMaskValue =
            prevInternalMask ? prevInternalMask : "";

        CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK", "YES");

        GDALNearblackOptions *psOptions = GDALNearblackOptionsNew(targs, nullptr);
        CSLDestroy(targs);

        if (!psOptions) {
            CPLSetThreadLocalConfigOption(
                "GDAL_TIFF_INTERNAL_MASK",
                prevInternalMask ? prevInternalMaskValue.c_str() : nullptr
            );
            GDALClose(hSrcDataset);
            throw GDALException("Failed to create nearblack options");
        }

        GDALDatasetH hOutDataset =
            GDALNearblack(output.c_str(), nullptr, hSrcDataset, psOptions, &bUsageError);

        GDALNearblackOptionsFree(psOptions);

        CPLSetThreadLocalConfigOption(
            "GDAL_TIFF_INTERNAL_MASK",
            prevInternalMask ? prevInternalMaskValue.c_str() : nullptr
        );

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
