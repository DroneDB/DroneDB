/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Vector entries: GeoJSON, Shapefile, GeoPackage, KML, FlatGeobuf, CAD drawings (DXF/DWG), ...
// built into a GeoPackage copy plus an MVT pyramid (or the drawing-coordinate metadata of a CAD
// drawing without CRS).

#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "vector.h"

namespace ddb {

namespace {

class VectorEntryHandler : public EntryHandler {
  public:
    VectorEntryHandler() : EntryHandler(EntryType::Vector, "Vector") {}

    std::string subfolder() const override { return VectorBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"vector.gpkg", artifactPath(VectorBuildSubfolder, VectorGpkgFileName), ArtifactKind::File,
                 "application/geopackage+sqlite3", VectorGpkgFileName, "", false},
                {"vector.mvt", MvtBuildSubfolder, ArtifactKind::Directory, "application/vnd.mapbox-vector-tile",
                 "", MvtMetadataFileName, false},
                {"vector.local", artifactPath(VectorBuildSubfolder, VectorLocalFileName), ArtifactKind::File,
                 "application/json", "", "", false}};
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
        buildVector(ctx.inputPath.string(), ctx.baseOutputPath.string(), ctx.force);
    }

    // The .dbf/.shx/.cpg of a shapefile; a .prj also gives a CAD drawing its CRS (DDB-12)
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
