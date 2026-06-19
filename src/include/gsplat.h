/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GSPLAT_H
#define GSPLAT_H

#include <array>
#include <string>
#include <vector>

#include "basicgeometry.h"
#include "ddb_export.h"
#include "fs.h"
#include "json.h"

namespace ddb
{
    // Standard layout of the Gaussian Splat build artifact within an entry's build folder.
    // Mirrors the COPC/COG/NXS conventions (see pointcloud.h).
    constexpr const char *GsplatBuildSubfolder = "gsplat";
    constexpr const char *GsplatFileName = "model.spz";
    constexpr const char *GsplatGeorefFileName = "georef.json";
    // Level-of-detail delivery artifact (Spark "RAD" container). Produced by the vendored
    // build-lod tool; streamed progressively by the viewer via HTTP range requests (paged: true).
    // This is the sole delivery artifact - the intermediate .spz is not kept after a successful build.
    constexpr const char *GsplatRadFileName = "model.rad";

    // Tiny sidecar with the splat's local-space axis-aligned bounding box, written at build
    // time. The viewer uses it to frame the camera deterministically. This is essential for the
    // paged .rad path: a streamed mesh has no resident splats (hence no computable bounds) when
    // it first initializes, so client-side auto-framing cannot work without this hint.
    constexpr const char *GsplatBoundsFileName = "bounds.json";

    // Thumbnail filename constant (kept for reference in test assertions).
    // Thumbnails are generated on demand from the .rad/.spz at request time - no build-time
    // thumbnail artifact is produced. The constant is used to verify that buildGsplat does
    // not write a thumbnail file alongside the delivery artifact.
    constexpr const char *GsplatThumbFileName = "thumbnail.webp";

    constexpr int GsplatThumbSize = 2048; // pixels, square

    // Web LOD delivery profile (see TBD/GaussianSplats/LOD-Support.md): bhatt-lod ("quality")
    // with full spherical harmonics (degree 3) so the streamed .rad preserves the look of the
    // source and can be served as the SOLE delivery artifact (the .spz is dropped after build).
    // Single-file RAD so Registry can serve it with HTTP range requests.
    constexpr int GsplatRadMaxSh = 3;

    // Standard spherical-harmonics DC -> RGB factor (Y_0^0 normalization constant).
    constexpr double SH_C0 = 0.2820947917738781;

    // Lightweight metadata about a Gaussian Splat source or artifact. Populated from the
    // file header where possible (no full decode), so it stays cheap during indexing.
    struct GaussianSplatInfo
    {
        size_t splatCount = 0;
        int shDegree = 0;
        // PLY property names, when the source is a .ply (empty for other formats).
        std::vector<std::string> dimensions;

        json toJSON() const;
    };

    // Georeferencing for a Gaussian Splat, following the ODX/ODM coordinates-file
    // convention: the splat geometry is stored in LOCAL coordinates, and the true
    // projected coordinate of any point is (local + offset), interpreted in `srs`.
    struct GsplatGeoref
    {
        // SRS header in any PROJ-understandable form: an ODM header ("WGS84 UTM 17N"),
        // an authority code ("EPSG:32617"), a proj4 string or WKT.
        std::string srs;
        // East, North, Z offset added to local coordinates to obtain projected coordinates.
        std::array<double, 3> offset = {0.0, 0.0, 0.0};
        // Optional [west, south, east, north] WGS84 footprint (convenience for consumers).
        std::vector<double> boundsWgs84;
        bool valid = false;

        json toJSON() const;
    };

    // True when the path ends with the canonical .spz delivery extension.
    DDB_DLL bool isSpzPath(const std::string &filename);

    // True when the path has any Gaussian Splat source extension we recognize
    // (.spz, .splat, .ksplat). Note: a .ply splat is recognized via identifyPly().
    DDB_DLL bool hasSplatExtension(const std::string &filename);

    // Content sanity checks used by detection. They never throw and tolerate I/O errors.
    DDB_DLL bool looksLikeSpz(const fs::path &path);         // NGSP magic (v4) or gzip magic (legacy)
    DDB_DLL bool looksLikeSplatBinary(const fs::path &path); // size > 0 and multiple of 32 bytes

    // Reads lightweight metadata (splat count + SH degree) from a splat source
    // (.ply | .splat | .spz). Returns false if the file cannot be interpreted.
    DDB_DLL bool getGaussianSplatInfo(const std::string &filename, GaussianSplatInfo &info);

    // Parses an ODX/ODM coordinates file: first line is the SRS header, second
    // non-empty/non-comment line is the "east north [z]" offset. Returns false when
    // the file is missing or malformed.
    DDB_DLL bool parseCoordsFile(const fs::path &coordsFile, GsplatGeoref &georef);

    // Writes the georef.json sidecar described by `georef`.
    DDB_DLL void writeGeoref(const GsplatGeoref &georef, const fs::path &outFile);

    // Converts a splat source (.ply | .splat | .spz) into a single .spz file.
    // Throws AppException on failure and BuildDepMissingException for formats that
    // require an external tool not yet available (e.g. .ksplat).
    DDB_DLL void convertToSpz(const std::string &input, const std::string &output);

    // Builds the Gaussian Splat delivery artifacts from a splat source into `outdir`.
    // Requires the build-lod tool; throws BuildDepMissingException if unavailable.
    // Produces `<outdir>/model.rad` (RAD container) and `<outdir>/bounds.json` (bounding box).
    // If `georef.valid`, also writes `<outdir>/georef.json`.
    // The intermediate .spz is not kept after a successful build.
    DDB_DLL void buildGsplat(const std::string &input, const std::string &outdir);
    DDB_DLL void buildGsplat(const std::string &input, const std::string &outdir,
                             const GsplatGeoref &georef);

} // namespace ddb

#endif // GSPLAT_H
