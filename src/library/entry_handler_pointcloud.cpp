/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// PointCloud entries: LAS/LAZ/E57/PTS/XYZ (and point-only PLY), built into a COPC file.

#include "build_layout.h"
#include "json.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "mio.h"
#include "pctiler.h"
#include "pointcloud.h"
#include "thumbs.h"

namespace ddb {

namespace {

// The COPC artifact of a point cloud: its thumbnail and its tiles
class CopcThumbnailGenerator : public ThumbnailGenerator {
  public:
    bool accepts(const fs::path& input) const override { return isCopcPath(io::toUtf8(input)); }
    void generate(const fs::path& input, int thumbSize, const fs::path& outImagePath, uint8_t** outBuffer,
                  int* outBufferSize) const override {
        generatePointCloudThumb(input, thumbSize, outImagePath, outBuffer, outBufferSize);
    }
};

class CopcTilerFactory : public TilerFactory {
  public:
    bool accepts(const fs::path& tileablePath) const override { return isCopcPath(io::toUtf8(tileablePath)); }
    std::unique_ptr<Tiler> create(const fs::path& input, const fs::path& outputFolder, int tileSize,
                                  bool tms) const override {
        return std::make_unique<PointCloudTiler>(io::toUtf8(input), io::toUtf8(outputFolder), tileSize, tms);
    }
};

void parsePointCloudMetadata(const fs::path &path, Entry &entry)
{
    PointCloudInfo info;
    if (getPointCloudInfo(io::toUtf8(path), info))
    {
        entry.properties = info;
        entry.polygon_geom = info.polyBounds;
        entry.point_geom = info.centroid;
    }
}

class PointCloudEntryHandler : public EntryHandler {
  public:
    PointCloudEntryHandler() : EntryHandler(EntryType::PointCloud, "PointCloud") {}

    std::vector<std::string> extensions() const override { return {"laz", "las", "e57", "pts", "xyz"}; }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parsePointCloudMetadata(path, entry);
    }

    std::string subfolder() const override { return CopcBuildSubfolder; }

    // A leaf inside a built EPT dataset is not built: the guard skips residual ept-data
    // folders from older builds.
    bool isBuildable(const Entry& e) const override {
        return io::toUtf8(io::fromUtf8(e.path).parent_path().filename()) != LegacyEptDataFolder;
    }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"pointcloud.copc", artifactPath(CopcBuildSubfolder, CopcFileName), ArtifactKind::File,
                 "application/vnd.laszip+copc", CopcFileName, "", false},
                {"pointcloud.ept", LegacyEptBuildSubfolder, ArtifactKind::Directory, "application/json", "",
                 LegacyEptFileName, true}};
    }

    void build(const BuildContext& ctx) const override {
        const std::vector vec = {io::toUtf8(ctx.inputPath)};
        // Defensive cleanup: remove any legacy EPT artifacts from previous builds
        io::assureIsRemoved(ctx.baseOutputPath / LegacyEptBuildSubfolder);
        buildCopc(vec, io::toUtf8(ctx.tempFolder), ctx.options);
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::BuildArtifact; }

    const ThumbnailGenerator* thumbnailGenerator() const override { return &thumbnails_; }

    const TilerFactory* tilerFactory() const override { return &tiler_; }

  private:
    CopcThumbnailGenerator thumbnails_;
    CopcTilerFactory tiler_;
};

}  // namespace

std::unique_ptr<EntryHandler> makePointCloudEntryHandler() {
    return std::make_unique<PointCloudEntryHandler>();
}

}  // namespace ddb
