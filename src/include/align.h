/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ALIGN_H
#define ALIGN_H

#include <string>
#include <vector>

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
    int       minInliers             = 8;     // tie points below this: translation; inliers below it: failure
    double    minInlierRatio         = 0.25;  // inliers over tie points below this: failure
    bool      usePhaseCorrelationSeed = true; // coarse seed before NCC
    // The checks of a solution; one that fails them is an error, not an aligned raster
    double    minCorrelation         = 0.03;  // NCC of the relief of the aligned source and the reference
    double    minCorrelationOverChance = 3.0; // that NCC over the NCC of the source moved off the solution
    double    minOverlap             = 0.25;  // their samples with data on each other, over those of the smaller
    double    maxScaleChange         = 0.2;   // |scale - 1| of a similarity
    double    maxRotationDeg         = 15.0;  // |rotation| of a similarity
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
    // The shift of the centre of the area both rasters share (pivotX, pivotY), in map units of
    // the source CRS. The transform is X_ref = pivot + scale·R(theta)·(X_src - pivot) + (tx, ty).
    double txMapUnits   = 0.0;
    double tyMapUnits   = 0.0;
    double pivotX       = 0.0;
    double pivotY       = 0.0;
    double thetaDeg     = 0.0;   // rotation in degrees (0 for Translation)
    double scale        = 1.0;   // uniform scale (1.0 for Translation)
    double shiftZ       = 0.0;   // vertical offset (map units, DEM only)
    int    inlierCount  = 0;
    double inlierRatio  = 0.0;
    double rmseMapUnits = 0.0;
    double confidence   = 0.0;   // [0,1] composite
    double correlation  = 0.0;   // NCC of the relief of the aligned source and the reference
    double overlap      = 0.0;   // their samples with data on each other, over those of the smaller
    std::string mode;
    std::string warningMessage;
};

// ─── Public API ───────────────────────────────────────────────────────────────

/** Validate that the two rasters are compatible for alignment. */
AlignValidationResult validateAlignRaster(
    const std::string &sourcePath,
    const std::string &referencePath);

/** Align sourcePath to referencePath and write outputPath (COG).
 *  The output CRS is the one produced by buildCog (Web Mercator).
 *  Similarity falls back to Translation when the source has too few tie points.
 *  Throws AppException, and writes nothing, when either raster has nothing to match or the
 *  solution fails the checks of AlignOptions (the aligned source does not lie on the reference). */
AlignResult alignRaster(
    const std::string &sourcePath,
    const std::string &referencePath,
    const std::string &outputPath,
    const AlignOptions &opts = {});

/** Adds zOffset to every sample of band 1 of the DEM at path, in place (nodata stays as it is);
 *  an offset below 1e-6 leaves the file alone. */
void applyZOffset(const std::string &path, double zOffset);

std::string alignResultToJson(const AlignResult &r);
std::string alignValidationToJson(const AlignValidationResult &r);

}  // namespace ddb

#endif  // ALIGN_H
