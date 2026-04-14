/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MERGE_MULTISPECTRAL_H
#define MERGE_MULTISPECTRAL_H

#include <string>
#include <vector>
#include <cstdint>
#include "ddb_export.h"

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
        int width = 0;
        int height = 0;
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

DDB_DLL std::vector<BandAlignmentInfo> detectBandAlignment(const std::vector<std::string> &inputPaths);

DDB_DLL MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths);

DDB_DLL void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                        const std::vector<int> &previewBands,
                                        int thumbSize,
                                        uint8_t **outBuffer, int *outBufferSize);

DDB_DLL void mergeMultispectral(const std::vector<std::string> &inputPaths,
                                 const std::string &outputCog);

} // namespace ddb

#endif // MERGE_MULTISPECTRAL_H
