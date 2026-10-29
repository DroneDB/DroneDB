/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The validation half of a multispectral merge: do the files describe one scene (same CRS, size,
// type, grid), and would the bands need an alignment correction.

#include <algorithm>
#include <cmath>

#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "logger.h"
#include "merge_multispectral.h"
#include "mio.h"

namespace ddb {

namespace {

std::string gdalTypeName(GDALDataType dt) {
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

// The first file, which every other file is compared with
struct ReferenceRaster {
    int width = 0;
    int height = 0;
    GDALDataType type = GDT_Unknown;
    double gt[6] = {0, 1, 0, 0, 0, -1};
    bool hasGeoTransform = false;
    std::string projection;
    SrsPtr srs;
    int bands = 0;
};

// Reads the reference and records its summary, its warnings and its errors in result
ReferenceRaster checkReference(GDALDatasetH hRef, const std::string &displayName, MergeValidationResult &result) {
    ReferenceRaster ref;
    ref.width = GDALGetRasterXSize(hRef);
    ref.height = GDALGetRasterYSize(hRef);
    ref.type = GDALGetRasterDataType(GDALGetRasterBand(hRef, 1));

    ref.hasGeoTransform = (GDALGetGeoTransform(hRef, ref.gt) == CE_None);
    if (!ref.hasGeoTransform) {
        result.warnings.push_back("Cannot read geotransform from reference file: " + displayName);
    }

    const char *refProj = GDALGetProjectionRef(hRef);
    ref.srs.reset(OSRNewSpatialReference(refProj));
    // A copy: the pointer belongs to the dataset, which is closed before the alignment check
    ref.projection = refProj ? std::string(refProj) : "";

    result.summary.crs = ref.projection;
    result.summary.width = ref.width;
    result.summary.height = ref.height;
    result.summary.pixelSizeX = ref.gt[1];
    result.summary.pixelSizeY = ref.gt[5];
    result.summary.dataType = gdalTypeName(ref.type);
    result.summary.totalBands = 0;

    // Check skew on reference
    if (ref.gt[2] != 0 || ref.gt[4] != 0) {
        result.errors.push_back("Reference file has non-zero skew (rotation not supported)");
    }

    ref.bands = GDALGetRasterCount(hRef);

    if (GDALGetRasterCount(hRef) > 1) {
        result.warnings.push_back(displayName + " has " + std::to_string(GDALGetRasterCount(hRef)) + " bands");
    }
    return ref;
}

// Compares the grid of one file with the reference's: the pixel size and the origin
void checkGeoTransform(GDALDatasetH hDs, const ReferenceRaster &ref, const std::string &displayName,
                       MergeValidationResult &result) {
    double gt[6];
    if (GDALGetGeoTransform(hDs, gt) != CE_None) {
        result.warnings.push_back("Cannot read geotransform from: " + displayName);
        return;
    }

    // Skew check
    if (gt[2] != 0 || gt[4] != 0) {
        result.errors.push_back(displayName + " has non-zero skew");
    }

    // Pixel size tolerance (relative)
    double pxDiffX = std::abs(gt[1] - ref.gt[1]);
    double pxDiffY = std::abs(gt[5] - ref.gt[5]);
    double relDiffX = (std::abs(ref.gt[1]) > 0) ? pxDiffX / std::abs(ref.gt[1]) : 0;
    double relDiffY = (std::abs(ref.gt[5]) > 0) ? pxDiffY / std::abs(ref.gt[5]) : 0;

    if (relDiffX > 0.05 || relDiffY > 0.05) {
        result.errors.push_back("Pixel size mismatch > 5%: " + displayName);
    } else if (relDiffX > 0.01 || relDiffY > 0.01) {
        result.warnings.push_back("Resolution differs slightly for " + displayName + ", bands will be resampled");
    }

    // Origin tolerance (1 pixel)
    double origDiffX = std::abs(gt[0] - ref.gt[0]);
    double origDiffY = std::abs(gt[3] - ref.gt[3]);
    if (origDiffX > std::abs(ref.gt[1]) || origDiffY > std::abs(ref.gt[5])) {
        result.errors.push_back("Origin mismatch > 1 pixel: " + displayName);
    }
}

// Compares one more file with the reference; returns how many bands it adds
int checkAgainstReference(GDALDatasetH hDs, const ReferenceRaster &ref, const std::string &displayName,
                          MergeValidationResult &result) {
    // CRS check
    const char *proj = GDALGetProjectionRef(hDs);
    const SrsPtr srs(OSRNewSpatialReference(proj));
    if (!OSRIsSame(ref.srs.get(), srs.get())) {
        result.errors.push_back("CRS mismatch: " + displayName);
    }

    // Dimension check
    if (GDALGetRasterXSize(hDs) != ref.width || GDALGetRasterYSize(hDs) != ref.height) {
        result.errors.push_back("Dimension mismatch: " + displayName +
            " (" + std::to_string(GDALGetRasterXSize(hDs)) + "x" + std::to_string(GDALGetRasterYSize(hDs)) +
            " vs " + std::to_string(ref.width) + "x" + std::to_string(ref.height) + ")");
    }

    // Data type check
    GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hDs, 1));
    if (dt != ref.type) {
        result.errors.push_back("Data type mismatch: " + displayName +
            " (" + gdalTypeName(dt) + " vs " + gdalTypeName(ref.type) + ")");
    }

    checkGeoTransform(hDs, ref, displayName, result);

    if (GDALGetRasterCount(hDs) > 1) {
        result.warnings.push_back(displayName + " has " + std::to_string(GDALGetRasterCount(hDs)) + " bands");
    }

    return GDALGetRasterCount(hDs);
}

// True when the files are georeferenced already (a CRS and a geotransform that is not the identity
// GDAL reports for a file without one)
bool isGeoreferenced(const ReferenceRaster &ref) {
    return ref.hasGeoTransform && !ref.projection.empty() &&
           !(ref.gt[0] == 0 && ref.gt[1] == 1 && ref.gt[2] == 0 &&
             ref.gt[3] == 0 && ref.gt[4] == 0 && ref.gt[5] == -1);
}

// Whether the bands can be shifted into place: not when the files are georeferenced (the warp
// aligns them), when too few bands carry metadata, when the shift is negligible, or when the bands
// differ in size or include a thermal band of another resolution
bool correctionApplies(const std::vector<BandAlignmentInfo> &alignInfo, const ReferenceRaster &ref,
                       int detectedCount, double maxShift) {
    bool allSameDims = true;
    for (const auto &a : alignInfo) {
        if (a.imageWidth != ref.width || a.imageHeight != ref.height) {
            allSameDims = false;
            break;
        }
    }

    bool hasThermalDiffRes = false;
    for (const auto &a : alignInfo) {
        if (a.isThermal && (a.imageWidth != ref.width || a.imageHeight != ref.height)) {
            hasThermalDiffRes = true;
            break;
        }
    }

    return !isGeoreferenced(ref) &&
           detectedCount >= 2 &&
           maxShift > 0.5 &&
           allSameDims &&
           !hasThermalDiffRes;
}

// The warnings about the misalignment of the bands
void addAlignmentWarnings(MergeValidationResult &result, const std::vector<BandAlignmentInfo> &alignInfo,
                          int detectedCount, double maxShift) {
    if (maxShift > 2.0) {
        int shiftRounded = static_cast<int>(std::round(maxShift));
        if (result.alignment.correctionApplied) {
            result.warnings.push_back(
                "Band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Bands from multi-camera sensors (e.g. MicaSense, DJI Multispectral) "
                "are captured from slightly different viewpoints. For accurate vegetation indices "
                "(NDVI, NDRE, etc.), process raw imagery with a photogrammetry tool (e.g. WebODM, "
                "Pix4D) before merging. The merge will proceed with approximate pixel-shift correction.");
        } else {
            result.warnings.push_back(
                "Band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Bands from multi-camera sensors (e.g. MicaSense, DJI Multispectral) "
                "are captured from slightly different viewpoints. For accurate vegetation indices "
                "(NDVI, NDRE, etc.), process raw imagery with a photogrammetry tool (e.g. WebODM, "
                "Pix4D) before merging. Approximate pixel-shift correction is not being applied "
                "under the current input conditions.");
        }
    } else if (maxShift > 0.5) {
        int shiftRounded = static_cast<int>(std::round(maxShift));
        if (result.alignment.correctionApplied) {
            result.warnings.push_back(
                "Minor band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Approximate correction will be applied.");
        } else {
            result.warnings.push_back(
                "Minor band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Approximate correction is not being applied under the current input "
                "conditions.");
        }
    }

    // Partial metadata warning
    if (detectedCount > 0 && detectedCount < static_cast<int>(alignInfo.size())) {
        if (result.alignment.correctionApplied) {
            result.warnings.push_back(
                "Band alignment metadata found for " + std::to_string(detectedCount) + "/" +
                std::to_string(alignInfo.size()) + " bands. Correction will only be applied to "
                "bands with calibration data. Results may be inconsistent.");
        } else {
            result.warnings.push_back(
                "Band alignment metadata found for " + std::to_string(detectedCount) + "/" +
                std::to_string(alignInfo.size()) + " bands, but approximate correction is not being "
                "applied under the current input conditions. Results may be inconsistent.");
        }
    }

    // LWIR warning
    for (const auto &a : alignInfo) {
        if (a.isThermal) {
            result.warnings.push_back(
                "Thermal (LWIR) band detected. Thermal bands have different optics and cannot "
                "be aligned using PrincipalPoint shift. Consider excluding the thermal band from the merge.");
            break;
        }
    }
}

// Detects the alignment of the bands and records it, with its warnings, in result
void assessAlignment(const std::vector<std::string> &inputPaths, const ReferenceRaster &ref,
                     MergeValidationResult &result) {
    auto alignInfo = detectBandAlignment(inputPaths);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }

    if (detectedCount < 2) return;

    result.alignment.detected = true;

    double maxShift = 0;
    std::string source;
    for (const auto &a : alignInfo) {
        if (!a.detected) continue;
        double s = std::max(std::abs(a.shiftX), std::abs(a.shiftY));
        if (s > maxShift) maxShift = s;
        if (source.empty() && !a.shiftSource.empty()) source = a.shiftSource;
    }
    result.alignment.maxShiftPixels = maxShift;
    result.alignment.shiftSource = source;
    result.alignment.bands = alignInfo;

    result.alignment.correctionApplied = correctionApplies(alignInfo, ref, detectedCount, maxShift);

    addAlignmentWarnings(result, alignInfo, detectedCount, maxShift);
}

}  // namespace

MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths) {
    MergeValidationResult result;

    if (inputPaths.size() < 2) {
        result.errors.push_back("At least 2 files required");
        return result;
    }

    // Build display names (filename only) for cleaner messages
    std::vector<std::string> displayNames;
    displayNames.reserve(inputPaths.size());
    for (const auto &p : inputPaths) {
        displayNames.push_back(io::toUtf8(io::fromUtf8(p).filename()));
    }

    // Open first file as reference
    auto refDs = openUserRaster(inputPaths[0]);
    if (!refDs) {
        result.errors.push_back("Cannot open " + displayNames[0]);
        return result;
    }
    ReferenceRaster ref = checkReference(datasetHandle(refDs), displayNames[0], result);
    int totalBands = ref.bands;

    for (size_t i = 1; i < inputPaths.size(); i++) {
        const auto dataset = openUserRaster(inputPaths[i]);
        if (!dataset) {
            result.errors.push_back("Cannot open " + displayNames[i]);
            continue;
        }
        totalBands += checkAgainstReference(datasetHandle(dataset), ref, displayNames[i], result);
    }

    refDs.reset();

    result.summary.totalBands = totalBands;
    result.summary.estimatedSize = static_cast<size_t>(ref.width) * ref.height * totalBands * GDALGetDataTypeSizeBytes(ref.type);
    result.ok = result.errors.empty();

    assessAlignment(inputPaths, ref, result);

    return result;
}

}  // namespace ddb
