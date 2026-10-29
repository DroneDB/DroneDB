/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Model entries: meshes (OBJ, PLY, STL, glTF), built into bm3d plus an additive OGC 3D
// Tiles tileset.

#include <algorithm>
#include <cmath>
#include <optional>

#include "3d.h"
#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "local_frame_georef.h"
#include "mio.h"
#include "ply.h"
#include "stl.h"

namespace ddb {

namespace {

void parseModelMetadata(const fs::path &path, Entry &entry)
{
    // Georeferencing for 3D models is optional and lives in a sidecar (the OpenDroneMap
    // reference_lla.json and coords.txt and friends resolved by detectLocalFrameGeoref).
    // When present we flag the model as georeferenced and derive a WGS84 footprint
    // from its local bounds, so it behaves like every other geo entry (footprint on
    // the dataset map, globe in the unified viewer). Best-effort: any failure leaves
    // the model indexed in local space rather than aborting the parse.
    try
    {
        const auto georef = detectLocalFrameGeoref(io::toUtf8(path));
        if (!georef.has_value())
        {
            entry.properties["georeferenced"] = false;
            return;
        }

        applyLocalFrameOrigin(entry, *georef);

        ModelInfo info;
        if (getModelInfo(io::toUtf8(path), info) && info.hasBounds)
            applyLocalFrameFootprint(entry, *georef, LocalBounds{info.minX, info.minY, info.maxX, info.maxY});
    }
    catch (const std::exception &e)
    {
        LOGD << "Model parse warning: " << e.what();
        entry.properties["warning"] = std::string("model parse failed: ") + e.what();
    }
}

class ModelEntryHandler : public EntryHandler {
  public:
    ModelEntryHandler() : EntryHandler(EntryType::Model, "Model") {}

    // A .ply can be a mesh, a plain point cloud or a Gaussian Splat; a .gltf/.glb can also
    // carry point geometry (3D Gaussian Splat tiles) or lines, neither of which yields a
    // buildable model: both are classified from their content. An .stl has no signature, so it
    // is a model only when its size (binary) or its first tokens (ASCII) say it holds triangles.
    std::vector<std::string> extensions() const override {
        return {"obj", "ply", "stl", "gltf", "glb"};
    }

    EntryType detect(const fs::path& path, FingerprintContext& /*ctx*/) const override {
        io::Path p(path);
        if (p.checkExtension({"ply"}))
            return identifyPly(path);
        if (p.checkExtension({"stl"}))
            return identifyStl(path);
        if (p.checkExtension({"gltf", "glb"}))
            return identifyGltf(path);
        return EntryType::Model;
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseModelMetadata(path, entry);
    }

    std::string subfolder() const override { return Bm3dBuildSubfolder; }

    // A model built before bm3d has its Nexus file in nxs/ and is served as model.nexus until a
    // rebuild publishes bm3d/ (the dispatcher then removes nxs/). A Nexus file stands in for
    // model.bm3d, not for the whole entry: such a model usually has 3D Tiles as well.
    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("model.bm3d",
                                    artifactPath(Bm3dBuildSubfolder, Bm3dFileName),
                                    "application/vnd.dronedb.bm3d"),
                BuildArtifact::file("model.nexus",
                                    artifactPath(LegacyNexusBuildSubfolder, LegacyNexusFileName),
                                    "application/octet-stream")
                    .legacyFor("model.bm3d"),
                // Offered as a ZIP: 3D Tiles is what other tools (Cesium, QGIS, ArcGIS) open
                BuildArtifact::directory("model.3dtiles",
                                         Tiles3DBuildSubfolder,
                                         "application/json",
                                         TilesetFileName,
                                         "model.3dtiles.zip")};
    }

    // Lock order is primary Model lock then tiles lock. Recovery runs before the complete
    // bm3d check of a build, without recursively taking the tiles lock.
    void recoverOutput(const fs::path& baseOutputPath) const override {
        recoverModel3DTilesSwap(io::toUtf8(baseOutputPath / Tiles3DBuildSubfolder));
    }

    // The 3D Tiles tileset is intentionally optional for completeness. The sole exception is
    // an identifiable interrupted tiles swap (usable backup and missing/incomplete final):
    // it is reported incomplete so the normal completeness sweep can enqueue recovery
    // without rebuilding bm3d/. A model with only the legacy nxs/ is incomplete: it is rebuilt.
    bool isComplete(const fs::path& baseOutputPath, bool reportRecoverable) const override {
        if (reportRecoverable && hasRecoverableModel3DTilesSwap(baseOutputPath / Tiles3DBuildSubfolder))
            return false;
        return directoryHasNonEmptyContent(baseOutputPath / Bm3dBuildSubfolder);
    }

    void build(const BuildContext& ctx) const override {
        const std::string inputPath = io::toUtf8(ctx.inputPath);
        const std::string datasetRoot = io::toUtf8(ctx.datasetRoot);
        buildBm3d(inputPath,
                  io::toUtf8(ctx.tempFolder / Bm3dFileName),
                  false,
                  datasetRoot,
                  ctx.options);

        // Additive, best-effort OGC 3D Tiles output for the unified Giro3D viewer.
        // Written directly to a sibling 3dtiles/ folder (buildModel3DTiles performs
        // its own atomic temp+rename), exactly as buildVector co-produces vec/+mvt/.
        // Non-blocking by design: if Obj2Tiles is unavailable or fails on a given
        // mesh, the bm3d output above is still published and build completeness
        // stays keyed on bm3d/ (see isComplete), so the build never loops.
        // Nothing is rethrown from here, BuildDepMissingException included: it cannot
        // occur (buildBm3d validated the same input above), and rethrowing it would
        // discard tempFolder before model.bm3d reaches bm3d/.
        // Lock order is the primary lock (held here) then the "3dtiles" lock.
        // cleanupBuild follows the same order, so the only way to meet a busy
        // tiles lock is a tiles build started outside a Model build (the
        // "ddb tiles3d" command on this output folder).
        const fs::path tilesFolder = ctx.baseOutputPath / Tiles3DBuildSubfolder;
        try {
            Tiles3DOptions tilesOptions;
            tilesOptions.overwrite = true;
            tilesOptions.datasetRoot = datasetRoot;
            tilesOptions.build = ctx.options;
            buildModel3DTiles(inputPath, io::toUtf8(tilesFolder), tilesOptions);
        } catch (const BuildInProgressException& tilesEx) {
            LOGW << "3D Tiles for " << ctx.entry.path
                 << " not generated: another tiles build holds "
                 << buildLockPath(io::toUtf8(tilesFolder))
                 << " (for example 'ddb tiles3d'): " << tilesEx.what()
                 << ". The bm3d output is published; rebuild with force to "
                 << "produce the tiles";
        } catch (const std::exception& tilesEx) {
            LOGW << "3D Tiles generation skipped for " << ctx.entry.path << ": " << tilesEx.what();
        } catch (...) {
            LOGW << "3D Tiles generation skipped for " << ctx.entry.path << " (unknown error)";
        }
    }
};

}  // namespace

std::unique_ptr<EntryHandler> makeModelEntryHandler() {
    return std::make_unique<ModelEntryHandler>();
}

}  // namespace ddb
