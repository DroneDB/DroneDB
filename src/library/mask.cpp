/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "mask.h"
#include "logger.h"
#include "exceptions.h"
#include "fs.h"
#include <cstdint>

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

        // Supported domain:
        //   - 1 band  any   -> greyscale ortho / thermal / DEM with black collar
        //   - 3 bands Byte  -> RGB orthophoto with black/white collar
        //   - 4 bands Byte  -> RGBA orthophoto (e.g. a previously-masked file being
        //                      reprocessed); the existing alpha is overwritten by
        //                      the new internal dataset mask.
        // Anything else (multispectral, mixed types) is out of scope: refusing here
        // avoids silently producing a corrupt or oversized output.
        if (srcBands != 1 && srcBands != 3 && srcBands != 4) {
            GDALClose(hSrcDataset);
            throw InvalidArgsException(
                "Mask borders supports only 1-band (DEM/thermal/greyscale), 3-band RGB "
                "or 4-band RGBA rasters; input has " + std::to_string(srcBands) + " bands");
        }

        GDALRasterBandH hBand1 = GDALGetRasterBand(hSrcDataset, 1);
        if (!hBand1) {
            GDALClose(hSrcDataset);
            throw GDALException("Cannot read band 1 from " + input);
        }
        const GDALDataType srcType = GDALGetRasterDataType(hBand1);
        const int bytesPerSample = GDALGetDataTypeSizeBytes(srcType);
        if (bytesPerSample <= 0) {
            GDALClose(hSrcDataset);
            throw InvalidArgsException(
                std::string("Unsupported band data type: ") + GDALGetDataTypeName(srcType));
        }

        // 3-/4-band paths are supported only for 8-bit (true RGB / RGBA orthophotos).
        if ((srcBands == 3 || srcBands == 4) && srcType != GDT_Byte) {
            GDALClose(hSrcDataset);
            throw InvalidArgsException(
                std::string("3- and 4-band masking require 8-bit Byte input (RGB/RGBA "
                            "orthophoto); got ") + GDALGetDataTypeName(srcType));
        }

        const uint64_t pixelCount =
            static_cast<uint64_t>(srcWidth) * static_cast<uint64_t>(srcHeight);
        // With -setmask the transparency is a dataset mask (1 bit/pixel), not an
        // added alpha band. This is only a rough uncompressed-size estimate used for
        // diagnostics; the actual BigTIFF promotion is decided by BIGTIFF=IF_SAFER.
        const uint64_t estimatedRasterBytes =
            pixelCount * static_cast<uint64_t>(bytesPerSample) * static_cast<uint64_t>(srcBands);
        const uint64_t estimatedMaskBytes = (pixelCount + 7) / 8;
        const uint64_t estimatedBytes = estimatedRasterBytes + estimatedMaskBytes;
        const bool logBigTiffLikely = estimatedBytes > (uint64_t(4) << 30);

        LOGD << "Mask source: " << srcWidth << "x" << srcHeight
            << " bands=" << srcBands
            << " type=" << GDALGetDataTypeName(srcType)
            << " bytesPerSample=" << bytesPerSample
            << " estimated uncompressed raster+1bit-mask ~"
            << (estimatedBytes >> 20) << " MiB"
            << (logBigTiffLikely ? " (BigTIFF may be needed; final decision uses BIGTIFF=IF_SAFER)" : "");

        char **targs = nullptr;

        // IMPORTANT: use mask, not alpha.
        // Alpha would create an extra Byte band and break JPEG/YCBCR layout.
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

        // Compression policy:
        //   * 3-band Byte (RGB ortho)  -> JPEG + YCBCR (small, fast, perceptually fine)
        //   * 1-band Byte (greyscale)  -> JPEG + MINISBLACK
        //   * 4-band Byte (RGBA)       -> DEFLATE + PREDICTOR=2 (JPEG can't carry the
        //                                  alpha band; DEFLATE keeps it fast)
        //   * 1-band integer non-Byte  -> DEFLATE + PREDICTOR=2 (horizontal)
        //   * 1-band Float32/Float64   -> DEFLATE + PREDICTOR=3 (floating point)
        // Lossless LZW is intentionally avoided on large orthos because it produces
        // multi-GB intermediates and is very slow. The mask output is consumed by
        // buildCog right after, so the only requirement is that the compressor
        // supports the band layout/type.
        const char *compressOpt = nullptr;
        const char *photometricOpt = nullptr;
        const char *predictorOpt = nullptr;
        const char *qualityOrZlevelOpt = nullptr;

        if (srcBands == 3) {
            compressOpt = "COMPRESS=JPEG";
            photometricOpt = "PHOTOMETRIC=YCBCR";
            qualityOrZlevelOpt = "JPEG_QUALITY=75";
        } else if (srcBands == 1 && srcType == GDT_Byte) {
            compressOpt = "COMPRESS=JPEG";
            photometricOpt = "PHOTOMETRIC=MINISBLACK";
            qualityOrZlevelOpt = "JPEG_QUALITY=75";
        } else {
            // 4-band Byte (RGBA) and 1-band non-Byte (integers/floats) all land here.
            compressOpt = "COMPRESS=DEFLATE";
            // ZLEVEL=2: very fast, still gives big savings on flat-collar borders.
            qualityOrZlevelOpt = "ZLEVEL=2";
            predictorOpt = (srcType == GDT_Float32 || srcType == GDT_Float64)
                               ? "PREDICTOR=3"
                               : "PREDICTOR=2";
        }

        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, compressOpt);
        if (photometricOpt) {
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, photometricOpt);
        }
        if (predictorOpt) {
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, predictorOpt);
        }
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, qualityOrZlevelOpt);

        // BIGTIFF=IF_SAFER lets the GTiff driver decide whether BigTIFF is needed.
        // The estimate logged above is informational only.
        targs = CSLAddString(targs, "-co");
        targs = CSLAddString(targs, "BIGTIFF=IF_SAFER");

        // TILED + 512px blocks avoid giant-strip scratch buffers and match the COG
        // pipeline. We do not generate overviews here; mask output is treated as an
        // intermediate and overview generation is left to the build/COG pipeline.
        targs = CSLAddString(targs, "-co");
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
