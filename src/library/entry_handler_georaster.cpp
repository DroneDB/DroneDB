/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GeoRaster entries: georeferenced rasters, built into a Cloud Optimized GeoTIFF. The
// Image handler detects them (a TIFF or JPEG 2000 with a projection), entry_handler_image.cpp.

#include <memory>

#include "build_layout.h"
#include "json.h"
#include "cog.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "logger.h"
#include "mio.h"
#include "raster_ops.h"

namespace ddb {

namespace {

// Size, geotransform, projection, WGS84 footprint and bands: what GDAL finds in the file
// (raster_ops.h), put in the entry
void parseGeoRasterMetadata(const fs::path &path, Entry &entry)
{
    LOGV << "Processing GeoRaster file: " << io::toUtf8(path);
    raster::applyProbe(raster::probe(path, raster::ProbeParams()), entry);
    LOGV << "GeoRaster processing completed successfully";
}

class GeoRasterEntryHandler : public EntryHandler {
  public:
    GeoRasterEntryHandler() : EntryHandler(EntryType::GeoRaster, "GeoRaster") {}

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseGeoRasterMetadata(path, entry);
    }

    std::string subfolder() const override { return CogBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("raster.cog", artifactPath(CogBuildSubfolder, CogFileName),
                                    "image/tiff; application=geotiff; profile=cloud-optimized", CogFileName)};
    }

    void build(const BuildContext& ctx) const override {
        buildCog(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder / CogFileName), ctx.options);
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::File; }

    bool tilesWithoutReprojection() const override { return true; }
};

}  // namespace

std::unique_ptr<EntryHandler> makeGeoRasterEntryHandler() {
    return std::make_unique<GeoRasterEntryHandler>();
}

}  // namespace ddb
