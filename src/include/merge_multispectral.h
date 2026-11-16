/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MERGE_MULTISPECTRAL_H
#define MERGE_MULTISPECTRAL_H

#include "raster_protocol.h"
#include <string>
#include <vector>
#include <cstdint>

namespace ddb {

struct BandAlignmentInfo {
    bool detected = false;
    std::string bandName;
    double shiftX = 0;
    double shiftY = 0;
    std::string shiftSource;
    bool isThermal = false;
    int imageWidth = 0;
    int imageHeight = 0;
    int centralWavelength = 0;
};

struct MergeValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    struct {
        std::string crs;
        // The size of the merged raster: the area that every band covers once the alignment
        // correction applies (alignment.correctionApplied), the size of the bands otherwise
        int width = 0;
        int height = 0;
        // The size of the bands (the first band)
        int sourceWidth = 0;
        int sourceHeight = 0;
        double pixelSizeX = 0;
        double pixelSizeY = 0;
        std::string dataType;
        int totalBands = 0;
        size_t estimatedSize = 0;
    } summary;

    struct AlignmentStatus {
        bool detected = false;
        double maxShiftPixels = 0;
        bool correctionApplied = false;
        std::string shiftSource;
        std::vector<BandAlignmentInfo> bands;
    } alignment;
};

std::vector<BandAlignmentInfo> detectBandAlignment(const std::vector<std::string> &inputPaths);

MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths);
/// The same from the shapes of the files, which the raster helper read (raster::rasterShapesOf), one per file
MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths,
                                                 const std::vector<raster::RasterShape> &shapes);

/// The validation as JSON (DDBValidateMergeMultispectral): ok, errors, warnings, summary
/// {totalBands, dataType, width, height, crs, pixelSizeX, pixelSizeY, estimatedSize} and
/// alignment {detected, maxShiftPixels, correctionApplied, shiftSource, bands: [{index, name,
/// wavelength, shiftX, shiftY}]}. width and height are the size of the
/// merged raster (smaller than the bands when the alignment correction crops them); bands names
/// every band that the camera metadata names, whether a shift was detected or not.
std::string mergeValidationToJson(const MergeValidationResult &r);

void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                        const std::vector<int> &previewBands,
                                        int thumbSize,
                                        uint8_t **outBuffer, int *outBufferSize);

/**
 * @brief Merges single-band rasters into one multi-band GeoTIFF at outputPath.
 *
 * Before it reads anything: an output that is one of the bands is refused
 * (InvalidArgsException), and an output that exists unless overwrite (OutputExistsException).
 * The merge and its GPS position are written in a private folder beside outputPath
 * (raster::makePrivateFolder) and replace an existing output in one rename once complete: a
 * failed run leaves the existing output as it was.
 */
void mergeMultispectral(const std::vector<std::string> &inputPaths,
                                 const std::string &outputPath,
                                 bool overwrite = false);

namespace raster {
/// Writes `out`, the bands of `params` merged (MergeBandsParams): what the raster helper runs for a
/// `mergeBands` request. Returns {"cropped": whether the bands were shifted into place}. Only where users'
/// files are opened (the helper). @throws GDALException
json mergeBandsFile(const MergeBandsParams &params, const fs::path &out);
/// Writes `out`, the WebP preview of the merge of `params` (MergePreviewParams): what the raster helper
/// runs for a `mergePreview` request. Only where users' files are opened. @throws GDALException
void mergePreviewFile(const MergePreviewParams &params, const fs::path &out);
}  // namespace raster


} // namespace ddb

#endif // MERGE_MULTISPECTRAL_H
