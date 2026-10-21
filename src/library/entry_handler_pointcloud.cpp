/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// PointCloud entries: LAS/LAZ/E57/PTS/XYZ (and point-only PLY), built into a COPC file.

#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "mio.h"
#include "pointcloud.h"

namespace ddb {

namespace {

class PointCloudEntryHandler : public EntryHandler {
  public:
    PointCloudEntryHandler() : EntryHandler(EntryType::PointCloud, "PointCloud") {}

    std::string subfolder() const override { return CopcBuildSubfolder; }

    // A leaf inside a built EPT dataset is not built: the guard skips residual ept-data
    // folders from older builds.
    bool isBuildable(const Entry& e) const override {
        return fs::path(e.path).parent_path().filename().string() != LegacyEptDataFolder;
    }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"pointcloud.copc", artifactPath(CopcBuildSubfolder, CopcFileName), ArtifactKind::File,
                 "application/vnd.laszip+copc", CopcFileName, "", false},
                {"pointcloud.ept", LegacyEptBuildSubfolder, ArtifactKind::Directory, "application/json", "",
                 LegacyEptFileName, true}};
    }

    void build(const BuildContext& ctx) const override {
        const std::vector vec = {ctx.inputPath.string()};
        // Defensive cleanup: remove any legacy EPT artifacts from previous builds
        io::assureIsRemoved(ctx.baseOutputPath / LegacyEptBuildSubfolder);
        buildCopc(vec, ctx.tempFolder.string());
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::BuildArtifact; }
};

}  // namespace

std::unique_ptr<EntryHandler> makePointCloudEntryHandler() {
    return std::make_unique<PointCloudEntryHandler>();
}

}  // namespace ddb
