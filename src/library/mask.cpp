/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cancellation.h"
#include "raster_input.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "mask.h"
#include "logger.h"
#include "exceptions.h"
#include "fs.h"
#include "build_layout.h"
#include "mio.h"
#include "parallel.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "raster_protocol.h"
#include "raster_utils.h"
#include <cmath>
#include <cstdint>
#include <functional>

namespace ddb
{

    namespace
    {

        // The external mask GDAL writes next to a raster when it keeps none inside
        fs::path externalMaskOf(const fs::path &raster)
        {
            return io::concat(raster, ".msk");
        }

        // Moves the external mask GDAL wrote next to `produced` beside `target`. Without one the
        // mask that is there belongs to the raster that was replaced: it goes.
        void publishExternalMask(const fs::path &produced, const fs::path &target)
        {
            const fs::path mask = externalMaskOf(produced);
            if (fs::exists(mask))
                raster::publishArtefact(mask, externalMaskOf(target));
            else
                io::assureIsRemoved(externalMaskOf(target));
        }

        // The layout of the source that the compression depends on
        struct MaskSource
        {
            int width = 0;
            int height = 0;
            int bands = 0;
            GDALDataType type = GDT_Unknown;
            int bytesPerSample = 0;
        };

        // Reads the layout of the source and refuses what masking does not cover
        MaskSource checkMaskable(GDALDatasetH hSrcDataset, const std::string &input)
        {
            MaskSource src;
            src.width = GDALGetRasterXSize(hSrcDataset);
            src.height = GDALGetRasterYSize(hSrcDataset);
            src.bands = GDALGetRasterCount(hSrcDataset);

            // Supported domain:
            //   - 1 band  any   -> greyscale ortho / thermal / DEM with black collar
            //   - 3 bands Byte  -> RGB orthophoto with black/white collar
            //   - 4 bands Byte  -> RGBA orthophoto (e.g. a previously-masked file being
            //                      reprocessed); the existing alpha is overwritten by
            //                      the new internal dataset mask.
            // Anything else (multispectral, mixed types) is out of scope: refusing here
            // avoids silently producing a corrupt or oversized output.
            if (src.bands != 1 && src.bands != 3 && src.bands != 4) {
                throw InvalidArgsException(
                    "Mask borders supports only 1-band (DEM/thermal/greyscale), 3-band RGB "
                    "or 4-band RGBA rasters; input has " + std::to_string(src.bands) + " bands");
            }

            GDALRasterBandH hBand1 = GDALGetRasterBand(hSrcDataset, 1);
            if (!hBand1) {
                throw GDALException("Cannot read band 1 from " + input);
            }
            src.type = GDALGetRasterDataType(hBand1);
            src.bytesPerSample = GDALGetDataTypeSizeBytes(src.type);
            if (src.bytesPerSample <= 0) {
                throw InvalidArgsException(
                    std::string("Unsupported band data type: ") + GDALGetDataTypeName(src.type));
            }

            // 3-/4-band paths are supported only for 8-bit (true RGB / RGBA orthophotos).
            if ((src.bands == 3 || src.bands == 4) && src.type != GDT_Byte) {
                throw InvalidArgsException(
                    std::string("3- and 4-band masking require 8-bit Byte input (RGB/RGBA "
                                "orthophoto); got ") + GDALGetDataTypeName(src.type));
            }

            // For 4-band inputs, require that band 4 is an alpha channel. A 4-band
            // raster where band 4 is not alpha (e.g. RGB+NIR multispectral) must go
            // through the multispectral pipeline instead.
            if (src.bands == 4) {
                GDALRasterBandH hBand4 = GDALGetRasterBand(hSrcDataset, 4);
                if (!hBand4) {
                    throw GDALException("Cannot read band 4 from " + input);
                }
                if (GDALGetRasterColorInterpretation(hBand4) != GCI_AlphaBand) {
                    throw InvalidArgsException(
                        "4-band input must be RGBA (band 4 must be an alpha channel); "
                        "for multispectral rasters (e.g. RGB+NIR) use the multispectral pipeline");
                }
            }
            return src;
        }

        void logMaskSource(const MaskSource &src)
        {
            const uint64_t pixelCount =
                static_cast<uint64_t>(src.width) * static_cast<uint64_t>(src.height);
            // With -setmask the transparency is a dataset mask (1 bit/pixel), not an
            // added alpha band. This is only a rough uncompressed-size estimate used for
            // diagnostics; the actual BigTIFF promotion is decided by BIGTIFF=IF_SAFER.
            const uint64_t estimatedRasterBytes =
                pixelCount * static_cast<uint64_t>(src.bytesPerSample) * static_cast<uint64_t>(src.bands);
            const uint64_t estimatedMaskBytes = (pixelCount + 7) / 8;
            const uint64_t estimatedBytes = estimatedRasterBytes + estimatedMaskBytes;
            const bool logBigTiffLikely = estimatedBytes > (uint64_t(4) << 30);

            LOGD << "Mask source: " << src.width << "x" << src.height
                << " bands=" << src.bands
                << " type=" << GDALGetDataTypeName(src.type)
                << " bytesPerSample=" << src.bytesPerSample
                << " estimated uncompressed raster+1bit-mask ~"
                << (estimatedBytes >> 20) << " MiB"
                << (logBigTiffLikely ? " (BigTIFF may be needed; final decision uses BIGTIFF=IF_SAFER)" : "");
        }

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
        GtiffOptions maskCreationOptions(const MaskSource &src)
        {
            GtiffOptions creation;
            if (src.bands == 3) {
                creation.compress(GtiffOptions::Codec::Jpeg).set("PHOTOMETRIC", "YCBCR").set("JPEG_QUALITY", "75");
            } else if (src.bands == 1 && src.type == GDT_Byte) {
                creation.compress(GtiffOptions::Codec::Jpeg).set("PHOTOMETRIC", "MINISBLACK").set("JPEG_QUALITY", "75");
            } else {
                // 4-band Byte (RGBA) and 1-band non-Byte (integers/floats) all land here.
                // ZLEVEL=2: very fast, still gives big savings on flat-collar borders.
                creation.compress(GtiffOptions::Codec::Deflate).predictorFor(src.type).set("ZLEVEL", "2");
            }

            // BIGTIFF=IF_SAFER lets the GTiff driver decide whether BigTIFF is needed.
            // The estimate logged above is informational only.
            // TILED + 512px blocks avoid giant-strip scratch buffers and match the COG
            // pipeline. We do not generate overviews here; mask output is treated as an
            // intermediate and overview generation is left to the build/COG pipeline.
            creation.bigTiffIfSafer().tiled(512);
            return creation;
        }

    } // namespace

    namespace
    {
        // A progress fraction of a part of the work: [begin, end] of the whole
        raster::ProgressCallback partOf(const raster::ProgressCallback &progress, double begin, double end)
        {
            return [progress, begin, end](double fraction) {
                if (progress)
                    progress(begin + (end - begin) * fraction);
            };
        }

        // The colour of the borders (nearblack -color, -white or black) and how near a sample is to it
        struct BorderColor
        {
            std::string color; // "c1,c2...": -color; empty: -white or black
            bool white = false;
            int nearDist = 15;
        };

        // Runs GDALNearblack -setmask -alg floodfill on `source` into `dest` (a GTiff with `creation`, or
        // a MEM dataset when `creation` is null), with its internal mask
        GDALDatasetUniquePtr runNearblack(GDALDatasetH source, const std::string &dest, const BorderColor &border,
                                          const GtiffOptions *creation, const raster::ProgressCallback &progress)
        {
            CPLStringList targs;
            // IMPORTANT: use mask, not alpha.
            // Alpha would create an extra Byte band and break JPEG/YCBCR layout.
            targs.AddString("-setmask");
            targs.AddString("-alg");
            targs.AddString("floodfill");
            // Faster alternative but doesn't handle concave collars
            // targs.AddString("twopasses");
            targs.AddString("-near");
            targs.AddString(std::to_string(border.nearDist).c_str());
            if (!border.color.empty()) {
                targs.AddString("-color");
                targs.AddString(border.color.c_str());
            } else if (border.white) {
                targs.AddString("-white");
            }
            targs.AddString("-of");
            targs.AddString(creation != nullptr ? "GTiff" : "MEM");
            if (creation != nullptr)
                creation->appendTo(targs);

            cancel::GdalProgress gdalProgress([&progress](double fraction) {
                if (progress)
                    progress(fraction);
            });
            int bUsageError = FALSE;
            GDALDatasetUniquePtr result;
            {
                // Force internal GeoTIFF mask instead of .msk sidecar, especially for GDAL < 3.9.
                const ScopedThreadConfigOption internalMask("GDAL_TIFF_INTERNAL_MASK", "YES");
                const GdalOptionsPtr<GDALNearblackOptions> psOptions(GDALNearblackOptionsNew(targs.List(), nullptr));
                if (!psOptions)
                    throw GDALException("Failed to create nearblack options");
                gdalProgress.attach(psOptions.get());
                result = adoptDataset(GDALNearblack(dest.c_str(), nullptr, source, psOptions.get(), &bUsageError));
            }
            if (bUsageError)
                throw GDALException("GDALNearblack usage error");
            return result;
        }

        // The value of the border colour of a raster of one band of any type: -color (one value), -white
        // (255, as nearblack) or black
        double borderValue(const BorderColor &border)
        {
            if (border.color.empty())
                return border.white ? 255.0 : 0.0;
            try
            {
                size_t used = 0;
                const double value = std::stod(border.color, &used);
                if (used == border.color.size())
                    return value;
            }
            catch (const std::exception &)
            {
            }
            throw InvalidArgsException("The border colour of a raster of 1 band is one number, not '" + border.color + "'");
        }

        // How nearblack sees the samples of a raster through a raster of bytes: the byte of a sample near the
        // border colour, of one that is not, and the colour nearblack is told (-near 0). nearblack writes
        // over each pixel it masks (0, or 255 when the colour is 255, in its two passes; 255 for -white, else
        // 0, in its fill) and tests that value against the colour again: the bytes are chosen so that it
        // is near the colour, or not, as it is with the samples themselves.
        struct NearBytes
        {
            uint8_t near = 0;
            uint8_t far = 255;
            BorderColor border;
        };

        NearBytes nearBytesFor(const BorderColor &border, double color)
        {
            const auto isNear = [&](double v) { return !(color - v > border.nearDist || v > border.nearDist + color); };
            const bool passesNear = isNear(color == 255.0 ? 255.0 : 0.0);
            const bool fillNear = isNear(border.white ? 255.0 : 0.0);
            NearBytes bytes;
            bytes.border.nearDist = 0;
            if (passesNear && fillNear)
                return bytes; // black: 0 is what nearblack writes, and near
            if (!passesNear && !fillNear)
            {
                // -color 128: nearblack writes 0, which is not near
                bytes.near = 128;
                bytes.border.color = "128";
                return bytes;
            }
            // -color 255: 255 (near) in the two passes, 0 (not near) in the fill. nearblack writes 0 in its passes
            // only for a colour other than 255, which then fills with 0 too: the other way round never happens
            bytes.near = 255;
            bytes.far = 0;
            bytes.border.color = "255";
            return bytes;
        }

        // Masks the borders of a raster of one band of more than 8 bits (a DEM, a thermal raster), which
        // nearblack writes as 8 bits only (it would cut 1234 to 255, and refuses floating-point output). The
        // mask is the one nearblack computes: it runs on a raster of bytes that says which samples are
        // near the border colour and which are not (NearBytes). The output keeps every sample and the
        // type of the raster, with that mask, which also hides the samples that are NoData (or NaN).
        void maskSamples(GDALDatasetH hSrc, const MaskSource &src, const fs::path &out, const BorderColor &border,
                         const GtiffOptions &creation, const raster::ProgressCallback &progress)
        {
            const double color = borderValue(border);
            const double near = border.nearDist;
            const NearBytes nearBytes = nearBytesFor(border, color);
            GDALRasterBandH srcBand = GDALGetRasterBand(hSrc, 1);
            const BandNodata nodata = readBandNodata(srcBand);
            const auto width = static_cast<size_t>(src.width);

            // The rasters of bytes in memory, or in files beside the output past 100 million pixels (as
            // nearblack keeps its own working raster)
            const bool inMemory = static_cast<uint64_t>(src.width) * static_cast<uint64_t>(src.height) < 100000000ULL;
            const fs::path nearFile = io::concat(out, ".near.tif");
            const fs::path maskedFile = io::concat(out, ".near-masked.tif");
            struct Cleanup
            {
                fs::path a, b;
                ~Cleanup()
                {
                    std::error_code ec;
                    fs::remove(a, ec);
                    fs::remove(io::concat(a, ".msk"), ec);
                    fs::remove(b, ec);
                    fs::remove(io::concat(b, ".msk"), ec);
                }
            } cleanup{nearFile, maskedFile};

            GtiffOptions bytes;
            bytes.compress(GtiffOptions::Codec::Deflate).bigTiffIfSafer();
            auto nearDs = adoptDataset(GDALCreate(GDALGetDriverByName(inMemory ? "MEM" : "GTiff"),
                                                  inMemory ? "" : io::toUtf8(nearFile).c_str(), src.width, src.height,
                                                  1, GDT_Byte, inMemory ? nullptr : bytes.list().List()));
            if (!nearDs)
                throw GDALException("Cannot create the raster of the border samples");
            {
                GDALRasterBandH nearBand = GDALGetRasterBand(datasetHandle(nearDs), 1);
                std::vector<double> row(width);
                std::vector<uint8_t> isFar(width);
                const raster::ProgressCallback classify = partOf(progress, 0.0, 0.2);
                for (int y = 0; y < src.height; y++)
                {
                    cancel::throwIfCanceled();
                    if (GDALRasterIO(srcBand, GF_Read, 0, y, src.width, 1, row.data(), src.width, 1, GDT_Float64, 0, 0) != CE_None)
                        throw GDALException("Cannot read row " + std::to_string(y) + " of the raster");
                    // nearblack's test of a sample against the colour
                    for (size_t x = 0; x < width; x++)
                        isFar[x] = (color - row[x] > near || row[x] > near + color) ? nearBytes.far : nearBytes.near;
                    if (GDALRasterIO(nearBand, GF_Write, 0, y, src.width, 1, isFar.data(), src.width, 1, GDT_Byte, 0, 0) != CE_None)
                        throw GDALException("Cannot write row " + std::to_string(y) + " of the border samples");
                    classify(static_cast<double>(y + 1) / src.height);
                }
            }

            auto masked = runNearblack(datasetHandle(nearDs), inMemory ? std::string() : io::toUtf8(maskedFile), nearBytes.border,
                                       inMemory ? nullptr : &bytes, partOf(progress, 0.2, 0.8));
            if (!masked)
                throw GDALException("GDALNearblack failed to mask the border samples");
            nearDs.reset();
            GDALRasterBandH borderMask = GDALGetMaskBand(GDALGetRasterBand(datasetHandle(masked), 1));

            // The output: the samples of the raster, its georeference and NoData, and the mask
            const std::string outText = io::toUtf8(out);
            auto outDs = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), outText.c_str(), src.width, src.height,
                                                 1, src.type, creation.list().List()));
            if (!outDs)
                throw GDALException("GDALNearblack failed to create output: " + io::toUtf8(out.filename()));
            GDALDatasetH hOut = datasetHandle(outDs);
            double gt[6];
            if (GDALGetGeoTransform(hSrc, gt) == CE_None)
            {
                GDALSetGeoTransform(hOut, gt);
                GDALSetProjection(hOut, GDALGetProjectionRef(hSrc));
            }
            GDALRasterBandH outBand = GDALGetRasterBand(hOut, 1);
            if (nodata.has)
                GDALSetRasterNoDataValue(outBand, nodata.value);
            {
                const ScopedThreadConfigOption internalMask("GDAL_TIFF_INTERNAL_MASK", "YES");
                if (GDALCreateDatasetMaskBand(hOut, GMF_PER_DATASET) != CE_None)
                    throw GDALException("Cannot create the mask of " + io::toUtf8(out.filename()));
            }
            GDALRasterBandH outMask = GDALGetMaskBand(outBand);

            const size_t sampleBytes = static_cast<size_t>(GDALGetDataTypeSizeBytes(src.type));
            std::vector<uint8_t> samples(width * sampleBytes);
            std::vector<double> values(width);
            std::vector<uint8_t> mask(width);
            const raster::ProgressCallback written = partOf(progress, 0.8, 1.0);
            for (int y = 0; y < src.height; y++)
            {
                cancel::throwIfCanceled();
                if (GDALRasterIO(srcBand, GF_Read, 0, y, src.width, 1, samples.data(), src.width, 1, src.type, 0, 0) != CE_None ||
                    GDALRasterIO(srcBand, GF_Read, 0, y, src.width, 1, values.data(), src.width, 1, GDT_Float64, 0, 0) != CE_None ||
                    GDALRasterIO(borderMask, GF_Read, 0, y, src.width, 1, mask.data(), src.width, 1, GDT_Byte, 0, 0) != CE_None)
                    throw GDALException("Cannot read row " + std::to_string(y) + " of the raster");
                for (size_t x = 0; x < width; x++)
                    if (std::isnan(values[x]) || isNodata(values[x], nodata))
                        mask[x] = 0;
                if (GDALRasterIO(outBand, GF_Write, 0, y, src.width, 1, samples.data(), src.width, 1, src.type, 0, 0) != CE_None ||
                    GDALRasterIO(outMask, GF_Write, 0, y, src.width, 1, mask.data(), src.width, 1, GDT_Byte, 0, 0) != CE_None)
                    throw GDALException("Cannot write row " + std::to_string(y) + " of " + io::toUtf8(out.filename()));
                written(static_cast<double>(y + 1) / src.height);
            }
            masked.reset();
            closeOutputOrThrow(std::move(outDs), io::toUtf8(out.filename()));
        }
    } // namespace

    namespace raster
    {
        void maskRasterFile(const fs::path &input, const fs::path &out, const MaskParams &params,
                            const ProgressCallback &progress)
        {
            const std::string inputText = io::toUtf8(input);
            const auto srcDataset = openInputRaster(inputText);
            if (!srcDataset)
                throw GDALException("Cannot open " + inputText + " for reading");
            GDALDatasetH hSrcDataset = datasetHandle(srcDataset);

            const MaskSource src = checkMaskable(hSrcDataset, inputText);
            logMaskSource(src);

            GtiffOptions creation = maskCreationOptions(src);
            // Compression threads: those the host holds CPU tokens for
            const BuildThreads threads(params.threads);
            creation.add("NUM_THREADS=" + std::to_string(threads.threads()));

            BorderColor border;
            border.color = params.color;
            border.white = params.white;
            border.nearDist = params.nearDist;

            if (src.type != GDT_Byte)
            {
                maskSamples(hSrcDataset, src, out, border, creation, progress);
                return;
            }

            auto outDataset = runNearblack(hSrcDataset, io::toUtf8(out), border, &creation, progress);
            // The output is named by its file: its folder is the private one of the request
            if (!outDataset)
                throw GDALException("GDALNearblack failed to create output: " + io::toUtf8(out.filename()));
            closeOutputOrThrow(std::move(outDataset), io::toUtf8(out.filename()));
        }
    } // namespace raster

    void maskBorders(const std::string &input,
                 const std::string &output,
                 int nearDist,
                 bool white,
                 const std::string &color,
                 bool overwrite)
    {
        const fs::path target = fs::absolute(io::fromUtf8(output));
        if (!target.has_filename())
            throw InvalidArgsException("Not the path of a file for a masked raster: " + output);
        io::assureReplaceable(io::fromUtf8(output), overwrite, {io::fromUtf8(input)});

        // The CPU tokens of the compression threads, held until the output is written
        const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
        raster::MaskParams params;
        params.nearDist = nearDist;
        params.white = white;
        params.color = color;
        params.threads = static_cast<int>(cpu.threads());

        // The raster helper reads the input (a file of a user) and writes the mask to a file of a
        // private folder beside the output, which is removed with whatever is left in it: an
        // existing output is replaced only by a complete mask
        const io::ScopedTempDir staging = raster::makePrivateFolder(target.parent_path(), MaskStagingPrefix);
        const fs::path produced = staging.path() / target.filename();
        raster::ProgressLog log("Mask borders");
        raster::rasterHelper().operate(raster::op::kMask, io::fromUtf8(input), produced, raster::toJson(params),
                                       std::ref(log));
        // A call canceled while the helper finished is canceled: the output stays as it was
        cancel::throwIfCanceled();

        // What the helper wrote is published as the user's raster: it has to read as one first
        if (!raster::writtenRasterOpens(produced))
            throw GDALException(std::string(raster::kGdalUtilName) + " wrote a mask of " + input +
                                " that cannot be read");

        // The output may have appeared while the borders were masked: it is replaced only when
        // asked. The raster first, then its external mask (or none).
        io::assureReplaceable(io::fromUtf8(output), overwrite);
        raster::publishArtefact(produced, target);
        publishExternalMask(produced, target);

        LOGD << "Masked borders: " << input << " -> " << output;
    }
}
