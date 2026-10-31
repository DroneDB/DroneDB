/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The EntryHandler registry is the one place that knows each entry type. These tests
// pin what the built-in handlers declare (the dispatch tables they replaced) and register a
// fake type that is detected, parsed, built and reported complete without any other change.

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "build.h"
#include "build_layout.h"
#include "dbops.h"
#include "entry.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"
#include "utils.h"

namespace {

using namespace ddb;

const std::vector<EntryType> AllBuiltinTypes = {
    EntryType::Undefined, EntryType::Directory, EntryType::Generic,  EntryType::GeoImage, EntryType::GeoRaster,   EntryType::PointCloud, EntryType::Image,  EntryType::DroneDB, EntryType::Markdown,
    EntryType::Video,     EntryType::GeoVideo,  EntryType::Model,    EntryType::Panorama, EntryType::GeoPanorama, EntryType::Vector,     EntryType::GaussianSplat, EntryType::Tiles3D};

TEST(entryHandler, EveryBuiltinTypeHasItsHandlerAndName) {
    const std::map<EntryType, std::string> names = {
        {EntryType::Undefined, "Undefined"},     {EntryType::Directory, "Directory"},   {EntryType::Generic, "Generic"},
        {EntryType::GeoImage, "GeoImage"},       {EntryType::GeoRaster, "GeoRaster"},   {EntryType::PointCloud, "PointCloud"},
        {EntryType::Image, "Image"},             {EntryType::DroneDB, "DroneDB"},       {EntryType::Markdown, "Markdown"},
        {EntryType::Video, "Video"},             {EntryType::GeoVideo, "GeoVideo"},     {EntryType::Model, "Model"},
        {EntryType::Panorama, "Panorama"},       {EntryType::GeoPanorama, "GeoPanorama"}, {EntryType::Vector, "Vector"},
        {EntryType::GaussianSplat, "GaussianSplat"}, {EntryType::Tiles3D, "Tiles3D"}};

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(static_cast<int>(t));
        const EntryHandler* handler = findEntryHandler(t);
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->type(), t);
        EXPECT_EQ(handler->name(), names.at(t));
        EXPECT_EQ(typeToHuman(t), names.at(t));
        EXPECT_EQ(typeFromHuman(names.at(t)), t);
    }

    const auto handlers = entryHandlers();
    ASSERT_EQ(handlers.size(), AllBuiltinTypes.size());
    for (size_t i = 0; i < handlers.size(); i++)
        EXPECT_EQ(handlers[i]->type(), AllBuiltinTypes[i]);

    EXPECT_EQ(typeToHuman(static_cast<EntryType>(29)), "?");
    EXPECT_EQ(entryHandler(static_cast<EntryType>(29)).name(), "?");
    EXPECT_EQ(typeFromHuman("geoRASTER"), EntryType::GeoRaster);
    EXPECT_EQ(typeFromHuman("nope"), EntryType::Undefined);

    const std::vector<std::string> expectedNames = {
        "generic", "geoimage", "georaster", "pointcloud", "image", "dronedb", "markdown", "video",
        "geovideo", "model", "panorama", "geopanorama", "vector", "gaussiansplat", "tiles3d"};
    EXPECT_EQ(getEntryTypeNames(), expectedNames);
}

// The types isBuildableInternal() built, with their output subfolders
TEST(entryHandler, BuildableTypesAndSubfoldersArePinned) {
    const std::map<EntryType, std::string> buildable = {{EntryType::PointCloud, "copc"},
                                                        {EntryType::GeoRaster, "cog"},
                                                        {EntryType::Model, "bm3d"},
                                                        {EntryType::Vector, "vec"},
                                                        {EntryType::GaussianSplat, "gsplat"},
                                                        {EntryType::Tiles3D, "3dtiles"}};

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        const EntryHandler& handler = entryHandler(t);
        const auto it = buildable.find(t);
        EXPECT_EQ(handler.buildable(), it != buildable.end());
        EXPECT_EQ(handler.subfolder(), it != buildable.end() ? it->second : "");
    }

    EXPECT_EQ(buildableEntryTypes(),
              (std::vector<EntryType>{EntryType::GeoRaster, EntryType::PointCloud, EntryType::Model, EntryType::Vector, EntryType::GaussianSplat, EntryType::Tiles3D}));

    // A point cloud inside a legacy EPT dataset is not built
    Entry leaf;
    leaf.type = EntryType::PointCloud;
    leaf.path = "survey/ept-data/0-0-0-0.laz";
    EXPECT_FALSE(entryHandler(EntryType::PointCloud).isBuildable(leaf));
    leaf.path = "survey/cloud.laz";
    EXPECT_TRUE(entryHandler(EntryType::PointCloud).isBuildable(leaf));

    // Only Vector publishes its outputs itself
    for (const auto t : buildableEntryTypes())
        EXPECT_EQ(entryHandler(t).usesTempFolder(), t != EntryType::Vector) << typeToHuman(t);
}

// supportsThumbnails() (Image, GeoImage, Panorama, GeoPanorama, GeoRaster) and the STAC thumbnail
// asset (those plus PointCloud and GaussianSplat)
TEST(entryHandler, ThumbnailSourcesArePinned) {
    const std::map<EntryType, ThumbnailSource> sources = {
        {EntryType::Image, ThumbnailSource::File},
        {EntryType::GeoImage, ThumbnailSource::File},
        {EntryType::Panorama, ThumbnailSource::File},
        {EntryType::GeoPanorama, ThumbnailSource::File},
        {EntryType::GeoRaster, ThumbnailSource::File},
        {EntryType::PointCloud, ThumbnailSource::BuildArtifact},
        {EntryType::GaussianSplat, ThumbnailSource::BuildArtifact}};

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        const auto it = sources.find(t);
        const ThumbnailSource expected = it != sources.end() ? it->second : ThumbnailSource::None;
        EXPECT_EQ(entryHandler(t).thumbnailSource(), expected);
        EXPECT_EQ(supportsThumbnails(t), expected == ThumbnailSource::File);
    }
}

// generateThumb() and the tilers dispatched on these suffixes before the registry
TEST(entryHandler, ThumbnailGeneratorsAndTilerFactoriesArePinned) {
    const ThumbnailGenerator* copc = entryHandler(EntryType::PointCloud).thumbnailGenerator();
    const ThumbnailGenerator* splat = entryHandler(EntryType::GaussianSplat).thumbnailGenerator();
    ASSERT_NE(copc, nullptr);
    ASSERT_NE(splat, nullptr);
    EXPECT_EQ(findThumbnailGenerator("build/h/copc/cloud.copc.laz"), copc);
    EXPECT_EQ(findThumbnailGenerator("build/h/gsplat/model.rad"), splat);
    EXPECT_EQ(findThumbnailGenerator("scene.SPZ"), splat);
    EXPECT_EQ(findThumbnailGenerator("cloud.laz"), nullptr);
    EXPECT_EQ(findThumbnailGenerator("photo.jpg"), nullptr);
    EXPECT_EQ(findThumbnailGenerator("ortho.tif"), nullptr);

    const TilerFactory* copcTiles = entryHandler(EntryType::PointCloud).tilerFactory();
    ASSERT_NE(copcTiles, nullptr);
    EXPECT_EQ(findTilerFactory("build/h/copc/cloud.copc.laz"), copcTiles);
    EXPECT_EQ(findTilerFactory("ortho.tif"), nullptr);
    EXPECT_EQ(findTilerFactory("cloud.laz"), nullptr);

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        EXPECT_EQ(entryHandler(t).tilesWithoutReprojection(), t == EntryType::GeoRaster);
        if (t != EntryType::PointCloud && t != EntryType::GaussianSplat) {
            EXPECT_EQ(entryHandler(t).thumbnailGenerator(), nullptr);
        }
        if (t != EntryType::PointCloud) {
            EXPECT_EQ(entryHandler(t).tilerFactory(), nullptr);
        }
    }
}

// The role vocabulary of the build manifest, with paths composed from build_layout.h
TEST(entryHandler, ArtifactManifestIsPinned) {
    using Row = std::tuple<std::string, std::string, ArtifactKind, std::string, bool>;  // role, path, kind, index, legacy
    const std::map<EntryType, std::vector<Row>> expected = {
        {EntryType::GeoRaster, {Row{"raster.cog", "cog/cog.tif", ArtifactKind::File, "", false}}},
        {EntryType::PointCloud,
         {Row{"pointcloud.copc", "copc/cloud.copc.laz", ArtifactKind::File, "", false},
          Row{"pointcloud.ept", "ept", ArtifactKind::Directory, "ept.json", true}}},
        {EntryType::Model,
         {Row{"model.bm3d", "bm3d/model.bm3d", ArtifactKind::File, "", false},
          Row{"model.nexus", "nxs/model.nxz", ArtifactKind::File, "", true},
          Row{"model.3dtiles", "3dtiles", ArtifactKind::Directory, "tileset.json", false}}},
        {EntryType::Vector,
         {Row{"vector.gpkg", "vec/source.gpkg", ArtifactKind::File, "", false},
          Row{"vector.mvt", "mvt", ArtifactKind::Directory, "metadata.json", false},
          Row{"vector.local", "vec/local.json", ArtifactKind::File, "", false}}},
        {EntryType::GaussianSplat,
         {Row{"gsplat.lod", "gsplat/model.rad", ArtifactKind::File, "", false},
          Row{"gsplat.spz", "gsplat/model.spz", ArtifactKind::File, "", true},
          Row{"gsplat.bounds", "gsplat/bounds.json", ArtifactKind::File, "", false},
          Row{"gsplat.export", "gsplat/export.spz", ArtifactKind::File, "", false}}},
        {EntryType::Tiles3D,
         {Row{"model.3dtiles", "3dtiles", ArtifactKind::Directory, "tileset.json", false}}}};

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        std::vector<Row> actual;
        for (const auto& a : entryHandler(t).artifacts()) {
            actual.emplace_back(a.role, a.path, a.kind, a.index, a.legacy);
            EXPECT_FALSE(a.mediaType.empty()) << a.role;
        }
        const auto it = expected.find(t);
        EXPECT_EQ(actual, it != expected.end() ? it->second : std::vector<Row>{});
    }

    // The folders a build of the type owns
    EXPECT_EQ(entryHandler(EntryType::Vector).artifactFolders(), (std::vector<std::string>{"vec", "mvt"}));
    EXPECT_EQ(entryHandler(EntryType::PointCloud).artifactFolders(), (std::vector<std::string>{"copc"}));
    EXPECT_EQ(entryHandler(EntryType::Model).artifactFolders(),
              (std::vector<std::string>{"bm3d", "3dtiles"}));
    // The folders that follow the entry or go stale with it: the legacy ones too
    EXPECT_EQ(entryHandler(EntryType::PointCloud).artifactFolders(true), (std::vector<std::string>{"copc", "ept"}));
    EXPECT_EQ(entryHandler(EntryType::Model).artifactFolders(true),
              (std::vector<std::string>{"bm3d", "nxs", "3dtiles"}));
    EXPECT_EQ(entryHandler(EntryType::Vector).artifactFolders(true), entryHandler(EntryType::Vector).artifactFolders());
}

// A legacy artifact is served while the role it stands in for is missing: every one of them
// names a current role of its own handler, and a current artifact names none
TEST(entryHandler, LegacyArtifactsNameTheRoleTheyStandInFor) {
    for (const EntryHandler* handler : entryHandlers()) {
        SCOPED_TRACE(handler->name());
        std::set<std::string> current;
        for (const auto& a : handler->artifacts())
            if (!a.legacy)
                current.insert(a.role);
        for (const auto& a : handler->artifacts()) {
            SCOPED_TRACE(a.role);
            if (a.legacy) {
                EXPECT_FALSE(a.fallbackFor.empty()) << "a legacy artifact must name the role it stands in for";
                EXPECT_EQ(current.count(a.fallbackFor), 1u) << "no current artifact has the role " << a.fallbackFor;
            } else {
                EXPECT_TRUE(a.fallbackFor.empty());
            }
        }
    }

    const auto& artifacts = entryHandler(EntryType::PointCloud).artifacts();
    ASSERT_EQ(artifacts.size(), 2u);
    EXPECT_EQ(artifacts[1].role, "pointcloud.ept");
    EXPECT_EQ(artifacts[1].fallbackFor, "pointcloud.copc");

    // The Nexus file of a model stands in for model.bm3d only: the 3D Tiles are a role of their own
    const auto& model = entryHandler(EntryType::Model).artifacts();
    ASSERT_EQ(model.size(), 3u);
    EXPECT_EQ(model[0].role, "model.bm3d");
    EXPECT_EQ(model[0].mediaType, "application/vnd.dronedb.bm3d");
    EXPECT_EQ(model[1].role, "model.nexus");
    EXPECT_EQ(model[1].fallbackFor, "model.bm3d");
    EXPECT_EQ(model[2].role, "model.3dtiles");
    EXPECT_FALSE(model[2].legacy);

    // The SPZ of a splat built before the RAD layout stands in for model.rad only: bounds.json is
    // current whichever layout built it
    const auto& splat = entryHandler(EntryType::GaussianSplat).artifacts();
    ASSERT_EQ(splat.size(), 4u);
    EXPECT_EQ(splat[0].role, "gsplat.lod");
    EXPECT_FALSE(splat[0].legacy);
    EXPECT_EQ(splat[1].role, "gsplat.spz");
    EXPECT_EQ(splat[1].fallbackFor, "gsplat.lod");
    EXPECT_EQ(splat[2].role, "gsplat.bounds");
    EXPECT_FALSE(splat[2].legacy);
    // The .spz an export makes for other tools stands in for nothing: it is no legacy artifact, so
    // it never replaces model.rad and the cleanup of superseded outputs never removes it
    EXPECT_EQ(splat[3].role, "gsplat.export");
    EXPECT_FALSE(splat[3].legacy);
    EXPECT_TRUE(splat[3].fallbackFor.empty());
}

// The artifacts a download offers: the file names a host sends them under
TEST(entryHandler, SplatArtifactsAreDownloadable) {
    std::map<std::string, std::string> names;
    for (const auto& a : entryHandler(EntryType::GaussianSplat).artifacts())
        names[a.role] = a.downloadName;
    EXPECT_EQ(names, (std::map<std::string, std::string>{{"gsplat.lod", "model.rad"},
                                                         {"gsplat.spz", "model.spz"},
                                                         {"gsplat.bounds", ""},
                                                         {"gsplat.export", "export.spz"}}));
}

// The 3D Tiles of a model are offered as a ZIP of the directory (other tools open 3D Tiles); the
// meshes of the DroneDB viewer are not, and a .3tz is its own archive already
TEST(entryHandler, ModelTilesAreDownloadableAsAnArchive) {
    std::map<std::string, std::string> names;
    for (const auto& a : entryHandler(EntryType::Model).artifacts())
        names[a.role] = a.downloadName;
    EXPECT_EQ(names, (std::map<std::string, std::string>{{"model.bm3d", ""},
                                                         {"model.nexus", ""},
                                                         {"model.3dtiles", "model.3dtiles.zip"}}));

    const auto tiles3d = entryHandler(EntryType::Tiles3D).artifacts();
    ASSERT_EQ(tiles3d.size(), 1u);
    EXPECT_EQ(tiles3d[0].kind, ArtifactKind::Directory);
    EXPECT_TRUE(tiles3d[0].downloadName.empty());
}

TEST(entryHandler, SidecarRulesArePinned) {
    EXPECT_EQ(entryHandler(EntryType::Vector).sidecarExtensions(),
              (std::vector<std::string>{"cpg", "dbf", "shx", "prj"}));
    for (const auto t : AllBuiltinTypes) {
        if (t != EntryType::Vector) {
            EXPECT_TRUE(entryHandler(t).sidecarRules().empty()) << typeToHuman(t);
        }
    }

    EXPECT_EQ(sidecarMainExtensions("a/roads.dbf"), (std::vector<std::string>{"shp"}));
    EXPECT_EQ(sidecarMainExtensions("a/roads.CPG"), (std::vector<std::string>{"shp"}));
    EXPECT_EQ(sidecarMainExtensions("a/plan.prj"), (std::vector<std::string>{"shp", "dxf", "dwg"}));
    EXPECT_TRUE(sidecarMainExtensions("a/roads.shp").empty());
    EXPECT_TRUE(sidecarMainExtensions("a/noext").empty());
}

// Completeness rules of the built-in types (what isBuildOutputComplete checked)
TEST(entryHandler, CompletenessRules) {
    TestArea ta(TEST_NAME, true);
    const fs::path base = ta.getFolder("hash");

    const auto complete = [&](EntryType t) { return entryHandler(t).isComplete(base); };

    // Folder types: an empty folder is incomplete, any non-empty file completes it
    fs::create_directories(base / "cog");
    EXPECT_FALSE(complete(EntryType::GeoRaster));
    writeFile(base / "cog" / "whatever.tif", "x");
    EXPECT_TRUE(complete(EntryType::GeoRaster));

    // Vector: source.gpkg and (metadata.json or local.json)
    writeFile(base / "vec" / "source.gpkg", "x");
    EXPECT_FALSE(complete(EntryType::Vector));
    writeFile(base / "vec" / "local.json", "{}");
    EXPECT_TRUE(complete(EntryType::Vector));

    // Gaussian Splat: model.rad; Tiles3D: tileset.json
    writeFile(base / "gsplat" / "model.spz", "x");
    EXPECT_FALSE(complete(EntryType::GaussianSplat));
    // The .spz of an export for other tools is not a build: it completes nothing
    writeFile(base / "gsplat" / "export.spz", "x");
    EXPECT_FALSE(complete(EntryType::GaussianSplat));
    writeFile(base / "gsplat" / "model.rad", "x");
    EXPECT_TRUE(complete(EntryType::GaussianSplat));
    fs::create_directories(base / "3dtiles");
    EXPECT_FALSE(complete(EntryType::Tiles3D));
    writeFile(base / "3dtiles" / "tileset.json", "{}");
    EXPECT_TRUE(complete(EntryType::Tiles3D));

    // Not built: never complete
    EXPECT_FALSE(complete(EntryType::Image));
}

TEST(entryHandler, ExtensionsBelongToOneType) {
    class Thief : public EntryHandler {
      public:
        Thief() : EntryHandler(static_cast<EntryType>(30), "Thief") {}
        std::vector<std::string> extensions() const override { return {"md"}; }
    };
    EXPECT_THROW(registerEntryHandler(std::make_unique<Thief>()), InvalidArgsException);
    EXPECT_EQ(findEntryHandler(static_cast<EntryType>(30)), nullptr);
    unregisterEntryHandler(static_cast<EntryType>(30));
    EXPECT_THROW(registerEntryHandler(nullptr), InvalidArgsException);
}

// ---- A type the library does not know -----------------------------------------------------

constexpr auto FakeType = static_cast<EntryType>(30);

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream content;
    content << in.rdbuf();
    return content.str();
}

// Claims ".ddbfake" files, records their content as a property, builds "<scratch>/out.txt"
// (the input's bytes) and is complete when fake/out.txt is not empty
class FakeEntryHandler : public EntryHandler {
  public:
    FakeEntryHandler() : EntryHandler(FakeType, "FakeType") {}

    std::vector<std::string> extensions() const override { return {"ddbfake"}; }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext&) const override {
        entry.properties["content"] = slurp(path);
    }

    std::string subfolder() const override { return "fake"; }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file("fake.out", "fake/out.txt", "text/plain", "out.txt")};
    }

    bool isComplete(const fs::path& baseOutputPath, bool) const override {
        return fileExistsAndNonEmpty(baseOutputPath / "fake" / "out.txt");
    }

    void build(const BuildContext& ctx) const override {
        writeFile(ctx.tempFolder / "out.txt", slurp(ctx.inputPath));
    }
};

TEST(entryHandler, FakeTypeIsDetectedParsedAndBuiltWithoutAnyOtherChange) {
    EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
    IndexFixture fx(TEST_NAME);
    writeFile(fx.root / "a.ddbfake", "alpha");
    writeFile(fx.root / "b.DDBFAKE", "beta");

    {
        ScopedEntryHandler registration(std::make_unique<FakeEntryHandler>());
        EXPECT_EQ(typeToHuman(FakeType), "FakeType");
        EXPECT_EQ(typeFromHuman("faketype"), FakeType);
        const auto buildable = buildableEntryTypes();
        EXPECT_NE(std::find(buildable.begin(), buildable.end(), FakeType), buildable.end());

        // Detected by its extension (any letter case) and parsed by its handler
        EXPECT_EQ(fingerprint(fx.root / "b.DDBFAKE"), FakeType);
        addToIndex(fx.db.get(), {(fx.root / "a.ddbfake").string(), (fx.root / "b.DDBFAKE").string()});
        Entry a;
        ASSERT_TRUE(loadEntry(fx.db.get(), "a.ddbfake", a));
        EXPECT_EQ(a.type, FakeType);
        EXPECT_EQ(a.properties.value("content", ""), "alpha");

        EXPECT_EQ(buildSubfolder(fx.db.get(), "a.ddbfake"), "fake");
        EXPECT_FALSE(isBuildComplete(fx.db.get(), "a.ddbfake"));

        build(fx.db.get(), "a.ddbfake", "", false);
        const fs::path outA = fx.db->buildDirectory() / a.hash / "fake" / "out.txt";
        ASSERT_TRUE(fs::exists(outA));
        EXPECT_EQ(slurp(outA), "alpha");
        EXPECT_TRUE(isBuildComplete(fx.db.get(), "a.ddbfake"));
        EXPECT_FALSE(fs::exists(fx.db->buildDirectory() / (a.hash + ".pending")));

        // buildAll takes its type list from the registry
        EXPECT_FALSE(isBuildComplete(fx.db.get(), "b.DDBFAKE"));
        buildAll(fx.db.get(), "", false);
        EXPECT_TRUE(isBuildComplete(fx.db.get(), "b.DDBFAKE"));

        const auto statuses = getBuildStatus(fx.db.get(), {"a.ddbfake", "b.DDBFAKE"});
        ASSERT_EQ(statuses.size(), 2u);
        for (const auto& s : statuses) {
            EXPECT_TRUE(s.buildable) << s.path;
            EXPECT_TRUE(s.complete) << s.path;
        }
    }

    // Unregistered again: the files are Generic and the entries are no longer buildable
    EXPECT_EQ(findEntryHandler(FakeType), nullptr);
    EXPECT_EQ(typeToHuman(FakeType), "?");
    EXPECT_EQ(fingerprint(fx.root / "a.ddbfake"), EntryType::Generic);
    EXPECT_FALSE(isBuildable(fx.db.get(), "a.ddbfake"));
}

// The extension table fingerprint() used to spell out, now declared by the handlers
TEST(entryHandler, ExtensionsArePinned) {
    const std::map<EntryType, std::vector<std::string>> expected = {
        {EntryType::Markdown, {"md"}},
        {EntryType::PointCloud, {"laz", "las", "e57", "pts", "xyz"}},
        {EntryType::GaussianSplat, {"splat", "spz", "ksplat", "sog", "rad"}},
        {EntryType::Model, {"obj", "ply", "stl", "gltf", "glb"}},
        {EntryType::Tiles3D, {"3tz"}},
        {EntryType::Vector, {"geojson", "dxf", "dwg", "shp", "shz", "fgb", "topojson", "kml", "kmz", "gpkg", "gpx"}},
        {EntryType::Image, {"jpg", "jpeg", "dng", "tif", "tiff", "jp2", "j2k", "png", "gif", "webp", "mp4", "mov", "webm",
                 "m4v", "avi", "mkv"}}};
    for (const auto t : AllBuiltinTypes) {
        const auto it = expected.find(t);
        EXPECT_EQ(entryHandler(t).extensions(), it != expected.end() ? it->second : std::vector<std::string>{})
            << typeToHuman(t);
    }

    EXPECT_EQ(findEntryHandlerByExtension("a/b/notes.MD"), findEntryHandler(EntryType::Markdown));
    EXPECT_EQ(findEntryHandlerByExtension("photo.JPG"), findEntryHandler(EntryType::Image));
    EXPECT_EQ(findEntryHandlerByExtension("noext"), nullptr);
    EXPECT_EQ(findEntryHandlerByExtension(".md"), nullptr);  // a hidden file, no extension
    EXPECT_EQ(findEntryHandlerByExtension("archive.zip"), nullptr);
}

// Content-free detection through the registry (files that need no reading)
TEST(entryHandler, FingerprintByExtension) {
    TestArea ta(TEST_NAME, true);
    const auto file = [&](const std::string& name) { return writeFile(ta.getFolder() / name, "x"); };

    EXPECT_EQ(fingerprint(file("readme.md")), EntryType::Markdown);
    EXPECT_EQ(fingerprint(file("cloud.LAZ")), EntryType::PointCloud);
    EXPECT_EQ(fingerprint(file("model.obj")), EntryType::Model);
    EXPECT_EQ(fingerprint(file("city.3tz")), EntryType::Tiles3D);
    EXPECT_EQ(fingerprint(file("roads.shp")), EntryType::Vector);
    EXPECT_EQ(fingerprint(file("roads.dbf")), EntryType::Generic);
    EXPECT_EQ(fingerprint(file("flight.GPX")), EntryType::Vector);
    EXPECT_EQ(fingerprint(file("splat.ksplat")), EntryType::GaussianSplat);
    EXPECT_EQ(fingerprint(file("scene.SOG")), EntryType::GaussianSplat);
    EXPECT_EQ(fingerprint(file("splats.rad")), EntryType::GaussianSplat);
    EXPECT_EQ(fingerprint(file("noext")), EntryType::Generic);
    // Unreadable content: the Image handler falls back to Image, a broken .ply to Generic
    EXPECT_EQ(fingerprint(file("photo.png")), EntryType::Image);
    EXPECT_EQ(fingerprint(file("broken.ply")), EntryType::Generic);
    // An .stl is a model only when its content is one
    EXPECT_EQ(fingerprint(file("broken.stl")), EntryType::Generic);
}

TEST(entryHandler, ScopedHandlerPutsBackTheOneItReplaced) {
    const EntryHandler* original = findEntryHandler(EntryType::Markdown);
    ASSERT_NE(original, nullptr);
    {
        class Notes : public EntryHandler {
          public:
            Notes() : EntryHandler(EntryType::Markdown, "Notes") {}
        };
        ScopedEntryHandler replaced(std::make_unique<Notes>());
        EXPECT_EQ(typeToHuman(EntryType::Markdown), "Notes");
    }
    EXPECT_EQ(findEntryHandler(EntryType::Markdown), original);
    EXPECT_EQ(typeToHuman(EntryType::Markdown), "Markdown");
}

// ---- Sidecars ------------------------------------------------------------------------------

// The main file of a sidecar matches whatever the letter case of its extension (Registry's
// rule). The stem stays exact.
TEST(entryHandler, SidecarMainExtensionMatchesAnyLetterCase) {
    IndexFixture fx(TEST_NAME);
    const std::string mainHash(64, 'a');
    insertEntry(fx.db.get(), "maps/roads.Shp", mainHash, EntryType::Vector);
    insertEntry(fx.db.get(), "maps/roads.dbf", std::string(64, 'b'), EntryType::Generic);
    insertEntry(fx.db.get(), "maps/Roads.shx", std::string(64, 'c'), EntryType::Generic);  // other stem
    writeFile(fx.db->buildDirectory() / mainHash / "vec" / "source.gpkg", "x");
    writeFile(fx.db->buildDirectory() / mainHash / "mvt" / "metadata.json", "x");

    EXPECT_EQ(buildSubfolder(fx.db.get(), "maps/roads.dbf"), "vec");
    EXPECT_TRUE(isBuildComplete(fx.db.get(), "maps/roads.dbf"));
    EXPECT_FALSE(isBuildable(fx.db.get(), "maps/Roads.shx"));

    const auto statuses = getBuildStatus(fx.db.get(), {"maps/roads.dbf", "maps/Roads.shx"});
    ASSERT_EQ(statuses.size(), 2u);
    EXPECT_TRUE(statuses[0].buildable);
    EXPECT_TRUE(statuses[0].complete);
    EXPECT_FALSE(statuses[1].buildable);
    EXPECT_FALSE(statuses[1].complete);
}

// Lookup order: rule extensions (shp, dxf, dwg), then lowercase, uppercase, mixed spellings
TEST(entryHandler, SidecarMainsLookupOrder) {
    IndexFixture fx(TEST_NAME);
    for (const auto* p : {"d/x.Shp", "d/x.SHP", "d/x.shp", "d/x.DWG", "d/x.dxf", "d/x.sHp", "d/x.prj",
                          "d/x.backup.shp", "d/X.shp", "d/x.shp.zip", "d/xy.shp", "e/x.shp", "d/x.shpx"})
        insertEntry(fx.db.get(), p, std::string(64, 'a'), EntryType::Vector);

    EXPECT_EQ(findSidecarMains(fx.db.get(), "d/x.prj"),
              (std::vector<std::string>{"d/x.shp", "d/x.SHP", "d/x.Shp", "d/x.sHp", "d/x.dxf", "d/x.DWG"}));
    EXPECT_EQ(findSidecarMains(fx.db.get(), "d/x.DBF"),
              (std::vector<std::string>{"d/x.shp", "d/x.SHP", "d/x.Shp", "d/x.sHp"}));
    EXPECT_TRUE(findSidecarMains(fx.db.get(), "d/x.shp").empty());   // not a sidecar
    EXPECT_TRUE(findSidecarMains(fx.db.get(), "d/z.dbf").empty());   // no main file
    EXPECT_EQ(findSidecarMains(fx.db.get(), "x.dbf"), std::vector<std::string>{});  // root folder
}

}  // namespace
