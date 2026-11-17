/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_BUILD_POLICY_H
#define COG_BUILD_POLICY_H

#include <array>
#include <optional>
#include <string>

#include "basicgeometry.h"
#include "fs.h"
#include "json_fwd.h"

namespace ddb {

// What makes the pixels of a raster transparent, as GDAL reports it: the facts that decide whether
// the COG of a GeoRaster built by an older release differs from the one this release builds.
struct RasterTransparency {
    bool palette = false;  // band 1 has a colour table (colour interpretation "Palette")
    bool alpha = false;    // a band is an alpha band (colour interpretation "Alpha")
    bool mask = false;     // a mask of its own: an internal or external mask, not its alpha band nor its nodata
    int noDataBands = 0;   // the bands other than an alpha band that have a nodata value
};

// What the header of a COG says (no pixel is read): what makes its pixels transparent, and where
// its grid lies.
struct BuiltCog {
    RasterTransparency transparency;
    std::string wkt;                 // its CRS
    std::array<double, 4> extent{};  // minX, minY, maxX, maxY of its grid, in its CRS
    double pixelSize = 0;            // of its full resolution (the larger side of a pixel)
};

// The rules of the COG build of a GeoRaster that changed what it writes. A COG built before them
// differs from the one built now only for some sources, and only those are built again:
//   - a source without nodata, mask or alpha band: the pixels the warp adds around it (up to the
//     tiles of the grid, the corners of a rotated raster) were opaque 0 and counted in the
//     statistics of the bands; they are now masked;
//   - a source with the nodata of several bands and no alpha band: a pixel was transparent when its
//     first band was at nodata; it is now transparent only when every band is, through a mask;
//   - a source with a colour table: its indices were warped bilinearly, averaging unrelated
//     colours; they are now warped by nearest neighbour.
// Sources with an alpha band, a mask or the nodata of one band (an RGBA orthophoto, a DEM with
// nodata) get the same COG, unless it lies elsewhere than the raster (misplaces()): the CRS the
// builds before derived from some GeoTIFFs was wrong (a prime meridian in grads, as NTF Paris; an
// EPSG CRS whose linear unit the file changes to feet or US feet; an IMAGINE citation that names a
// unit), which put their COG up to thousands of kilometres away, and the COG of a raster around a
// pole stopped at the latitude of its edges.
class CogBuildPolicy {
  public:
    // The version of the rules. A COG built by this release records it (CogBuildRecord); a COG
    // without a record, or with an older version, was built before them.
    static constexpr int kVersion = 1;

    // Whether the COG a build before kVersion made of a source with these facts differs from the
    // one built now
    static bool outdates(const RasterTransparency& source);

    // The facts the index holds of the source of a GeoRaster entry (its `properties`), when they
    // decide outdates(): the colour interpretations of the bands, which entries have always had,
    // decide for a palette or an alpha band; the nodata of the bands and `hasMask`, which the probe
    // records since kVersion, decide the others. Nothing for an entry indexed before them whose
    // bands are neither.
    static std::optional<RasterTransparency> indexed(const json& properties);

    // The header of a COG that a build before kVersion made (no pixel is read): where it lies,
    // for misplaces(), and its transparency, for an entry whose index does not decide (indexed()):
    // such a build kept the nodata of the source, turned its mask into an alpha band or a mask and
    // kept its alpha, so the COG shows what its source had. (A colour table, which some builds
    // dropped beside an alpha band or a mask, is in the index of every entry.)
    // @throws AppException when the COG cannot be read or has no geotransform
    static BuiltCog ofBuiltCog(const fs::path& cog);

    // Whether a COG a build before kVersion made lies elsewhere than its raster: a corner of the
    // footprint of the raster (the index's polygon in EPSG:4326, which the probe computes with
    // GDAL) is outside the grid of the COG by more than kPlacementTolerancePixels of its pixels.
    // The grid of a build covers the whole raster and more (it is padded to whole tiles), so a COG
    // in the right place holds every corner. A latitude past the limit of Web Mercator, the CRS of
    // every grid a build writes, is taken at that limit (a raster beyond it is cut there).
    // Nothing to compare without a footprint: not misplaced.
    // @throws AppException when the footprint cannot be transformed into the CRS of the COG
    static bool misplaces(const BuiltCog& cog, const BasicPolygonGeometry& footprint);

    // The rounding misplaces() allows, in pixels of the COG: far below the distance a wrong CRS
    // moves a raster (tens of kilometres at least)
    static constexpr double kPlacementTolerancePixels = 4.0;
};

// Beside the COG of a build (CogBuildRecordFileName, build_layout.h): {"version"}, the version of
// CogBuildPolicy it was built under.
struct CogBuildRecord {
    int version = CogBuildPolicy::kVersion;
};

// @throws FSException when the file cannot be written
void writeCogBuildRecord(const fs::path& file, const CogBuildRecord& record);
// Nothing when the file does not exist or is not a build record
std::optional<CogBuildRecord> readCogBuildRecord(const fs::path& file);

}  // namespace ddb

#endif  // COG_BUILD_POLICY_H
