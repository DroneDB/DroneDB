/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GSPLAT_H
#define GSPLAT_H

#include <cstdint>
#include <optional>
#include <string>

#include "build_layout.h"
#include "json_fwd.h"
#include "local_frame_georef.h"

namespace ddb
{
    // The Gaussian Splat artifact names (GsplatBuildSubfolder, GsplatRadFileName,
    // GsplatBoundsFileName, ...) are in build_layout.h; the formats a splat can be in, in
    // splat_format.h.

    // The delivery profile: bhatt-lod ("quality") with full spherical harmonics (degree 3), so the
    // streamed .rad keeps the look of the source and is the sole delivery artifact (build-lod
    // reads the source directly; no .spz is made). Single-file RAD so Registry can serve it with
    // HTTP range requests.
    constexpr int GsplatRadMaxSh = 3;

    // Lightweight metadata about a Gaussian Splat source. Populated from the file header (no full
    // decode), so it stays cheap during indexing.
    struct GaussianSplatInfo
    {
        uint64_t splatCount = 0;
        int shDegree = 0;
    };

    /// The properties of a Gaussian splat entry: {"splatCount", "shDegree"}
    void to_json(json &j, const GaussianSplatInfo &info);

    // Reads the splat count and SH degree of a splat source from its header. Returns false when
    // the format has no metadata reader (SplatStage::Metadata in splat_format.h: .sog and .rad
    // have none, so their entries carry no splatCount) or the file cannot be interpreted.
    bool getGaussianSplatInfo(const std::string &filename, GaussianSplatInfo &info);

    // Builds the Gaussian Splat delivery artifacts from a splat source (a format SplatStage::Build
    // names) into `outdir`: `<outdir>/model.rad` (RAD container) and `<outdir>/bounds.json`
    // (bounding box and up vector). build-lod is mandatory: throws AppException if it is unavailable.
    //
    // bounds.json is {"min": [x, y, z], "max": [x, y, z], "up": [x, y, z], "upConfidence", "upSource"}
    // in the frame of model.rad; "up" is the unit vector to the sky and may be missing. When the
    // splat is `georef`erenced its frame is metric East/North/Up, so up is [0, 0, 1] with
    // upConfidence 1 and upSource "georef" without any estimate, and "wgs84Footprint" is the GeoJSON
    // Polygon geometry of the bounds on the Earth ({"type": "Polygon", "coordinates": [[[lon, lat], ...]]}).
    // A .rad source is already a model.rad: it is validated with the RAD reader and linked or copied,
    // without build-lod (a file the reader cannot read throws AppException with the reason).
    void buildGsplat(const std::string &input, const std::string &outdir,
                     const std::optional<LocalFrameGeoref> &georef = std::nullopt);

} // namespace ddb

#endif // GSPLAT_H
