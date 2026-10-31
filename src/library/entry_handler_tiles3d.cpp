/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Tiles3D entries: OGC 3D Tiles archives (.3tz), extracted into a tileset folder.

#include "build_internal.h"
#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "mzip.h"
#include "tiles3d.h"

namespace ddb {

namespace {

void parseTiles3DMetadata(const fs::path &path, Entry &entry)
{
    // Reads tileset.json from the .3tz (ZIP) root and, when the tileset is
    // georeferenced (WGS84 region or ECEF bounding volume), derives a WGS84
    // footprint so the entry gets a map footprint and the unified viewer opens
    // it in globe mode. Best-effort: any failure leaves the archive indexed in
    // local space rather than aborting the parse.
    try
    {
        const auto found = getTiles3DInfo(io::toUtf8(path));
        if (!found)
        {
            entry.properties["georeferenced"] = false;
            return;
        }
        const Tiles3DInfo &info = *found;

        entry.properties["georeferenced"] = info.georeferenced;
        if (!info.assetVersion.empty())
            entry.properties["tilesetVersion"] = info.assetVersion;
        entry.properties["geometricError"] = info.geometricError;

        if (info.georeferenced && info.hasBounds)
        {
            entry.point_geom.clear();
            entry.point_geom.addPoint(info.centerLon, info.centerLat, info.centerAlt);

            // 5-point closed ring (CCW), matching point-cloud/model footprints.
            entry.polygon_geom.clear();
            entry.polygon_geom.addPoint(info.west, info.south, 0.0);
            entry.polygon_geom.addPoint(info.east, info.south, 0.0);
            entry.polygon_geom.addPoint(info.east, info.north, 0.0);
            entry.polygon_geom.addPoint(info.west, info.north, 0.0);
            entry.polygon_geom.addPoint(info.west, info.south, 0.0);
        }
    }
    catch (const std::exception &e)
    {
        LOGD << "Tiles3D parse warning: " << e.what();
        entry.properties["georeferenced"] = false;
    }
}

class Tiles3DEntryHandler : public EntryHandler {
  public:
    Tiles3DEntryHandler() : EntryHandler(EntryType::Tiles3D, "Tiles3D") {}

    // OGC 3D Tiles archive (.3tz, a ZIP container with tileset.json at its root). The
    // extension is the signal; ZIP validity is checked when the footprint is read
    // (parseMetadata) and when the archive is extracted at build time.
    std::vector<std::string> extensions() const override { return {"3tz"}; }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseTiles3DMetadata(path, entry);
    }

    std::string subfolder() const override { return Tiles3DBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::directory("model.3dtiles", Tiles3DBuildSubfolder, "application/json",
                                         TilesetFileName)};
    }

    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / Tiles3DBuildSubfolder / TilesetFileName);
    }

    // Extraction is hardened in zip::extractAll: Zip-Slip / absolute paths, names Windows cannot
    // create, and the default expansion limits (a .3tz is stored tiles, close to 1:1), and an
    // archive whose declared size does not fit the free disk is refused before it is written.
    // The archive is only valid when tileset.json is present at its root; the temp folder is
    // then renamed onto build/{hash}/3dtiles/ by the dispatcher.
    void build(const BuildContext& ctx) const override {
        zip::ExtractOptions options;
        options.beforeWrite = [&ctx](uint64_t declaredTotal) {
            ensureSufficientDiskSpace(ctx.tempFolder, declaredTotal);
        };
        zip::extractAll(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder), options);
        if (!fileExistsAndNonEmpty(ctx.tempFolder / TilesetFileName))
            throw AppException("Invalid .3tz archive: tileset.json not found at root");
    }
};

}  // namespace

std::unique_ptr<EntryHandler> makeTiles3DEntryHandler() {
    return std::make_unique<Tiles3DEntryHandler>();
}

}  // namespace ddb
