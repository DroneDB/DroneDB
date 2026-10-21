/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GaussianSplat entries: SPZ/SPLAT/KSPLAT (and splat-flavoured PLY), built into a
// level-of-detail RAD container with its bounds (and georeferencing when known).

#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gsplat.h"
#include "mio.h"
#include "rad.h"
#include "thumbs.h"

namespace ddb {

namespace {

// On-demand thumbnails of the .rad artifact and of .spz files
class SplatThumbnailGenerator : public ThumbnailGenerator {
  public:
    bool accepts(const fs::path& input) const override {
        return isRadPath(input.string()) || isSpzPath(input.string());
    }
    void generate(const fs::path& input, int thumbSize, const fs::path& outImagePath, uint8_t** outBuffer,
                  int* outBufferSize) const override {
        if (isRadPath(input.string()))
            generateSplatThumbFromRad(input, thumbSize, outImagePath, outBuffer, outBufferSize);
        else
            generateSplatThumb(input, thumbSize, outImagePath, outBuffer, outBufferSize);
    }
};

void parseGaussianSplatMetadata(const fs::path &path, Entry &entry)
{
    // Lightweight metadata only (no full decode): splat count + SH degree.
    // Georeferencing (and therefore footprint geometry) is established at
    // build time from an ODX coords file; a freshly indexed splat renders
    // in local space until then.
    GaussianSplatInfo info;
    if (getGaussianSplatInfo(path.string(), info))
    {
        entry.properties = info.toJSON();
        entry.properties["georeferenced"] = false;
    }
}

class GaussianSplatEntryHandler : public EntryHandler {
  public:
    GaussianSplatEntryHandler() : EntryHandler(EntryType::GaussianSplat, "GaussianSplat") {}

    // Gaussian Splat binary formats (a splat-flavoured .ply is detected by the Model handler)
    std::vector<std::string> extensions() const override { return {"spz", "splat", "ksplat"}; }

    // The extension is an explicit signal; a lightweight content check only logs a
    // warning when the bytes look unexpected, never reclassifies the file.
    EntryType detect(const fs::path& path, FingerprintContext& /*ctx*/) const override {
        io::Path p(path);
        if (p.checkExtension({"spz"}) && !looksLikeSpz(path))
            LOGD << "File has .spz extension but no NGSP/gzip magic: " << path.string();
        else if (p.checkExtension({"splat"}) && !looksLikeSplatBinary(path))
            LOGD << "File has .splat extension but size is not a multiple of 32: " << path.string();
        return EntryType::GaussianSplat;
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseGaussianSplatMetadata(path, entry);
    }

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

    const ThumbnailGenerator* thumbnailGenerator() const override { return &thumbnails_; }

  private:
    SplatThumbnailGenerator thumbnails_;
};

}  // namespace

std::unique_ptr<EntryHandler> makeGaussianSplatEntryHandler() {
    return std::make_unique<GaussianSplatEntryHandler>();
}

}  // namespace ddb
