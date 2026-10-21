/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Tiles3D entries: OGC 3D Tiles archives (.3tz), extracted into a tileset folder.

#include "build_layout.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "mzip.h"

namespace ddb {

namespace {

class Tiles3DEntryHandler : public EntryHandler {
  public:
    Tiles3DEntryHandler() : EntryHandler(EntryType::Tiles3D, "Tiles3D") {}

    std::string subfolder() const override { return Tiles3DBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"model.3dtiles", Tiles3DBuildSubfolder, ArtifactKind::Directory, "application/json", "",
                 TilesetFileName, false}};
    }

    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / Tiles3DBuildSubfolder / TilesetFileName);
    }

    // Extraction is hardened against Zip-Slip / absolute paths in mzip::_extractAll. The
    // archive is only valid when tileset.json is present at its root; the temp folder is
    // then renamed onto build/{hash}/3dtiles/ by the dispatcher.
    void build(const BuildContext& ctx) const override {
        zip::extractAll(ctx.inputPath.string(), ctx.tempFolder.string());
        if (!fileExistsAndNonEmpty(ctx.tempFolder / TilesetFileName))
            throw AppException("Invalid .3tz archive: tileset.json not found at root");
    }
};

}  // namespace

std::unique_ptr<EntryHandler> makeTiles3DEntryHandler() {
    return std::make_unique<Tiles3DEntryHandler>();
}

}  // namespace ddb
