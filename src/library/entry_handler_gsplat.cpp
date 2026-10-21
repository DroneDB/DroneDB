/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GaussianSplat entries: SPZ/SPLAT/KSPLAT (and splat-flavoured PLY), built into a
// level-of-detail RAD container with its bounds (and georeferencing when known).

#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gsplat.h"

namespace ddb {

namespace {

class GaussianSplatEntryHandler : public EntryHandler {
  public:
    GaussianSplatEntryHandler() : EntryHandler(EntryType::GaussianSplat, "GaussianSplat") {}

    std::string subfolder() const override { return GsplatBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"gsplat.lod", artifactPath(GsplatBuildSubfolder, GsplatRadFileName), ArtifactKind::File,
                 "application/octet-stream", "", "", false},
                {"gsplat.spz", artifactPath(GsplatBuildSubfolder, GsplatFileName), ArtifactKind::File,
                 "application/octet-stream", "", "", false},
                {"gsplat.bounds", artifactPath(GsplatBuildSubfolder, GsplatBoundsFileName), ArtifactKind::File,
                 "application/json", "", "", false},
                {"gsplat.georef", artifactPath(GsplatBuildSubfolder, GsplatGeorefFileName), ArtifactKind::File,
                 "application/json", "", "", false}};
    }

    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / GsplatBuildSubfolder / GsplatRadFileName);
    }

    // Runs build-lod to produce model.rad + bounds.json (+ georef.json when known). Throws a
    // plain AppException if build-lod is unavailable: a missing tool is not a missing
    // dependency, so the build fails instead of staying pending.
    void build(const BuildContext& ctx) const override {
        buildGsplat(ctx.inputPath.string(), ctx.tempFolder.string());
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::BuildArtifact; }
};

}  // namespace

std::unique_ptr<EntryHandler> makeGaussianSplatEntryHandler() {
    return std::make_unique<GaussianSplatEntryHandler>();
}

}  // namespace ddb
