/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_BUILD_POLICY_H
#define COG_BUILD_POLICY_H

#include <optional>

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
// nodata) get the same COG.
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

    // The facts of a COG that a build before kVersion made, read from its header (no pixel is
    // read), for an entry whose index does not decide (indexed()): such a build kept the nodata of
    // the source, turned its mask into an alpha band or a mask and kept its alpha, so the COG shows
    // what its source had. (A colour table, which some builds dropped beside an alpha band or a
    // mask, is in the index of every entry.) @throws AppException when the COG cannot be read
    static RasterTransparency ofBuiltCog(const fs::path& cog);
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
