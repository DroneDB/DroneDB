/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ALIGN_H
#define ALIGN_H

#include <string>
#include <vector>
#include "ddb_export.h"

namespace ddb {

// ─── Public types ─────────────────────────────────────────────────────────────

enum class AlignMode {
    Similarity,   // 4-DOF: tx, ty, rotation, uniform scale (default)
    Translation   // 2-DOF: tx, ty only via phase correlation
};

struct AlignOptions {
    AlignMode mode                   = AlignMode::Similarity;
    int       patchSize              = 64;    // NCC template size (pixels)
    int       searchRadius           = 128;   // NCC search half-window (pixels, common grid)
    int       maxPatches             = 400;   // max candidate patches
    int       ransacIterations       = 1000;
    double    ransacThreshold        = 2.0;   // inlier threshold (map units)
    int       minInliers             = 8;     // fallback to translation below this
    double    minInlierRatio         = 0.25;
    bool      usePhaseCorrelationSeed = true; // coarse seed before NCC
};

struct AlignValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    struct Summary {
        std::string sourceCrs;
        std::string referenceCrs;
        bool        crsMismatch      = false;
        double      overlapPercent   = 0.0;
        std::string sourceType;     // "ortho" | "dem"
        std::string referenceType;
        double      sourceGsdM      = 0.0;
        double      referenceGsdM   = 0.0;
    } summary;
};

struct AlignResult {
    bool   success      = false;
    double txMapUnits   = 0.0;   // X translation in map units (source CRS)
    double tyMapUnits   = 0.0;   // Y translation in map units
    double thetaDeg     = 0.0;   // rotation in degrees (0 for Translation)
    double scale        = 1.0;   // uniform scale (1.0 for Translation)
    double shiftZ       = 0.0;   // vertical offset (map units, DEM only)
    int    inlierCount  = 0;
    double inlierRatio  = 0.0;
    double rmseMapUnits = 0.0;
    double confidence   = 0.0;   // [0,1] composite
    std::string mode;
    std::string warningMessage;
};

// ─── Public API ───────────────────────────────────────────────────────────────

/** Validate that the two rasters are compatible for alignment. */
DDB_DLL AlignValidationResult validateAlignRaster(
    const std::string &sourcePath,
    const std::string &referencePath);

/** Align sourcePath to referencePath and write outputPath (COG).
 *  The output CRS is the one produced by buildCog (Web Mercator).
 *  Falls back to Translation when there are not enough inliers. */
DDB_DLL AlignResult alignRaster(
    const std::string &sourcePath,
    const std::string &referencePath,
    const std::string &outputPath,
    const AlignOptions &opts = {});

DDB_DLL std::string alignResultToJson(const AlignResult &r);
DDB_DLL std::string alignValidationToJson(const AlignValidationResult &r);

}  // namespace ddb

#endif  // ALIGN_H
