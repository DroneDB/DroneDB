/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_HANDLER_H
#define ENTRY_HANDLER_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "build_budget.h"
#include "entry_types.h"
#include "exceptions.h"
#include "fs.h"

/**
 * Entry-type handler registry: everything libddb knows about one entry
 * type lives in one EntryHandler (src/library/entry_handler_<type>.cpp), registered once by
 * its EntryType. Detection, metadata parsing, build dispatch, completeness, sidecars,
 * thumbnails, tiling and the artifact manifest all ask the registry, so a new type is one new
 * handler file plus one registration (registerBuiltinEntryHandlers, entry_handler_registry.cpp)
 * and no other code changes.
 *
 * The artifact list of a handler (artifacts()) is the role-tagged build manifest: the roles
 * are a stable vocabulary that says what an artifact is for, the paths are composed from the
 * names in build_layout.h.
 */
namespace ddb {

class Database;
class Tiler;
struct Entry;
struct FingerprintContext;

// ---- Artifact manifest -------------------------------------------------------------

enum class ArtifactKind { File, Directory };

// The artifacts of a type are listed in the order of artifacts(), its primary output first:
// the file thumbnails and tiles are made from.
struct BuildArtifact {
    // What the artifact is for, never where it is: "raster.cog", "vector.mvt", ...
    std::string role;
    // Relative to the entry's build root "<build root>/<hash>/", '/'-separated
    std::string path;
    ArtifactKind kind = ArtifactKind::File;
    // Of the file a client opens: the file itself, or the index of a Directory. Hosts send it
    // as the Content-Type of the artifact (tests/contract pins the values)
    std::string mediaType;
    // File name offered for a download of the artifact; empty when it is not downloadable
    std::string downloadName;
    // Directory artifacts: the descriptor file inside it ("metadata.json", "tileset.json")
    std::string index;
    // Leftover of an older layout: read when present, never written
    bool legacy = false;
    // Legacy artifacts only (required for them): the role this one stands in for. It is served
    // only while an artifact of that role does not exist, so an entry never lists both
    std::string fallbackFor;
};

// "<folder>/<file>": the path of an artifact file, from the names in build_layout.h
inline std::string artifactPath(const std::string& folder, const std::string& file) {
    return folder + "/" + file;
}

// ---- Thumbnails and tiles ------------------------------------------------------------------

enum class ThumbnailSource {
    None,           // the type has no thumbnail
    File,           // rendered from the entry's own file (images, rasters)
    BuildArtifact   // rendered from one of its build artifacts by the handler's ThumbnailGenerator
};

// Renders the thumbnail of the files it accepts (e.g. the COPC artifact of a point cloud).
class ThumbnailGenerator {
  public:
    virtual ~ThumbnailGenerator() = default;
    virtual bool accepts(const fs::path& input) const = 0;
    // Writes a WEBP thumbnail to outImagePath, or into *outBuffer when outImagePath is empty
    virtual void generate(const fs::path& input, int thumbSize, const fs::path& outImagePath,
                          uint8_t** outBuffer, int* outBufferSize) const = 0;
};

// Creates the tiler of the files it accepts (e.g. the COPC artifact of a point cloud). Files no
// factory accepts are tiled by GDAL (TilerHelper).
class TilerFactory {
  public:
    virtual ~TilerFactory() = default;
    virtual bool accepts(const fs::path& tileablePath) const = 0;
    virtual std::unique_ptr<Tiler> create(const fs::path& input, const fs::path& outputFolder,
                                          int tileSize, bool tms) const = 0;
};

// ---- Sidecars and builds -------------------------------------------------------------------

// Files that belong to a main file of this type: same folder and stem, one of sidecarExtensions.
// Their build is the (forced) build of the main file; the main file's extension matches in any
// letter case (.shp, .SHP, .Shp).
struct SidecarRule {
    std::vector<std::string> mainExtensions;     // lowercase, without the dot, in lookup order
    std::vector<std::string> sidecarExtensions;  // lowercase, without the dot
};

// One build attempt of an entry, prepared by the dispatcher (build_dispatch.cpp): the build
// lock of outputFolder is held, tempFolder exists and baseOutputPath exists.
struct BuildContext {
    Database* db = nullptr;
    // The entry whose artifacts are built (the main file when a sidecar triggered the build)
    const Entry& entry;
    // Absolute path of the entry's file
    fs::path inputPath;
    fs::path datasetRoot;
    // "<build root>/<hash>"
    fs::path baseOutputPath;
    // baseOutputPath / subfolder()
    fs::path outputFolder;
    // Scratch folder; renamed onto outputFolder after build() when usesTempFolder()
    fs::path tempFolder;
    bool force = false;
    // What the caller asked of this build (the thread cap): the handler passes it on to the
    // builders that use threads
    BuildOptions options{};
};

// ---- Completeness helpers ------------------------------------------------------------------

// The file exists, is a regular file and is not empty.
bool fileExistsAndNonEmpty(const fs::path& p);
// The directory exists and holds at least one non-empty regular file (recursively): an empty
// folder left by an aborted build is not an output.
bool directoryHasNonEmptyContent(const fs::path& dir);

// ---- The handler ---------------------------------------------------------------------------

// Every capability defaults to "none": a type without a build, a parser or a thumbnail
// overrides nothing but its name.
class EntryHandler {
  public:
    EntryHandler(EntryType type, std::string name) : type_(type), name_(std::move(name)) {}
    virtual ~EntryHandler() = default;
    EntryHandler(const EntryHandler&) = delete;
    EntryHandler& operator=(const EntryHandler&) = delete;

    EntryType type() const { return type_; }
    // typeToHuman() name ("GeoRaster")
    const std::string& name() const { return name_; }

    // ---- Detection (fingerprint)

    // The file extensions (lowercase, without the dot) this handler classifies. Each extension
    // belongs to one handler; files whose extension no handler claims are Generic.
    virtual std::vector<std::string> extensions() const { return {}; }
    // The type of a file with one of extensions(). May read the file (and fill ctx) to refine
    // it; the default is type().
    virtual EntryType detect(const fs::path& /*path*/, FingerprintContext& /*ctx*/) const { return type_; }
    // Starts the reads of the file that detect() makes of every file with this extension, and
    // leaves them in ctx for detect() to take, so they run while the caller works on the file
    // (parseEntry hashes it meanwhile). Only reads detect() makes whatever the file holds: a
    // file is never read more often, nor read when it would not have been. The default starts
    // none.
    virtual void prefetch(const fs::path& /*path*/, FingerprintContext& /*ctx*/) const {}

    // ---- Metadata (parseEntry)

    // Fills properties and geometries of a file of this type (entry.type is set). ctx holds
    // what detect() already read.
    virtual void parseMetadata(const fs::path& /*path*/, Entry& /*entry*/, FingerprintContext& /*ctx*/) const {}

    // ---- Build

    // Primary output subfolder in "<build root>/<hash>/" (build_layout.h); empty when the type
    // is not built. Its ".building" lock guards the whole build.
    virtual std::string subfolder() const { return {}; }
    bool buildable() const { return !subfolder().empty(); }
    // An entry of this type is built (a handler may exclude some entries, e.g. by location)
    virtual bool isBuildable(const Entry& /*e*/) const { return buildable(); }
    // Every artifact a build of this type can produce (the build manifest)
    virtual std::vector<BuildArtifact> artifacts() const { return {}; }
    // The top-level folders of artifacts() in "<build root>/<hash>/", in order, without
    // duplicates. The folders a build writes; includeLegacy adds the ones it only reads, which
    // are the data that follows the entry (copy) or goes stale with it (cleanup)
    std::vector<std::string> artifactFolders(bool includeLegacy = false) const {
        std::vector<std::string> folders;
        for (const auto& a : artifacts()) {
            if (a.legacy && !includeLegacy)
                continue;
            const std::string folder = a.path.substr(0, a.path.find('/'));
            bool seen = false;
            for (const auto& f : folders)
                seen = seen || f == folder;
            if (!seen)
                folders.push_back(folder);
        }
        return folders;
    }
    // build() writes into BuildContext::tempFolder, which is then renamed onto the output
    // folder; false when build() publishes its outputs itself
    virtual bool usesTempFolder() const { return true; }
    // Repairs an interrupted publish under the build lock, before completeness is checked
    virtual void recoverOutput(const fs::path& /*baseOutputPath*/) const {}
    // The artifacts in baseOutputPath ("<build root>/<hash>") exist and are not empty.
    // reportRecoverable: an output recoverOutput() would repair counts as incomplete.
    // The default: the output subfolder holds a non-empty file (the file names may vary by input).
    virtual bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/ = true) const {
        return buildable() && directoryHasNonEmptyContent(baseOutputPath / subfolder());
    }
    virtual void build(const BuildContext& /*ctx*/) const {
        throw AppException(name_ + " entries are not built");
    }

    // ---- Sidecars

    virtual std::vector<SidecarRule> sidecarRules() const { return {}; }
    // Every sidecar extension of sidecarRules(), in order, without duplicates
    std::vector<std::string> sidecarExtensions() const {
        std::vector<std::string> exts;
        for (const auto& rule : sidecarRules())
            for (const auto& ext : rule.sidecarExtensions) {
                bool seen = false;
                for (const auto& e : exts)
                    seen = seen || e == ext;
                if (!seen)
                    exts.push_back(ext);
            }
        return exts;
    }

    // ---- Thumbnails and tiles

    virtual ThumbnailSource thumbnailSource() const { return ThumbnailSource::None; }
    // The generator of ThumbnailSource::BuildArtifact thumbnails
    virtual const ThumbnailGenerator* thumbnailGenerator() const { return nullptr; }
    virtual const TilerFactory* tilerFactory() const { return nullptr; }
    // GDAL tiles files of this type as they are (georeferenced rasters); other files handled
    // by GDAL are geoprojected first
    virtual bool tilesWithoutReprojection() const { return false; }

  private:
    EntryType type_;
    std::string name_;
};

// ---- The registry --------------------------------------------------------------------------
//
// The built-in handlers are registered on first use. Lookups are thread-safe; register and
// unregister only while no other thread uses the handler being replaced (process start, tests):
// lookups return references that stay valid until their handler is replaced.

// The handler of a type. A value without a handler gets a handler without capabilities, named "?".
const EntryHandler& entryHandler(EntryType type);
// The handler of a type, or nullptr
const EntryHandler* findEntryHandler(EntryType type);
// Every registered handler, by type value
std::vector<const EntryHandler*> entryHandlers();
// The handler that claims the extension of path (case-insensitive), or nullptr
const EntryHandler* findEntryHandlerByExtension(const fs::path& path);
// The types whose handler builds them, by type value
std::vector<EntryType> buildableEntryTypes();
// The first ThumbnailGenerator (by type value) that accepts input, or nullptr
const ThumbnailGenerator* findThumbnailGenerator(const fs::path& input);
// The first TilerFactory (by type value) that accepts tileablePath, or nullptr
const TilerFactory* findTilerFactory(const fs::path& tileablePath);
// The main-file extensions (lowercase, without the dot, in lookup order) a file at path is a
// sidecar of; empty when no SidecarRule lists its extension
std::vector<std::string> sidecarMainExtensions(const std::string& path);

// Registers handler for handler->type() and returns the handler it replaces (nullptr if none).
// Tests register fake handlers with it to check that the registry alone drives detection and the
// build manifest of a type.
// Throws InvalidArgsException when one of its extensions belongs to another type.
std::unique_ptr<EntryHandler> registerEntryHandler(std::unique_ptr<EntryHandler> handler);
// Removes the handler of type and returns it (nullptr if none)
std::unique_ptr<EntryHandler> unregisterEntryHandler(EntryType type);

// Registers a handler for its lifetime and then puts back the one it replaced: the way tests
// register a fake handler without leaking it into the next test.
class ScopedEntryHandler {
  public:
    explicit ScopedEntryHandler(std::unique_ptr<EntryHandler> handler) : type_(handler->type()) {
        previous_ = registerEntryHandler(std::move(handler));
    }
    ~ScopedEntryHandler() {
        if (previous_)
            registerEntryHandler(std::move(previous_));
        else
            unregisterEntryHandler(type_);
    }
    ScopedEntryHandler(const ScopedEntryHandler&) = delete;
    ScopedEntryHandler& operator=(const ScopedEntryHandler&) = delete;

  private:
    EntryType type_;
    std::unique_ptr<EntryHandler> previous_;
};

}  // namespace ddb

#endif  // ENTRY_HANDLER_H
