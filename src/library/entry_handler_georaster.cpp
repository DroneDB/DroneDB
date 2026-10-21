/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GeoRaster entries: georeferenced rasters, built into a Cloud Optimized GeoTIFF.

#include "build_layout.h"
#include "cog.h"
#include "entry.h"
#include "entry_handlers_builtin.h"

namespace ddb {

namespace {

class GeoRasterEntryHandler : public EntryHandler {
  public:
    GeoRasterEntryHandler() : EntryHandler(EntryType::GeoRaster, "GeoRaster") {}

    std::string subfolder() const override { return CogBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"raster.cog", artifactPath(CogBuildSubfolder, CogFileName), ArtifactKind::File,
                 "image/tiff; application=geotiff; profile=cloud-optimized", CogFileName, "", false}};
    }

    void build(const BuildContext& ctx) const override {
        buildCog(ctx.inputPath.string(), (ctx.tempFolder / CogFileName).string());
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::File; }

    bool tilesWithoutReprojection() const override { return true; }
};

}  // namespace

std::unique_ptr<EntryHandler> makeGeoRasterEntryHandler() {
    return std::make_unique<GeoRasterEntryHandler>();
}

}  // namespace ddb
