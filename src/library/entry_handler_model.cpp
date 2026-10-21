/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Model entries: textured meshes (OBJ, PLY, glTF), built into Nexus plus an additive OGC 3D
// Tiles tileset.

#include <optional>

#include "3d.h"
#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"

namespace ddb {

namespace {

class ModelEntryHandler : public EntryHandler {
  public:
    ModelEntryHandler() : EntryHandler(EntryType::Model, "Model") {}

    std::string subfolder() const override { return NexusBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"model.nexus", artifactPath(NexusBuildSubfolder, NexusFileName), ArtifactKind::File,
                 "application/octet-stream", "", "", false},
                {"model.3dtiles", Tiles3DBuildSubfolder, ArtifactKind::Directory, "application/json", "",
                 TilesetFileName, false}};
    }

    // Lock order is primary Model lock then tiles lock. Recovery runs before the complete
    // Nexus check of a build, without recursively taking the tiles lock.
    void recoverOutput(const fs::path& baseOutputPath) const override {
        recoverModel3DTilesSwap((baseOutputPath / Tiles3DBuildSubfolder).string());
    }

    // The 3D Tiles tileset is intentionally optional for completeness. The sole exception is
    // an identifiable interrupted tiles swap (usable backup and missing/incomplete final):
    // it is reported incomplete so the normal completeness sweep can enqueue recovery
    // without rebuilding nxs/.
    bool isComplete(const fs::path& baseOutputPath, bool reportRecoverable) const override {
        if (reportRecoverable && hasRecoverableModel3DTilesSwap(baseOutputPath / Tiles3DBuildSubfolder))
            return false;
        return directoryHasNonEmptyContent(baseOutputPath / NexusBuildSubfolder);
    }

    void build(const BuildContext& ctx) const override {
        const std::string inputPath = ctx.inputPath.string();
        const std::string datasetRoot = ctx.datasetRoot.string();
        buildNexus(inputPath, (ctx.tempFolder / NexusFileName).string(), false, datasetRoot);

        // Additive, best-effort OGC 3D Tiles output for the unified Giro3D viewer.
        // Written directly to a sibling 3dtiles/ folder (buildModel3DTiles performs
        // its own atomic temp+rename), exactly as buildVector co-produces vec/+mvt/.
        // Non-blocking by design: if Obj2Tiles is unavailable or fails on a given
        // mesh, the Nexus output above is still published and build completeness
        // stays keyed on nxs/ (see isComplete), so the build never loops.
        // Nothing is rethrown from here, BuildDepMissingException included: it cannot
        // occur (buildNexus validated the same input above), and rethrowing it would
        // discard tempFolder before model.nxz reaches nxs/.
        // Lock order is the primary lock (held here) then the "3dtiles" lock.
        // cleanupBuild follows the same order, so the only way to meet a busy
        // tiles lock is a tiles build started outside a Model build (the
        // "ddb tiles3d" command on this output folder).
        const fs::path tilesFolder = ctx.baseOutputPath / Tiles3DBuildSubfolder;
        try {
            buildModel3DTiles(inputPath, tilesFolder.string(), true, std::nullopt, true, datasetRoot);
        } catch (const BuildInProgressException& tilesEx) {
            LOGW << "3D Tiles for " << ctx.entry.path << " not generated: another tiles build holds "
                 << buildLockPath(tilesFolder.string())
                 << " (for example 'ddb tiles3d'): "
                 << tilesEx.what() << ". The Nexus output is published; rebuild with force to "
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
