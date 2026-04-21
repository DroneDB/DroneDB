/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <cstdlib>
#include <iomanip>
#include <pdal/filters/ColorinterpFilter.hpp>
#include <pdal/io/EptReader.hpp>
#include <sstream>
#include <algorithm>
#include <cmath>

#include "coordstransformer.h"
#include "dbops.h"
#include "epttiler.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "hash.h"
#include "mio.h"
#include "pointcloud.h"
#include "raster_utils.h"
#include "sensorprofile.h"
#include "vegetation.h"
#include "tiler.h"
#include "userprofile.h"
#include "utils.h"

namespace ddb {

fs::path getThumbFromUserCache(const fs::path& imagePath, int thumbSize, bool forceRecreate) {
    if (std::rand() % 1000 == 0)
        cleanupThumbsUserCache();
    if (!fs::exists(imagePath))
        throw FSException(imagePath.filename().string() + " does not exist");
    const fs::path outdir = UserProfile::get()->getThumbsDir(thumbSize);
    io::Path p = imagePath;
    const fs::path thumbPath = outdir / getThumbFilename(imagePath, p.getModifiedTime(), thumbSize);
    return generateThumb(imagePath, thumbSize, thumbPath, forceRecreate, nullptr, nullptr);
}

bool supportsThumbnails(EntryType type) {
    return type == Image || type == GeoImage || type == GeoRaster;
}

void generateThumbs(const std::vector<std::string>& input,
                    const fs::path& output,
                    int thumbSize,
                    bool useCrc) {
    if (input.size() > 1)
        io::assureFolderExists(output);
    const bool outputIsFile = input.size() == 1 && io::Path(output).checkExtension(
                                                       {"jpg", "jpeg", "png", "webp", "json"});

    const std::vector<fs::path> filePaths = std::vector<fs::path>(input.begin(), input.end());

    for (auto& fp : filePaths) {
        LOGD << "Parsing entry " << fp.string();

        const EntryType type = fingerprint(fp);
        io::Path p(fp);

        // NOTE: This check is looking pretty ugly, maybe move "ept.json" in a const?
        if (supportsThumbnails(type) || fp.filename() == "ept.json") {
            fs::path outImagePath;
            if (useCrc) {
                outImagePath = output / getThumbFilename(fp, p.getModifiedTime(), thumbSize);
            } else if (outputIsFile) {
                outImagePath = output;
            } else {
                outImagePath = output / fs::path(fp).replace_extension(".webp").filename();
            }
            std::cout << generateThumb(fp, thumbSize, outImagePath, true, nullptr, nullptr).string()
                      << std::endl;
        } else {
            LOGD << "Skipping " << fp;
        }
    }
}

fs::path getThumbFilename(const fs::path& imagePath, time_t modifiedTime, int thumbSize) {
    // Thumbnails are WEBP files idenfitied by:
    // CRC64(imagePath + "*" + modifiedTime + "*" + thumbSize).webp
    std::ostringstream os;
    os << imagePath.string() << "*" << modifiedTime << "*" << thumbSize;
    return fs::path(Hash::strCRC64(os.str()) + ".webp");
}

namespace {

// RAII guard that closes a GDAL dataset on scope exit. Does nothing if released.
struct GdalDatasetGuard {
    GDALDatasetH handle;
    explicit GdalDatasetGuard(GDALDatasetH h = nullptr) : handle(h) {}
    ~GdalDatasetGuard() { if (handle) GDALClose(handle); }
    GdalDatasetGuard(const GdalDatasetGuard&) = delete;
    GdalDatasetGuard& operator=(const GdalDatasetGuard&) = delete;
    GDALDatasetH release() { GDALDatasetH h = handle; handle = nullptr; return h; }
};

// RAII guard that unlinks a vsimem path on scope exit.
struct VsiMemGuard {
    std::string path;
    explicit VsiMemGuard(std::string p) : path(std::move(p)) {}
    ~VsiMemGuard() { if (!path.empty()) VSIUnlink(path.c_str()); }
    VsiMemGuard(const VsiMemGuard&) = delete;
    VsiMemGuard& operator=(const VsiMemGuard&) = delete;
    void release() { path.clear(); }
};

// Build gdal_translate-style args to normalize a source raster to a 3-band
// Byte GTiff of a given target size. Alpha is handled separately via the
// mask band to keep this stage source-agnostic.
char** buildThumbNormalizationArgs(int targetWidth,
                                   int targetHeight,
                                   bool hasPalette,
                                   int bandCount) {
    char** args = nullptr;
    args = CSLAddString(args, "-outsize");
    args = CSLAddString(args, std::to_string(targetWidth).c_str());
    args = CSLAddString(args, std::to_string(targetHeight).c_str());
    args = CSLAddString(args, "-ot");
    args = CSLAddString(args, "Byte");
    args = CSLAddString(args, "-r");
    args = CSLAddString(args, hasPalette ? "nearest" : "average");
    args = CSLAddString(args, "-of");
    args = CSLAddString(args, "GTiff");

    if (hasPalette) {
        // Expand palette to RGB. Palette transparency, if any, is recovered
        // from the source mask band in a later step.
        args = CSLAddString(args, "-expand");
        args = CSLAddString(args, "rgb");
    } else if (bandCount == 1) {
        // Grayscale (e.g. DEM/DSM): replicate band 1 three times and rescale
        // from the data range to 0-255 for visual contrast.
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-scale");
    } else if (bandCount == 2) {
        // Grayscale + alpha: replicate band 1 for the RGB triplet; alpha is
        // read explicitly from band 2 later.
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-scale");
    } else {
        // RGB or more: always pick the first three bands for the color output.
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "1");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "2");
        args = CSLAddString(args, "-b");
        args = CSLAddString(args, "3");
        args = CSLAddString(args, "-scale");
    }

    // Drop SRS: the thumbnail is meant as a display-only artifact.
    args = CSLAddString(args, "-a_srs");
    args = CSLAddString(args, "");
    return args;
}

// Decide which source band should provide the alpha channel for the thumbnail.
// Returns nullptr when the source is known to be fully opaque (no nodata, no
// alpha band, no per-dataset mask) so the caller can skip the alpha pass.
GDALRasterBandH resolveAlphaBand(GDALDatasetH hSrc, int bandCount) {
    // Generic 2-band rasters conventionally carry grayscale + alpha.
    if (bandCount == 2)
        return GDALGetRasterBand(hSrc, 2);

    // Prefer an explicit alpha band when present on 4+ band rasters.
    if (bandCount >= 4) {
        GDALRasterBandH band4 = GDALGetRasterBand(hSrc, 4);
        if (band4 && GDALGetRasterColorInterpretation(band4) == GCI_AlphaBand)
            return band4;
    }

    GDALRasterBandH band1 = GDALGetRasterBand(hSrc, 1);
    if (!band1)
        return nullptr;

    // GDALGetMaskBand always returns a valid handle, but for fully-valid
    // rasters its only flag is GMF_ALL_VALID. In that case skip the extra
    // mask read and output a plain 3-band WEBP.
    const int maskFlags = GDALGetMaskFlags(band1);
    if (maskFlags == GMF_ALL_VALID)
        return nullptr;

    return GDALGetMaskBand(band1);
}

// Encode a prepared GDAL dataset (MEM or GTiff-in-vsimem) as a WEBP file or
// in-memory buffer. Centralises the WEBP creation options and the vsimem
// buffer extraction logic.
void writeWebPFromDataset(GDALDatasetH hSrc,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize) {
    GDALDriverH webpDrv = GDALGetDriverByName("WEBP");
    if (!webpDrv)
        throw GDALException("WEBP driver not available");

    char** webpOpts = nullptr;
    webpOpts = CSLAddString(webpOpts, "QUALITY=95");
    webpOpts = CSLAddString(webpOpts, "LOSSLESS=FALSE");

    const bool writeToMemory = outImagePath.empty() && outBuffer != nullptr;
    std::string outPath;
    if (writeToMemory) {
        outPath = "/vsimem/" + utils::generateRandomString(32) + ".webp";
    } else {
        outPath = outImagePath.string();
    }

    GDALDatasetH hOut =
        GDALCreateCopy(webpDrv, outPath.c_str(), hSrc, FALSE, webpOpts, nullptr, nullptr);
    CSLDestroy(webpOpts);

    if (!hOut) {
        if (writeToMemory) VSIUnlink(outPath.c_str());
        throw GDALException(std::string("Cannot create WEBP thumbnail: ") + CPLGetLastErrorMsg());
    }
    GDALFlushCache(hOut);
    GDALClose(hOut);

    if (!writeToMemory)
        return;

    vsi_l_offset bufSize = 0;
    *outBuffer = VSIGetMemFileBuffer(outPath.c_str(), &bufSize, TRUE);
    if (*outBuffer == nullptr)
        throw GDALException("Failed to read WEBP thumbnail from vsimem");
    if (bufSize > static_cast<vsi_l_offset>(std::numeric_limits<int>::max())) {
        VSIFree(*outBuffer);
        *outBuffer = nullptr;
        throw GDALException("WEBP thumbnail exceeds maximum buffer size");
    }
    *outBufferSize = static_cast<int>(bufSize);
}

// Open a source raster, transparently falling back between a /vsicurl/ URL
// and the raw path when the input looks like a remote TIFF.
GDALDatasetH openThumbSource(const fs::path& imagePath, std::string& resolvedPath) {
    resolvedPath = imagePath.string();
    bool tryReopen = false;
    if (utils::isNetworkPath(resolvedPath) &&
        io::Path(resolvedPath).checkExtension({"tif", "tiff"})) {
        resolvedPath = "/vsicurl/" + resolvedPath;
        tryReopen = true;
    }

    GDALDatasetH hSrc = GDALOpen(resolvedPath.c_str(), GA_ReadOnly);
    if (!hSrc && tryReopen) {
        resolvedPath = imagePath.string();
        hSrc = GDALOpen(resolvedPath.c_str(), GA_ReadOnly);
    }
    if (!hSrc)
        throw GDALException("Cannot open " + resolvedPath + " for reading");
    return hSrc;
}

// Compute thumbnail target dimensions preserving aspect ratio.
// Guaranteed to return values >= 1 pixel.
void computeThumbTargetSize(int srcWidth, int srcHeight, int thumbSize,
                            int& outWidth, int& outHeight) {
    if (srcWidth <= 0 || srcHeight <= 0 || thumbSize <= 0)
        throw InvalidArgsException("Invalid thumbnail size parameters");

    if (srcWidth > srcHeight) {
        outWidth = thumbSize;
        outHeight = static_cast<int>((static_cast<float>(thumbSize) /
                                      static_cast<float>(srcWidth)) *
                                     static_cast<float>(srcHeight));
    } else {
        outHeight = thumbSize;
        outWidth = static_cast<int>((static_cast<float>(thumbSize) /
                                     static_cast<float>(srcHeight)) *
                                    static_cast<float>(srcWidth));
    }
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;
}

// Detect whether the source's palette includes at least one transparent entry.
bool paletteContainsAlpha(GDALColorTableH hColorTable) {
    if (!hColorTable) return false;
    const int colorCount = GDALGetColorEntryCount(hColorTable);
    for (int i = 0; i < colorCount; i++) {
        const GDALColorEntry* entry = GDALGetColorEntry(hColorTable, i);
        if (entry && entry->c4 < 255) return true;
    }
    return false;
}

// Build an in-memory RGBA dataset from a normalized RGB intermediate plus an
// explicit alpha buffer. The caller retains ownership of the returned handle.
GDALDatasetH buildRGBAMemDataset(GDALDatasetH hIntermediate,
                                 const uint8_t* alphaBuffer,
                                 int width, int height) {
    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (!memDrv)
        throw GDALException("MEM driver not available");

    GDALDatasetH hMem = GDALCreate(memDrv, "", width, height, 4, GDT_Byte, nullptr);
    if (!hMem)
        throw GDALException("Cannot create in-memory RGBA dataset");

    GdalDatasetGuard memGuard(hMem);

    // Copy RGB from intermediate → MEM bands 1..3.
    std::vector<uint8_t> rgbBuffer(static_cast<size_t>(width) * height * 3);
    if (GDALDatasetRasterIO(hIntermediate, GF_Read, 0, 0, width, height,
                            rgbBuffer.data(), width, height,
                            GDT_Byte, 3, nullptr, 0, 0, 0) != CE_None) {
        throw GDALException(std::string("Failed to read intermediate RGB: ") +
                            CPLGetLastErrorMsg());
    }
    if (GDALDatasetRasterIO(hMem, GF_Write, 0, 0, width, height,
                            rgbBuffer.data(), width, height,
                            GDT_Byte, 3, nullptr, 0, 0, 0) != CE_None) {
        throw GDALException(std::string("Failed to write RGB into MEM dataset: ") +
                            CPLGetLastErrorMsg());
    }

    // Write alpha to band 4 and mark the color interpretation accordingly so
    // downstream drivers (WEBP, PNG, …) treat it as a true alpha channel.
    GDALRasterBandH hAlphaOut = GDALGetRasterBand(hMem, 4);
    if (!hAlphaOut)
        throw GDALException("Cannot access alpha band in MEM dataset");
    if (GDALRasterIO(hAlphaOut, GF_Write, 0, 0, width, height,
                     const_cast<uint8_t*>(alphaBuffer), width, height,
                     GDT_Byte, 0, 0) != CE_None) {
        throw GDALException(std::string("Failed to write alpha into MEM dataset: ") +
                            CPLGetLastErrorMsg());
    }
    GDALSetRasterColorInterpretation(hAlphaOut, GCI_AlphaBand);

    return memGuard.release();
}

// Read an alpha source band at target resolution. Returns an empty vector if
// the alpha would be entirely opaque (in which case no alpha channel is
// required in the output).
std::vector<uint8_t> readAlphaBuffer(GDALRasterBandH hAlphaBand,
                                     int srcWidth, int srcHeight,
                                     int dstWidth, int dstHeight) {
    std::vector<uint8_t> buffer(static_cast<size_t>(dstWidth) * dstHeight);

    GDALRasterIOExtraArg extraArg;
    INIT_RASTERIO_EXTRA_ARG(extraArg);
    extraArg.eResampleAlg = GRIORA_Average;

    if (GDALRasterIOEx(hAlphaBand, GF_Read, 0, 0, srcWidth, srcHeight,
                       buffer.data(), dstWidth, dstHeight,
                       GDT_Byte, 0, 0, &extraArg) != CE_None) {
        throw GDALException(std::string("Failed to read alpha/mask band: ") +
                            CPLGetLastErrorMsg());
    }

    // If every pixel is fully opaque we can drop the alpha channel entirely.
    for (uint8_t v : buffer) {
        if (v < 255) return buffer;
    }
    return {};
}

}  // namespace

void generateImageThumb(const fs::path& imagePath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    // ---- Open source ---------------------------------------------------------
    std::string openPath;
    GdalDatasetGuard srcGuard(openThumbSource(imagePath, openPath));
    GDALDatasetH hSrc = srcGuard.handle;

    const int srcWidth = GDALGetRasterXSize(hSrc);
    const int srcHeight = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    if (srcWidth <= 0 || srcHeight <= 0 || bandCount <= 0)
        throw GDALException("Invalid source raster dimensions in " + openPath);

    GDALRasterBandH hBand1 = GDALGetRasterBand(hSrc, 1);
    GDALColorTableH hColorTable = hBand1 ? GDALGetRasterColorTable(hBand1) : nullptr;
    const bool hasPalette = (hColorTable != nullptr);
    const bool paletteHasAlpha = paletteContainsAlpha(hColorTable);
    (void)paletteHasAlpha;  // Alpha is always recovered via the mask band.

    // ---- Compute target size -------------------------------------------------
    int targetWidth = 0, targetHeight = 0;
    computeThumbTargetSize(srcWidth, srcHeight, thumbSize, targetWidth, targetHeight);

    // ---- Step 1: normalize source to an RGB Byte GTiff in vsimem -------------
    char** targs = buildThumbNormalizationArgs(targetWidth, targetHeight,
                                               hasPalette, bandCount);
    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);
    if (!psOptions)
        throw GDALException("Failed to create GDAL translate options for " + openPath);

    const std::string intermediatePath =
        "/vsimem/" + utils::generateRandomString(32) + ".tif";
    VsiMemGuard intermediateGuard(intermediatePath);

    int translateError = 0;
    GDALDatasetH hIntermediateRaw =
        GDALTranslate(intermediatePath.c_str(), hSrc, psOptions, &translateError);
    GDALTranslateOptionsFree(psOptions);
    if (!hIntermediateRaw) {
        throw GDALException("Failed to normalize " + openPath +
                            " for thumbnail: " + CPLGetLastErrorMsg());
    }
    GdalDatasetGuard intermediateGuardDs(hIntermediateRaw);

    // ---- Step 2: read alpha source (if any) ----------------------------------
    std::vector<uint8_t> alphaBuffer;
    GDALRasterBandH hAlphaBand = resolveAlphaBand(hSrc, bandCount);
    if (hAlphaBand != nullptr) {
        alphaBuffer = readAlphaBuffer(hAlphaBand,
                                      srcWidth, srcHeight,
                                      targetWidth, targetHeight);
    }

    // ---- Step 3: encode as WEBP ---------------------------------------------
    if (alphaBuffer.empty()) {
        LOGD << "Writing 3-band WEBP thumbnail for " << openPath;
        writeWebPFromDataset(hIntermediateRaw, outImagePath, outBuffer, outBufferSize);
        return;
    }

    LOGD << "Writing 4-band RGBA WEBP thumbnail for " << openPath;
    GdalDatasetGuard memGuard(buildRGBAMemDataset(hIntermediateRaw,
                                                  alphaBuffer.data(),
                                                  targetWidth, targetHeight));
    writeWebPFromDataset(memGuard.handle, outImagePath, outBuffer, outBufferSize);
}

void generateImageThumbEx(const fs::path& imagePath,
                          int thumbSize,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize,
                          const ThumbVisParams& visParams) {
    GDALDatasetH hSrcDataset = GDALOpen(imagePath.string().c_str(), GA_ReadOnly);
    if (!hSrcDataset)
        throw GDALException("Cannot open " + imagePath.string() + " for reading");

    const int width = GDALGetRasterXSize(hSrcDataset);
    const int height = GDALGetRasterYSize(hSrcDataset);
    const int bandCount = GDALGetRasterCount(hSrcDataset);
    const GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrcDataset, 1));

    int targetWidth, targetHeight;
    if (width > height) {
        targetWidth = thumbSize;
        targetHeight = std::max(1, static_cast<int>((static_cast<float>(thumbSize) / width) * height));
    } else {
        targetHeight = thumbSize;
        targetWidth = std::max(1, static_cast<int>((static_cast<float>(thumbSize) / height) * width));
    }

    // Determine band indices to use
    std::vector<int> selectedBands;

    if (!visParams.bands.empty()) {
        // Explicit bands provided
        std::istringstream ss(visParams.bands);
        std::string token;
        while (std::getline(ss, token, ',')) {
            int b;
            try {
                b = std::stoi(token);
            } catch (const std::exception &) {
                throw InvalidArgsException("Invalid band value: '" + token + "'");
            }
            if (b < 1 || b > bandCount)
                throw InvalidArgsException("Band index " + std::to_string(b) + " out of range");
            selectedBands.push_back(b);
        }
    } else if (!visParams.preset.empty()) {
        // Use preset from sensor profile
        auto& spm = SensorProfileManager::instance();
        auto mapping = spm.getBandMappingForPreset(imagePath.string(), visParams.preset);
        selectedBands = {mapping.r, mapping.g, mapping.b};
    } else {
        // Auto-detect sensor and use default mapping
        auto& spm = SensorProfileManager::instance();
        auto detection = spm.detectSensor(imagePath.string());
        if (detection.detected) {
            selectedBands = {detection.defaultBandMapping.r, detection.defaultBandMapping.g, detection.defaultBandMapping.b};
        }
        // If not multispectral, selectedBands stays empty → fall through to standard path
    }

    // Formula mode: read required bands, apply formula, apply colormap
    if (!visParams.formula.empty()) {
        auto& ve = VegetationEngine::instance();

        // Determine band filter
        BandFilter bf;
        if (!visParams.bandFilter.empty()) {
            bf = VegetationEngine::parseFilter(visParams.bandFilter, bandCount);
        } else {
            bf = ve.autoDetectFilter(imagePath.string());
        }

        // Read all bands at thumbnail resolution as float
        size_t pixCount = static_cast<size_t>(targetWidth) * targetHeight;
        std::vector<std::vector<float>> bandDataStorage(bandCount);
        std::vector<float*> bandDataPtrs(bandCount);

        auto nodataInfo = detectBandNodata(hSrcDataset, bandCount);
        for (int b = 0; b < bandCount; b++) {
            bandDataStorage[b].resize(pixCount);
            bandDataPtrs[b] = bandDataStorage[b].data();
            GDALRasterBandH hBand = GDALGetRasterBand(hSrcDataset, b + 1);
            if (GDALRasterIO(hBand, GF_Read, 0, 0, width, height,
                             bandDataPtrs[b], targetWidth, targetHeight,
                             GDT_Float32, 0, 0) != CE_None) {
                GDALClose(hSrcDataset);
                throw GDALException("Cannot read band " + std::to_string(b + 1));
            }
        }

        // Pre-mask transparent and nodata pixels
        float nodata = NODATA_SENTINEL;
        premaskNodata(bandDataPtrs, pixCount, bandCount, nodataInfo, nodata);

        // Apply formula
        std::vector<float> result(pixCount);
        const auto* formulaPtr = ve.getFormula(visParams.formula);
        if (!formulaPtr) {
            GDALClose(hSrcDataset);
            throw InvalidArgsException("Unknown formula: " + visParams.formula);
        }
        ve.applyFormula(*formulaPtr, bf, bandDataPtrs, result.data(), pixCount, nodata);

        // Determine rescale range
        float rMin, rMax;
        if (!visParams.rescale.empty()) {
            auto commaPos = visParams.rescale.find(',');
            if (commaPos == std::string::npos) {
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale format: " + visParams.rescale + ". Expected format: min,max");
            }
            std::string minStr = visParams.rescale.substr(0, commaPos);
            std::string maxStr = visParams.rescale.substr(commaPos + 1);
            if (minStr.empty() || maxStr.empty()) {
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale format: " + visParams.rescale + ". Expected format: min,max");
            }
            try {
                rMin = std::stof(minStr);
                rMax = std::stof(maxStr);
            } catch (const std::exception&) {
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale values: " + visParams.rescale + ". Expected numeric format: min,max");
            }
        } else {
            // Use formula's natural range or p2-p98
            bool found = false;
            if (formulaPtr->hasRange && formulaPtr->rangeMin != formulaPtr->rangeMax) {
                rMin = static_cast<float>(formulaPtr->rangeMin);
                rMax = static_cast<float>(formulaPtr->rangeMax);
                found = true;
            }
            if (!found) {
                // Compute p2-p98
                std::vector<float> valid;
                valid.reserve(pixCount);
                for (size_t i = 0; i < pixCount; i++) {
                    if (result[i] != nodata) valid.push_back(result[i]);
                }
                if (!valid.empty()) {
                    std::sort(valid.begin(), valid.end());
                    rMin = valid[static_cast<size_t>(valid.size() * 0.02)];
                    rMax = valid[static_cast<size_t>(std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98)))];
                } else {
                    rMin = 0; rMax = 1;
                }
            }
        }

        // Apply colormap
        std::string cmId = visParams.colormap.empty() ? "rdylgn" : visParams.colormap;
        const auto* cmap = ve.getColormap(cmId);
        if (!cmap) {
            GDALClose(hSrcDataset);
            throw InvalidArgsException("Unknown colormap: " + cmId);
        }
        std::vector<uint8_t> rgba(pixCount * 4);
        ve.applyColormap(result.data(), rgba.data(), pixCount, *cmap, rMin, rMax, nodata);

        GDALClose(hSrcDataset);

        // Write output via MEM → WEBP
        GDALDriverH memDrv = GDALGetDriverByName("MEM");
        if (!memDrv)
            throw GDALException("MEM driver not available");
        GDALDatasetH hMem = GDALCreate(memDrv, "", targetWidth, targetHeight, 4, GDT_Byte, nullptr);
        if (!hMem)
            throw GDALException("Cannot create in-memory dataset for formula thumbnail");
        for (int b = 0; b < 4; b++) {
            std::vector<uint8_t> chanData(pixCount);
            for (size_t i = 0; i < pixCount; i++) chanData[i] = rgba[i * 4 + b];
            GDALRasterBandH hBand = GDALGetRasterBand(hMem, b + 1);
            GDALRasterIO(hBand, GF_Write, 0, 0, targetWidth, targetHeight,
                         chanData.data(), targetWidth, targetHeight, GDT_Byte, 0, 0);
        }
        GDALSetRasterColorInterpretation(GDALGetRasterBand(hMem, 4), GCI_AlphaBand);

        GDALDriverH webpDrv = GDALGetDriverByName("WEBP");
        if (!webpDrv) {
            GDALClose(hMem);
            throw GDALException("WEBP driver not available");
        }
        char** webpOpts = nullptr;
        webpOpts = CSLAddString(webpOpts, "QUALITY=95");

        bool writeToMemory = outImagePath.empty() && outBuffer != nullptr;
        if (writeToMemory) {
            std::string vsiPath = "/vsimem/" + utils::generateRandomString(32) + ".webp";
            GDALDatasetH hOut = GDALCreateCopy(webpDrv, vsiPath.c_str(), hMem, FALSE, webpOpts, nullptr, nullptr);
            if (!hOut) {
                CSLDestroy(webpOpts);
                GDALClose(hMem);
                throw GDALException("Cannot create formula thumbnail");
            }
            GDALFlushCache(hOut);
            GDALClose(hOut);
            vsi_l_offset bufSize;
            *outBuffer = VSIGetMemFileBuffer(vsiPath.c_str(), &bufSize, TRUE);
            if (!*outBuffer) {
                CSLDestroy(webpOpts);
                GDALClose(hMem);
                throw GDALException("Cannot retrieve formula thumbnail buffer from vsimem");
            }
            *outBufferSize = static_cast<int>(bufSize);
        } else {
            GDALDatasetH hOut = GDALCreateCopy(webpDrv, outImagePath.string().c_str(), hMem, FALSE, webpOpts, nullptr, nullptr);
            if (!hOut) {
                CSLDestroy(webpOpts);
                GDALClose(hMem);
                throw GDALException("Cannot create formula thumbnail");
            }
            GDALFlushCache(hOut);
            GDALClose(hOut);
        }
        CSLDestroy(webpOpts);
        GDALClose(hMem);
        return;
    }

    // Non-formula mode: band selection + stretch
    char** targs = nullptr;
    targs = CSLAddString(targs, "-outsize");
    targs = CSLAddString(targs, std::to_string(targetWidth).c_str());
    targs = CSLAddString(targs, std::to_string(targetHeight).c_str());
    targs = CSLAddString(targs, "-ot");
    targs = CSLAddString(targs, "Byte");
    targs = CSLAddString(targs, "-r");
    targs = CSLAddString(targs, "average");

    if (!selectedBands.empty()) {
        for (int b : selectedBands) {
            targs = CSLAddString(targs, "-b");
            targs = CSLAddString(targs, std::to_string(b).c_str());
        }
    } else if (bandCount == 1) {
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
    } else if (bandCount == 2) {
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "2");
    } else {
        // Default: first 3 bands
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "2");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "3");
    }

    // Stretch: for non-Byte data, use -scale
    if (srcType != GDT_Byte) {
        if (!visParams.rescale.empty()) {
            auto commaPos = visParams.rescale.find(',');
            if (commaPos == std::string::npos) {
                CSLDestroy(targs);
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale format: " + visParams.rescale + ". Expected format: min,max");
            }
            std::string sMin = visParams.rescale.substr(0, commaPos);
            std::string sMax = visParams.rescale.substr(commaPos + 1);
            if (sMin.empty() || sMax.empty()) {
                CSLDestroy(targs);
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale format: " + visParams.rescale + ". Expected format: min,max");
            }
            try {
                (void)std::stof(sMin);
                (void)std::stof(sMax);
            } catch (const std::exception&) {
                CSLDestroy(targs);
                GDALClose(hSrcDataset);
                throw InvalidArgsException("Invalid rescale values: " + visParams.rescale + ". Expected numeric format: min,max");
            }
            targs = CSLAddString(targs, "-scale");
            targs = CSLAddString(targs, sMin.c_str());
            targs = CSLAddString(targs, sMax.c_str());
            targs = CSLAddString(targs, "0");
            targs = CSLAddString(targs, "255");
        } else {
            // Percentile stretch: compute p2-p98 on selected bands
            std::vector<int> bandsToSample;
            if (!selectedBands.empty()) {
                bandsToSample = selectedBands;
            } else {
                for (int i = 1; i <= std::min(3, bandCount); i++) bandsToSample.push_back(i);
            }

            double globalMin = std::numeric_limits<double>::max();
            double globalMax = std::numeric_limits<double>::lowest();
            for (int b : bandsToSample) {
                double bMin, bMax, bMean, bStdDev;
                GDALRasterBandH hBand = GDALGetRasterBand(hSrcDataset, b);
                if (GDALComputeRasterStatistics(hBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr) == CE_None) {
                    // p2-p98 approximation: mean ± 2.33*stdDev (for normal distribution)
                    double p2 = std::max(bMin, bMean - 2.33 * bStdDev);
                    double p98 = std::min(bMax, bMean + 2.33 * bStdDev);
                    globalMin = std::min(globalMin, p2);
                    globalMax = std::max(globalMax, p98);
                }
            }
            if (globalMin < globalMax) {
                targs = CSLAddString(targs, "-scale");
                targs = CSLAddString(targs, std::to_string(globalMin).c_str());
                targs = CSLAddString(targs, std::to_string(globalMax).c_str());
                targs = CSLAddString(targs, "0");
                targs = CSLAddString(targs, "255");
            } else {
                targs = CSLAddString(targs, "-scale");
            }
        }
    } else {
        // Byte data might still need scale for consistency
        targs = CSLAddString(targs, "-scale");
    }

    targs = CSLAddString(targs, "-of");
    targs = CSLAddString(targs, "WEBP");
    targs = CSLAddString(targs, "-co");
    targs = CSLAddString(targs, "QUALITY=95");
    targs = CSLAddString(targs, "-a_srs");
    targs = CSLAddString(targs, "");

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);

    if (!psOptions) {
        GDALClose(hSrcDataset);
        throw GDALException("Failed to create GDAL translate options");
    }

    bool writeToMemory = outImagePath.empty() && outBuffer != nullptr;
    if (writeToMemory) {
        std::string vsiPath = "/vsimem/" + utils::generateRandomString(32) + ".webp";
        GDALDatasetH hNew = GDALTranslate(vsiPath.c_str(), hSrcDataset, psOptions, nullptr);
        if (!hNew) {
            VSIUnlink(vsiPath.c_str());
            GDALTranslateOptionsFree(psOptions);
            GDALClose(hSrcDataset);
            throw GDALException("Failed to generate thumbnail");
        }
        GDALFlushCache(hNew);
        GDALClose(hNew);
        vsi_l_offset bufSize;
        *outBuffer = VSIGetMemFileBuffer(vsiPath.c_str(), &bufSize, TRUE);
        *outBufferSize = static_cast<int>(bufSize);
    } else {
        GDALDatasetH hNew = GDALTranslate(outImagePath.string().c_str(), hSrcDataset, psOptions, nullptr);
        if (!hNew) {
            GDALTranslateOptionsFree(psOptions);
            GDALClose(hSrcDataset);
            throw GDALException("Failed to generate thumbnail");
        }
        GDALFlushCache(hNew);
        GDALClose(hNew);
    }

    GDALTranslateOptionsFree(psOptions);
    GDALClose(hSrcDataset);
}

void RenderImage(const fs::path& outImagePath,
                 const int tileSize,
                 const int nBands,
                 uint8_t* buffer,
                 uint8_t* alphaBuffer = nullptr,
                 uint8_t** outBuffer = nullptr,
                 int* outBufferSize = nullptr) {
    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (memDrv == nullptr)
        throw GDALException("Cannot create MEM driver");

    GDALDriverH webpDrv = GDALGetDriverByName("WEBP");
    if (webpDrv == nullptr)
        throw GDALException("Cannot create WEBP driver");  // Determine the effective number of bands
                                                           // (3 RGB + optionally 1 Alpha)
    int effectiveBands = nBands;
    if (alphaBuffer != nullptr) {
        effectiveBands = 4;  // RGB + Alpha
    }

    // Need to create in-memory dataset
    const GDALDatasetH hDataset =
        GDALCreate(memDrv, "", tileSize, tileSize, effectiveBands, GDT_Byte, nullptr);
    if (hDataset == nullptr)
        throw GDALException("Cannot create GDAL dataset");

    // Write RGB data
    if (GDALDatasetRasterIO(hDataset,
                            GF_Write,
                            0,
                            0,
                            tileSize,
                            tileSize,
                            buffer,
                            tileSize,
                            tileSize,
                            GDT_Byte,
                            nBands,
                            nullptr,
                            0,
                            0,
                            0) != CE_None) {
        throw GDALException("Cannot write tile data");
    }

    // If we have alphaBuffer, also write the alpha channel
    if (alphaBuffer != nullptr) {
        GDALRasterBandH alphaBand = GDALGetRasterBand(hDataset, 4);
        if (GDALRasterIO(alphaBand,
                         GF_Write,
                         0,
                         0,
                         tileSize,
                         tileSize,
                         alphaBuffer,
                         tileSize,
                         tileSize,
                         GDT_Byte,
                         0,
                         0) != CE_None) {
            throw GDALException("Cannot write alpha channel data");
        }
    }

    // Define consistent WEBP creation options
    char** webpOpts = nullptr;
    webpOpts = CSLAddString(webpOpts, "QUALITY=95");
    webpOpts = CSLAddString(webpOpts, "WRITE_EXIF_METADATA=NO");

    bool writeToMemory = outImagePath.empty() && outBuffer != nullptr;
    if (writeToMemory) {
        // Write to memory via vsimem
        std::string vsiPath = "/vsimem/" + utils::generateRandomString(32) + ".webp";
        const GDALDatasetH outDs =
            GDALCreateCopy(webpDrv, vsiPath.c_str(), hDataset, FALSE, webpOpts, nullptr, nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " + outImagePath.string());
        GDALFlushCache(outDs);
        GDALClose(outDs);

        // Read memory to buffer
        vsi_l_offset bufSize;
        *outBuffer = VSIGetMemFileBuffer(vsiPath.c_str(), &bufSize, TRUE);
        if (bufSize > std::numeric_limits<int>::max())
            throw GDALException("Exceeded max buf size");
        *outBufferSize = bufSize;
    } else {
        const GDALDatasetH outDs = GDALCreateCopy(webpDrv,
                                                  outImagePath.string().c_str(),
                                                  hDataset,
                                                  FALSE,
                                                  webpOpts,
                                                  nullptr,
                                                  nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " + outImagePath.string() + ": " + CPLGetLastErrorMsg());
        GDALFlushCache(outDs);
        GDALClose(outDs);
    }

    CSLDestroy(webpOpts);
    GDALClose(hDataset);
}

// Helper function to render points with optional coordinate transformation
int renderPoints(pdal::PointViewPtr point_view,
                const std::vector<PointColor>& colors,
                bool hasSpatialSystem,
                const std::string& wktProjection,
                uint8_t* buffer,
                uint8_t* alphaBuffer,
                float* zBuffer,
                int tileSize,
                double tileScale,
                double offsetX,
                double offsetY,
                double oMinX,
                double oMinY) {
    int pointsRendered = 0;
    const auto wSize = tileSize * tileSize;

    if (hasSpatialSystem) {
        CoordsTransformer ict(wktProjection, 3857);

        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            auto p = point_view->point(idx);
            auto x = p.getFieldAs<double>(pdal::Dimension::Id::X);
            auto y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
            auto z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

            ict.transform(&x, &y);

            // Map projected coordinates to local PNG coordinates
            int px = std::round((x - oMinX) * tileScale + offsetX);
            int py = tileSize - 1 - std::round((y - oMinY) * tileScale + offsetY);

            if (px >= 0 && px < tileSize && py >= 0 && py < tileSize &&
                zBuffer[py * tileSize + px] < z) {
                zBuffer[py * tileSize + px] = z;
                drawCircle(buffer, alphaBuffer, px, py, 2,
                          colors[idx].r, colors[idx].g, colors[idx].b,
                          tileSize, wSize);
                pointsRendered++;
            }
        }
    } else {
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            auto p = point_view->point(idx);
            auto x = p.getFieldAs<double>(pdal::Dimension::Id::X);
            auto y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
            auto z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

            // Map coordinates to local PNG coordinates
            int px = std::round((x - oMinX) * tileScale + offsetX);
            int py = tileSize - 1 - std::round((y - oMinY) * tileScale + offsetY);

            if (px >= 0 && px < tileSize && py >= 0 && py < tileSize &&
                zBuffer[py * tileSize + px] < z) {
                zBuffer[py * tileSize + px] = z;
                drawCircle(buffer, alphaBuffer, px, py, 2,
                          colors[idx].r, colors[idx].g, colors[idx].b,
                          tileSize, wSize);
                pointsRendered++;
            }
        }
    }

    return pointsRendered;
}

void generatePointCloudThumb(const fs::path& eptPath,
                             int thumbSize,
                             const fs::path& outImagePath,
                             uint8_t** outBuffer,
                             int* outBufferSize) {
    PointCloudInfo eptInfo;

    // Load EPT information
    int span;
    if (!getEptInfo(eptPath.string(), eptInfo, 3857, &span)) {
        throw InvalidArgsException("Cannot get EPT info for " + eptPath.string());
    }

    // Validate bounds array has required elements (minX, minY, minZ, maxX, maxY, maxZ)
    if (eptInfo.bounds.size() < 6) {
        throw InvalidArgsException("EPT bounds array does not contain at least 6 elements (minX, minY, minZ, maxX, maxY, maxZ required)");
    }

    const auto tileSize = thumbSize;
    GlobalMercator mercator(tileSize);

    // Calculate bounds based on spatial reference system
    double oMinX, oMaxX, oMaxY, oMinY;
    bool hasSpatialSystem = !eptInfo.wktProjection.empty() && !eptInfo.polyBounds.empty();

    if (hasSpatialSystem) {
        oMinX = eptInfo.polyBounds.getPoint(0).x;
        oMaxX = eptInfo.polyBounds.getPoint(2).x;
        oMaxY = eptInfo.polyBounds.getPoint(2).y;
        oMinY = eptInfo.polyBounds.getPoint(0).y;
    } else {
        oMinX = eptInfo.bounds[0];
        oMinY = eptInfo.bounds[1];
        oMaxX = eptInfo.bounds[3];
        oMaxY = eptInfo.bounds[4];
    }

    // Calculate length for zoom level determination
    auto length = std::min(std::abs(oMaxX - oMinX), std::abs(oMaxY - oMinY));

    if (length == 0) {
        // Fallback to raw bounds if transformed bounds are invalid
        oMinX = eptInfo.bounds[0];
        oMaxX = eptInfo.bounds[3];
        oMaxY = eptInfo.bounds[4];
        oMinY = eptInfo.bounds[1];

        length = std::min(std::abs(oMaxX - oMinX), std::abs(oMaxY - oMinY));

        if (length == 0) {
            throw GDALException("Cannot calculate length: point cloud has zero extent");
        }

        hasSpatialSystem = false;
    }

    // Determine zoom level and check for color dimensions
    const auto tMinZ = mercator.zoomForLength(length);
    const auto hasColors =
        std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Red") !=
            eptInfo.dimensions.end() &&
        std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Green") !=
            eptInfo.dimensions.end() &&
        std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Blue") !=
            eptInfo.dimensions.end();

#ifdef _WIN32
    const fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
    if (!caBundlePath.empty()) {
        LOGD << "ARBITRER CA Bundle: " << caBundlePath.string();
        std::stringstream ss;
        ss << "ARBITER_CA_INFO=" << caBundlePath.string();
        if (_putenv(ss.str().c_str()) != 0) {
            LOGD << "Cannot set ARBITER_CA_INFO";
        }
    }
#endif

    // Configure EPT reader options
    const auto tz = tMinZ;
    double resolution = tz < 0 ? 1 : mercator.resolution(tz);

    pdal::Options eptOpts;
    eptOpts.add("filename",
                (!utils::isNetworkPath(eptPath.string()) && eptPath.is_relative())
                    ? ("." / eptPath).string()
                    : eptPath.string());
    eptOpts.add("resolution", resolution);

    std::unique_ptr<pdal::EptReader> eptReader = std::make_unique<pdal::EptReader>();
    pdal::Stage* main = eptReader.get();
    eptReader->setOptions(eptOpts);

    // Execute PDAL pipeline with comprehensive error handling
    pdal::PointTable table;
    pdal::PointViewSet point_view_set;
    pdal::PointViewPtr point_view;

    try {
        main->prepare(table);
        point_view_set = main->execute(table);
        point_view = *point_view_set.begin();

        if (point_view->empty()) {
            throw GDALException("No points fetched from cloud, check zoom level");
        }
    } catch (const pdal::pdal_error& e) {
        throw PDALException("PDAL error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw PDALException("Failed to process point cloud pipeline: " + std::string(e.what()));
    }

    // Initialize rendering buffers
    const auto wSize = tileSize * tileSize;
    constexpr int nBands = 3;
    const int bufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * wSize;

    std::unique_ptr<uint8_t> buffer(new uint8_t[bufSize * nBands]);
    std::unique_ptr<uint8_t> alphaBuffer(new uint8_t[bufSize]);
    std::unique_ptr<float> zBuffer(new float[bufSize]);

    memset(buffer.get(), 0, bufSize * nBands);
    memset(alphaBuffer.get(), 0, bufSize);
    std::fill_n(zBuffer.get(), wSize, -99999.0f);

    // Calculate scaling and offset for centering
    const double width = oMaxX - oMinX;
    const double height = oMaxY - oMinY;
    const double tileScaleW = tileSize / width;
    const double tileScaleH = tileSize / height;

    double tileScale, offsetX, offsetY;
    if (tileScaleW > tileScaleH) {
        // Taller than wider
        tileScale = tileScaleH;
        offsetY = 0;
        offsetX = (tileSize - width * tileScaleH) / 2;
    } else {
        // Wider than taller
        tileScale = tileScaleW;
        offsetX = 0;
        offsetY = (tileSize - height * tileScaleW) / 2;
    }

    // Generate colors based on available data
    std::vector<PointColor> colors = hasColors ?
        normalizeColors(point_view) :
        generateZBasedColors(point_view, eptInfo.bounds[2], eptInfo.bounds[5]);

    // Render points using appropriate coordinate transformation
    int pointsRendered = renderPoints(point_view, colors, hasSpatialSystem,
                                     eptInfo.wktProjection, buffer.get(), alphaBuffer.get(),
                                     zBuffer.get(), tileSize, tileScale, offsetX, offsetY,
                                     oMinX, oMinY);

    RenderImage(outImagePath,
                tileSize,
                nBands,
                buffer.get(),
                alphaBuffer.get(),
                outBuffer,
                outBufferSize);
}

// imagePath can be either absolute or relative or a network URL and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path& inputPath,
                       int thumbSize,
                       const fs::path& outImagePath,
                       bool forceRecreate,
                       uint8_t** outBuffer,
                       int* outBufferSize) {
    if (thumbSize <= 0)
        throw InvalidArgsException("thumbSize must be greater than 0");

    if (!utils::isNetworkPath(inputPath.string()) && !exists(inputPath))
        throw FSException(inputPath.string() + " does not exist");

    // Check existance of thumbnail, return if exists
    if (!utils::isNetworkPath(inputPath.string()) && exists(outImagePath) && !forceRecreate)
        return outImagePath;

    LOGD << "ImagePath = " << inputPath;
    LOGD << "OutImagePath = " << outImagePath;
    LOGD << "Size = " << thumbSize;

    if (inputPath.filename() == "ept.json")
        generatePointCloudThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);
    else
        generateImageThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);

    return outImagePath;
}

void cleanupThumbsUserCache() {
    LOGD << "Cleaning up thumbs user cache";

    const time_t threshold = utils::currentUnixTimestamp() - 60 * 60 * 24 * 5;  // 5 days
    const fs::path thumbsDir = UserProfile::get()->getThumbsDir();
    std::vector<fs::path> cleanupDirs;

    // Iterate size directories
    for (auto sd = fs::recursive_directory_iterator(thumbsDir);
         sd != fs::recursive_directory_iterator();
         ++sd) {
        fs::path sizeDir = sd->path();
        if (is_directory(sizeDir)) {
            for (auto t = fs::recursive_directory_iterator(sizeDir);
                 t != fs::recursive_directory_iterator();
                 ++t) {
                fs::path thumb = t->path();
                if (io::Path(thumb).getModifiedTime() < threshold) {
                    if (fs::remove(thumb))
                        LOGD << "Cleaned " << thumb.string();
                    else
                        LOGD << "Cannot clean " << thumb.string();
                }
            }

            if (is_empty(sizeDir)) {
                // Remove directory too
                cleanupDirs.push_back(sizeDir);
            }
        }
    }

    for (auto& d : cleanupDirs) {
        if (fs::remove(d))
            LOGD << "Cleaned " << d.string();
        else
            LOGD << "Cannot clean " << d.string();
    }
}

}  // namespace ddb
