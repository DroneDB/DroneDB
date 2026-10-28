/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GaussianSplat entries: the formats of splat_format.h (and splat-flavoured PLY), built into a
// level-of-detail RAD container with its bounds.

#include "build_layout.h"
#include "json.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gsplat.h"
#include "mio.h"
#include "splat_format.h"
#include "thumbs.h"

namespace ddb {

namespace {

// On-demand thumbnails of the files the format table names for it: the .rad artifact and .spz files
class SplatThumbnailGenerator : public ThumbnailGenerator {
  public:
    bool accepts(const fs::path& input) const override {
        return splatAccepts(input, SplatStage::Thumbnail);
    }
    void generate(const fs::path& input, int thumbSize, const fs::path& outImagePath, uint8_t** outBuffer,
                  int* outBufferSize) const override {
        if (splatFormatOf(input) == SplatFormat::Rad)
            generateSplatThumbFromRad(input, thumbSize, outImagePath, outBuffer, outBufferSize);
        else
            generateSplatThumb(input, thumbSize, outImagePath, outBuffer, outBufferSize);
    }
};

void parseGaussianSplatMetadata(const fs::path &path, Entry &entry)
{
    // Lightweight metadata only (no full decode): splat count + SH degree.
    GaussianSplatInfo info;
    if (getGaussianSplatInfo(io::toUtf8(path), info))
        entry.properties = info;
}

class GaussianSplatEntryHandler : public EntryHandler {
  public:
    GaussianSplatEntryHandler() : EntryHandler(EntryType::GaussianSplat, "GaussianSplat") {}

    // The formats an extension alone makes a splat (a splat-flavoured .ply is detected by the
    // Model handler, from its header)
    std::vector<std::string> extensions() const override { return splatExtensions(SplatStage::Detect); }

    // The extension is an explicit signal: the file is not read, and a broken one fails when it
    // is built
    EntryType detect(const fs::path& /*path*/, FingerprintContext& /*ctx*/) const override {
        return EntryType::GaussianSplat;
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseGaussianSplatMetadata(path, entry);
    }

    std::string subfolder() const override { return GsplatBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("gsplat.lod", artifactPath(GsplatBuildSubfolder, GsplatRadFileName),
                                    GsplatMediaType),
                // Written by builds before the RAD layout; read when present, listed only while
                // model.rad is missing, removed alone (not gsplat/) once model.rad exists
                BuildArtifact::file("gsplat.spz", artifactPath(GsplatBuildSubfolder, GsplatFileName),
                                    GsplatMediaType)
                    .legacyFor("gsplat.lod"),
                BuildArtifact::file("gsplat.bounds", artifactPath(GsplatBuildSubfolder, GsplatBoundsFileName),
                                    "application/json")};
    }

    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / GsplatBuildSubfolder / GsplatRadFileName);
    }

    // Runs build-lod to produce model.rad + bounds.json. Throws a
    // plain AppException if build-lod is unavailable: a missing tool is not a missing
    // dependency, so the build fails instead of staying pending.
    void build(const BuildContext& ctx) const override {
        buildGsplat(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder));
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
