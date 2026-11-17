/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GeoRaster entries: georeferenced rasters, built into a Cloud Optimized GeoTIFF. The
// Image handler detects them (a TIFF or JPEG 2000 with a projection), entry_handler_image.cpp.

#include <memory>

#include "build_layout.h"
#include "json.h"
#include "cog.h"
#include "cog_build_policy.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "raster_helper.h"

namespace ddb {

namespace {

// Size, geotransform, projection, WGS84 footprint and bands: what GDAL finds in the file, as the
// raster helper tells it (raster_helper.h), put in the entry. The detection of the Image handler
// asked already: its answer is in the context.
void parseGeoRasterMetadata(const fs::path &path, Entry &entry, FingerprintContext &ctx)
{
    LOGV << "Processing GeoRaster file: " << io::toUtf8(path);
    const raster::ProbeOutcome &outcome = raster::probeFile(ctx, path);
    if (outcome.result)
        raster::applyProbe(*outcome.result, entry);
    else if (outcome.notRead)
        setEntryWarning(entry, raster::notReadWarning(*outcome.notRead));
    else
        throw raster::RasterOpException(raster::ErrorCode::Unreadable, "Cannot open " + io::toUtf8(path) + " for reading");
    LOGV << "GeoRaster processing completed successfully";
}

class GeoRasterEntryHandler : public EntryHandler {
  public:
    GeoRasterEntryHandler() : EntryHandler(EntryType::GeoRaster, "GeoRaster") {}

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& ctx) const override {
        parseGeoRasterMetadata(path, entry, ctx);
    }

    std::string subfolder() const override { return CogBuildSubfolder; }

    // isComplete() reads the bands of the raster and whether it has a mask, and its footprint
    std::vector<std::string> buildPropertyKeys() const override { return {"bands", "hasMask"}; }
    bool buildReadsFootprint() const override { return true; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("raster.cog", artifactPath(CogBuildSubfolder, CogFileName),
                                    "image/tiff; application=geotiff; profile=cloud-optimized", CogFileName)};
    }

    // The COG folder has its output (the default rule). A COG built before the rules of
    // CogBuildPolicy (no build record, or one of an older version) is incomplete when the COG built
    // now differs from it, so that it is built again; the one it has is served meanwhile. The
    // index decides the transparency of most entries; for one indexed before it held the nodata
    // and the mask of the raster, the header of the COG does. A COG the facts find healthy is read
    // (its header) to check that it lies where the raster is.
    bool isComplete(const Entry& e, const fs::path& baseOutputPath, bool reportRecoverable) const override {
        if (!EntryHandler::isComplete(e, baseOutputPath, reportRecoverable))
            return false;
        const fs::path folder = baseOutputPath / CogBuildSubfolder;
        const auto record = readCogBuildRecord(folder / CogBuildRecordFileName);
        if (record && record->version >= CogBuildPolicy::kVersion)
            return true;
        const auto indexed = CogBuildPolicy::indexed(e.properties);
        if (indexed && CogBuildPolicy::outdates(*indexed))
            return false;
        try {
            const BuiltCog cog = CogBuildPolicy::ofBuiltCog(folder / CogFileName);
            if (!indexed && CogBuildPolicy::outdates(cog.transparency))
                return false;
            if (CogBuildPolicy::misplaces(cog, e.polygon_geom)) {
                LOGD << "The COG of " << e.path << " is built again: it lies elsewhere than the raster";
                return false;
            }
            return true;
        } catch (const AppException& ex) {
            LOGD << "The COG of " << e.path << " is built again: " << ex.what();
            return false;
        }
    }

    void build(const BuildContext& ctx) const override {
        buildCog(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder / CogFileName), ctx.options);
        writeCogBuildRecord(ctx.tempFolder / CogBuildRecordFileName, CogBuildRecord{});
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::File; }

    bool tilesWithoutReprojection() const override { return true; }
};

}  // namespace

std::unique_ptr<EntryHandler> makeGeoRasterEntryHandler() {
    return std::make_unique<GeoRasterEntryHandler>();
}

}  // namespace ddb
