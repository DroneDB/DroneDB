/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GaussianSplat entries: the formats of splat_format.h (and splat-flavoured PLY), built into a
// level-of-detail RAD container with its bounds.

#include <optional>

#include "build_layout.h"
#include "json.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gsplat.h"
#include "local_frame_georef.h"
#include "logger.h"
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

    // A splat trained in the frame of an ODM project (or of a model with a georeferencing sidecar) is
    // georeferenced: its origin is the point of the entry. The footprint needs the bounds of the
    // scene, which only the build knows, so it is not a polygon of the entry but the wgs84Footprint
    // of bounds.json. Best-effort: a splat whose georeference cannot be used stays local, and says why
    // in its warning property as a model does.
    try
    {
        if (const auto georef = detectLocalFrameGeoref(io::toUtf8(path)))
            applyLocalFrameOrigin(entry, *georef);
    }
    catch (const std::exception &e)
    {
        LOGW << "Splat georeference ignored for " << io::toUtf8(path) << ": " << e.what();
        entry.properties["warning"] = std::string("georeference ignored: ") + e.what();
    }
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
                                    GsplatMediaType, GsplatRadFileName),
                // Written by builds before the RAD layout; read when present, listed only while
                // model.rad is missing, removed alone (not gsplat/) once model.rad exists
                BuildArtifact::file("gsplat.spz", artifactPath(GsplatBuildSubfolder, GsplatFileName),
                                    GsplatMediaType, GsplatFileName)
                    .legacyFor("gsplat.lod"),
                BuildArtifact::file("gsplat.bounds", artifactPath(GsplatBuildSubfolder, GsplatBoundsFileName),
                                    "application/json"),
                // Made on demand by exportSplatSpz, never by a build: listed only while it exists, it
                // is no render role and not part of completeness, and the next build removes it with
                // the rest of gsplat/. Not legacy: it stands in for nothing, so it never replaces
                // model.rad and cleanup never touches it
                BuildArtifact::file(GsplatExportRole, artifactPath(GsplatBuildSubfolder, GsplatExportFileName),
                                    GsplatMediaType, GsplatExportFileName)};
    }

    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / GsplatBuildSubfolder / GsplatRadFileName);
    }

    // Runs build-lod to produce model.rad + bounds.json. Throws a
    // plain AppException if build-lod is unavailable: a missing tool is not a missing
    // dependency, so the build fails instead of staying pending.
    void build(const BuildContext& ctx) const override {
        // The georeference was found when the entry was indexed and is the only source of it
        std::optional<LocalFrameGeoref> georef;
        if (ctx.entry.properties.is_object() && ctx.entry.properties.contains("georef"))
            georef = localFrameGeorefFromProperty(ctx.entry.properties["georef"]);
        buildGsplat(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder), georef);
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
