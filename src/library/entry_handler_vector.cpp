/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Vector entries: GeoJSON, Shapefile, GeoPackage, KML, GPX, FlatGeobuf, CAD drawings (DXF/DWG), ...
// built into a GeoPackage copy plus an MVT pyramid (or the drawing-coordinate metadata of a CAD
// drawing without CRS).

#include <optional>

#include "build_layout.h"
#include "json.h"
#include "cad.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "raster_helper.h"
#include "vector.h"

namespace ddb {

namespace {

// Layers, extent and footprint in WGS84 (and the coordinate system of a CAD drawing), found by
// GDAL in the raster helper (raster_helper.h) and put in the entry.
// Best-effort: any failure leaves the entry indexed without geometry but
// never aborts the parse (a corrupted vector file should not block ddb add). What does stop
// the batch is a helper that cannot be used, a full disk and a canceled run.
void parseVectorMetadata(const fs::path &path, Entry &entry, FingerprintContext &ctx)
{
    // Declared outside try: the CAD source outlives the dataset (a converted DXF is
    // deleted only once closed).
    std::optional<cad::CadSource> cadSource;
    try
    {
        // CAD drawings: a DWG is read through a temporary DXF, and the CRS
        // comes from the drawing (the same rule the build uses, cad.h).
        const bool isCad = cad::isCadFile(path);
        if (isCad)
            cadSource.emplace(path);
        const fs::path openPath = isCad ? cadSource->dxfPath() : path;

        raster::ProbeParams params;
        params.kind = raster::ProbeKind::Vector;
        if (isCad)
        {
            params.source = path;
            params.converted = cadSource->converted();
            // The helper cannot see which .prj this thread ignores (cad.h IgnoredSidecarScope)
            params.sidecarDecided = true;
            params.sidecar = cad::findPrjSidecar(path).value_or(fs::path());
        }

        const raster::ProbeOutcome &outcome = raster::probeFile(ctx, openPath, params);
        if (outcome.result)
            raster::applyProbe(*outcome.result, entry);
        else if (outcome.notRead)
            setEntryWarning(entry, raster::notReadWarning(*outcome.notRead));
        else
            LOGD << "Vector parse: GDAL cannot open " << io::toUtf8(path);
    }
    catch (const HelperUnavailableException &)
    {
        throw;
    }
    catch (const CanceledException &)
    {
        throw;
    }
    catch (const std::exception &e)
    {
        if (raster::classify(e) == raster::ErrorCode::NoSpace)
            throw;
        LOGD << "Vector parse warning: " << e.what();
        setEntryWarning(entry, std::string("vector parse failed: ") + e.what());
    }
}

class VectorEntryHandler : public EntryHandler {
  public:
    VectorEntryHandler() : EntryHandler(EntryType::Vector, "Vector") {}

    std::vector<std::string> extensions() const override {
        return {"geojson", "dxf", "dwg", "shp", "shz", "fgb", "topojson", "kml", "kmz", "gpkg", "gpx"};
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& ctx) const override {
        parseVectorMetadata(path, entry, ctx);
    }

    std::string subfolder() const override { return VectorBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("vector.gpkg", artifactPath(VectorBuildSubfolder, VectorGpkgFileName),
                                    "application/geopackage+sqlite3", VectorGpkgFileName),
                // The media type of a directory artifact is its index's: the TileJSON (the tiles
                // themselves, "<z>/<x>/<y>.pbf", are application/vnd.mapbox-vector-tile)
                BuildArtifact::directory("vector.mvt", MvtBuildSubfolder, "application/json",
                                         MvtMetadataFileName),
                BuildArtifact::file("vector.local", artifactPath(VectorBuildSubfolder, VectorLocalFileName),
                                    "application/json")};
    }

    // buildVector manages its own atomic write to baseOutputPath/vec and baseOutputPath/mvt
    bool usesTempFolder() const override { return false; }

    // vec/source.gpkg AND mvt/metadata.json both exist and are not empty: two canonical
    // sentinel files rather than a directory scan, since both artifacts are always
    // co-produced and uniquely identify a complete build. A local CAD drawing (no CRS) has no
    // MVT: vec/local.json replaces mvt/metadata.json as the second sentinel.
    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorGpkgFileName) &&
               (fileExistsAndNonEmpty(baseOutputPath / MvtBuildSubfolder / MvtMetadataFileName) ||
                fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorLocalFileName));
    }

    void build(const BuildContext& ctx) const override {
        // Propagate the force flag so a partial build (e.g. vec/ present but mvt/ missing) is
        // fully rebuilt instead of being silently treated as up-to-date inside buildVector.
        buildVector(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.baseOutputPath), ctx.force);
    }

    // The .dbf/.shx/.cpg of a shapefile; a .prj also gives a CAD drawing its CRS
    std::vector<SidecarRule> sidecarRules() const override {
        return {{{"shp"}, {"cpg", "dbf", "shx", "prj"}},
                {{"dxf", "dwg"}, {"prj"}}};
    }
};

}  // namespace

std::unique_ptr<EntryHandler> makeVectorEntryHandler() {
    return std::make_unique<VectorEntryHandler>();
}

}  // namespace ddb
