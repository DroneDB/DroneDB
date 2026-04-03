/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "merge_multispectral.h"
#include "gdal_inc.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "utils.h"

#include <cmath>
#include <sstream>
#include <algorithm>

namespace ddb {

static std::string gdalTypeName(GDALDataType dt) {
    switch (dt) {
        case GDT_Byte: return "Byte";
        case GDT_UInt16: return "UInt16";
        case GDT_Int16: return "Int16";
        case GDT_UInt32: return "UInt32";
        case GDT_Int32: return "Int32";
        case GDT_Float32: return "Float32";
        case GDT_Float64: return "Float64";
        default: return "Unknown";
    }
}

MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths) {
    MergeValidationResult result;

    if (inputPaths.size() < 2) {
        result.errors.push_back("At least 2 files required");
        return result;
    }

    // Open first file as reference
    GDALDatasetH hRef = GDALOpen(inputPaths[0].c_str(), GA_ReadOnly);
    if (!hRef) {
        result.errors.push_back("Cannot open " + inputPaths[0]);
        return result;
    }

    int refWidth = GDALGetRasterXSize(hRef);
    int refHeight = GDALGetRasterYSize(hRef);
    GDALDataType refType = GDALGetRasterDataType(GDALGetRasterBand(hRef, 1));

    double refGt[6];
    if (GDALGetGeoTransform(hRef, refGt) != CE_None) {
        result.warnings.push_back("Cannot read geotransform from reference file: " + inputPaths[0]);
    }

    const char *refProj = GDALGetProjectionRef(hRef);
    OGRSpatialReferenceH refSrs = OSRNewSpatialReference(refProj);

    result.summary.crs = refProj ? std::string(refProj) : "";
    result.summary.width = refWidth;
    result.summary.height = refHeight;
    result.summary.pixelSizeX = refGt[1];
    result.summary.pixelSizeY = refGt[5];
    result.summary.dataType = gdalTypeName(refType);
    result.summary.totalBands = 0;

    // Check skew on reference
    if (refGt[2] != 0 || refGt[4] != 0) {
        result.errors.push_back("Reference file has non-zero skew (rotation not supported)");
    }

    int totalBands = GDALGetRasterCount(hRef);

    if (GDALGetRasterCount(hRef) > 1) {
        result.warnings.push_back(inputPaths[0] + " has " + std::to_string(GDALGetRasterCount(hRef)) + " bands");
    }

    for (size_t i = 1; i < inputPaths.size(); i++) {
        GDALDatasetH hDs = GDALOpen(inputPaths[i].c_str(), GA_ReadOnly);
        if (!hDs) {
            result.errors.push_back("Cannot open " + inputPaths[i]);
            continue;
        }

        // CRS check
        const char *proj = GDALGetProjectionRef(hDs);
        OGRSpatialReferenceH srs = OSRNewSpatialReference(proj);
        if (!OSRIsSame(refSrs, srs)) {
            result.errors.push_back("CRS mismatch: " + inputPaths[i]);
        }
        OSRDestroySpatialReference(srs);

        // Dimension check
        if (GDALGetRasterXSize(hDs) != refWidth || GDALGetRasterYSize(hDs) != refHeight) {
            result.errors.push_back("Dimension mismatch: " + inputPaths[i] +
                " (" + std::to_string(GDALGetRasterXSize(hDs)) + "x" + std::to_string(GDALGetRasterYSize(hDs)) +
                " vs " + std::to_string(refWidth) + "x" + std::to_string(refHeight) + ")");
        }

        // Data type check
        GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hDs, 1));
        if (dt != refType) {
            result.errors.push_back("Data type mismatch: " + inputPaths[i] +
                " (" + gdalTypeName(dt) + " vs " + gdalTypeName(refType) + ")");
        }

        // Geotransform check
        double gt[6];
        if (GDALGetGeoTransform(hDs, gt) != CE_None) {
            result.warnings.push_back("Cannot read geotransform from: " + inputPaths[i]);
        }

        // Skew check
        if (gt[2] != 0 || gt[4] != 0) {
            result.errors.push_back(inputPaths[i] + " has non-zero skew");
        }

        // Pixel size tolerance (relative)
        double pxDiffX = std::abs(gt[1] - refGt[1]);
        double pxDiffY = std::abs(gt[5] - refGt[5]);
        double relDiffX = (std::abs(refGt[1]) > 0) ? pxDiffX / std::abs(refGt[1]) : 0;
        double relDiffY = (std::abs(refGt[5]) > 0) ? pxDiffY / std::abs(refGt[5]) : 0;

        if (relDiffX > 0.05 || relDiffY > 0.05) {
            result.errors.push_back("Pixel size mismatch > 5%: " + inputPaths[i]);
        } else if (relDiffX > 0.01 || relDiffY > 0.01) {
            result.warnings.push_back("Resolution differs slightly for " + inputPaths[i] + ", bands will be resampled");
        }

        // Origin tolerance (1 pixel)
        double origDiffX = std::abs(gt[0] - refGt[0]);
        double origDiffY = std::abs(gt[3] - refGt[3]);
        if (origDiffX > std::abs(refGt[1]) || origDiffY > std::abs(refGt[5])) {
            result.errors.push_back("Origin mismatch > 1 pixel: " + inputPaths[i]);
        }

        if (GDALGetRasterCount(hDs) > 1) {
            result.warnings.push_back(inputPaths[i] + " has " + std::to_string(GDALGetRasterCount(hDs)) + " bands");
        }

        totalBands += GDALGetRasterCount(hDs);
        GDALClose(hDs);
    }

    OSRDestroySpatialReference(refSrs);
    GDALClose(hRef);

    result.summary.totalBands = totalBands;
    result.summary.estimatedSize = static_cast<size_t>(refWidth) * refHeight * totalBands * GDALGetDataTypeSizeBytes(refType);
    result.ok = result.errors.empty();

    return result;
}

void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                const std::vector<int> &previewBands,
                                int thumbSize,
                                uint8_t **outBuffer, int *outBufferSize) {
    if (inputPaths.empty()) throw InvalidArgsException("No input paths");
    if (previewBands.size() < 3) throw InvalidArgsException("Need at least 3 preview bands");

    // Build VRT with separate=TRUE
    char **vrtArgs = nullptr;
    vrtArgs = CSLAddString(vrtArgs, "-separate");
    vrtArgs = CSLAddString(vrtArgs, "-r");
    vrtArgs = CSLAddString(vrtArgs, "average");

    GDALBuildVRTOptions *vrtOpts = GDALBuildVRTOptionsNew(vrtArgs, nullptr);
    CSLDestroy(vrtArgs);

    std::vector<GDALDatasetH> datasets;
    for (const auto &p : inputPaths) {
        GDALDatasetH ds = GDALOpen(p.c_str(), GA_ReadOnly);
        if (!ds) {
            for (auto d : datasets) GDALClose(d);
            GDALBuildVRTOptionsFree(vrtOpts);
            throw GDALException("Cannot open " + p);
        }
        datasets.push_back(ds);
    }

    std::string vsiVrtPath = "/vsimem/" + utils::generateRandomString(16) + ".vrt";
    GDALDatasetH hVrt = GDALBuildVRT(vsiVrtPath.c_str(), static_cast<int>(datasets.size()),
                                      datasets.data(), nullptr, vrtOpts, nullptr);
    GDALBuildVRTOptionsFree(vrtOpts);

    if (!hVrt) {
        for (auto d : datasets) GDALClose(d);
        throw GDALException("Cannot build VRT");
    }
    GDALFlushCache(hVrt);

    // Translate with band selection and scaling
    char **tArgs = nullptr;
    tArgs = CSLAddString(tArgs, "-of");
    tArgs = CSLAddString(tArgs, "WEBP");
    tArgs = CSLAddString(tArgs, "-outsize");
    tArgs = CSLAddString(tArgs, std::to_string(thumbSize).c_str());
    tArgs = CSLAddString(tArgs, std::to_string(thumbSize).c_str());
    tArgs = CSLAddString(tArgs, "-ot");
    tArgs = CSLAddString(tArgs, "Byte");
    tArgs = CSLAddString(tArgs, "-scale");

    for (int band : previewBands) {
        tArgs = CSLAddString(tArgs, "-b");
        tArgs = CSLAddString(tArgs, std::to_string(band).c_str());
    }

    GDALTranslateOptions *transOpts = GDALTranslateOptionsNew(tArgs, nullptr);
    CSLDestroy(tArgs);

    std::string vsiOutPath = "/vsimem/" + utils::generateRandomString(16) + ".webp";
    GDALDatasetH hOut = GDALTranslate(vsiOutPath.c_str(), hVrt, transOpts, nullptr);
    GDALTranslateOptionsFree(transOpts);

    if (!hOut) {
        GDALClose(hVrt);
        for (auto d : datasets) GDALClose(d);
        VSIUnlink(vsiVrtPath.c_str());
        throw GDALException("Cannot generate preview");
    }

    GDALFlushCache(hOut);
    GDALClose(hOut);
    GDALClose(hVrt);
    for (auto d : datasets) GDALClose(d);

    // Read memory buffer
    vsi_l_offset bufSize;
    *outBuffer = VSIGetMemFileBuffer(vsiOutPath.c_str(), &bufSize, TRUE);
    *outBufferSize = static_cast<int>(bufSize);

    VSIUnlink(vsiVrtPath.c_str());
}

void mergeMultispectral(const std::vector<std::string> &inputPaths,
                         const std::string &outputCog) {
    if (inputPaths.size() < 2) throw InvalidArgsException("At least 2 files required");

    if (fs::exists(outputCog)) {
        throw AppException("Output file already exists: " + outputCog);
    }

    auto validation = validateMergeMultispectral(inputPaths);
    if (!validation.ok) {
        std::string errMsg = "Validation failed:";
        for (const auto &e : validation.errors) errMsg += "\n  - " + e;
        throw AppException(errMsg);
    }

    // Build VRT with separate=TRUE
    char **vrtArgs = nullptr;
    vrtArgs = CSLAddString(vrtArgs, "-separate");
    vrtArgs = CSLAddString(vrtArgs, "-r");
    vrtArgs = CSLAddString(vrtArgs, "average");

    GDALBuildVRTOptions *vrtOpts = GDALBuildVRTOptionsNew(vrtArgs, nullptr);
    CSLDestroy(vrtArgs);

    std::vector<GDALDatasetH> datasets;
    for (const auto &p : inputPaths) {
        GDALDatasetH ds = GDALOpen(p.c_str(), GA_ReadOnly);
        if (!ds) {
            for (auto d : datasets) GDALClose(d);
            GDALBuildVRTOptionsFree(vrtOpts);
            throw GDALException("Cannot open " + p);
        }
        datasets.push_back(ds);
    }

    std::string vsiVrtPath = "/vsimem/" + utils::generateRandomString(16) + ".vrt";
    GDALDatasetH hVrt = GDALBuildVRT(vsiVrtPath.c_str(), static_cast<int>(datasets.size()),
                                      datasets.data(), nullptr, vrtOpts, nullptr);
    GDALBuildVRTOptionsFree(vrtOpts);

    if (!hVrt) {
        for (auto d : datasets) GDALClose(d);
        throw GDALException("Cannot build VRT for merge");
    }
    GDALFlushCache(hVrt);

    // Determine compression
    GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hVrt, 1));
    int nBands = GDALGetRasterCount(hVrt);
    bool useJpeg = (dt == GDT_Byte && (nBands == 3 || nBands == 4));

    // Warp to COG
    char **warpArgs = nullptr;
    warpArgs = CSLAddString(warpArgs, "-of");
    warpArgs = CSLAddString(warpArgs, "COG");
    warpArgs = CSLAddString(warpArgs, "-multi");
    warpArgs = CSLAddString(warpArgs, "-wo");
    warpArgs = CSLAddString(warpArgs, "NUM_THREADS=ALL_CPUS");
    warpArgs = CSLAddString(warpArgs, "-co");
    warpArgs = CSLAddString(warpArgs, "NUM_THREADS=ALL_CPUS");
    warpArgs = CSLAddString(warpArgs, "-co");
    warpArgs = CSLAddString(warpArgs, "BIGTIFF=IF_SAFER");
    warpArgs = CSLAddString(warpArgs, "-co");
    warpArgs = CSLAddString(warpArgs, "PREDICTOR=YES");

    if (useJpeg) {
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "COMPRESS=JPEG");
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "QUALITY=90");
    } else {
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "COMPRESS=LZW");
    }

    GDALWarpAppOptions *warpOpts = GDALWarpAppOptionsNew(warpArgs, nullptr);
    CSLDestroy(warpArgs);

    GDALDatasetH hOut = GDALWarp(outputCog.c_str(), nullptr, 1, &hVrt, warpOpts, nullptr);
    GDALWarpAppOptionsFree(warpOpts);

    if (!hOut) {
        GDALClose(hVrt);
        for (auto d : datasets) GDALClose(d);
        VSIUnlink(vsiVrtPath.c_str());
        throw GDALException("Cannot create merged COG: " + outputCog);
    }

    GDALFlushCache(hOut);
    GDALClose(hOut);
    GDALClose(hVrt);
    for (auto d : datasets) GDALClose(d);
    VSIUnlink(vsiVrtPath.c_str());

    LOGD << "Merged " << inputPaths.size() << " bands into " << outputCog;
}

} // namespace ddb
