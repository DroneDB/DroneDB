/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-08: the EntryHandler registry is the one place that knows each entry type. These tests
// pin what the built-in handlers declare (the dispatch tables they replaced) and register a
// fake type that is detected, parsed, built and reported complete without any other change.

#include <algorithm>
#include <fstream>
#include <map>
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
    Undefined, Directory, Generic,  GeoImage, GeoRaster,   PointCloud, Image,  DroneDB, Markdown,
    Video,     GeoVideo,  Model,    Panorama, GeoPanorama, Vector,     GaussianSplat, Tiles3D};

TEST(entryHandler, EveryBuiltinTypeHasItsHandlerAndName) {
    const std::map<EntryType, std::string> names = {
        {Undefined, "Undefined"},     {Directory, "Directory"},   {Generic, "Generic"},
        {GeoImage, "GeoImage"},       {GeoRaster, "GeoRaster"},   {PointCloud, "PointCloud"},
        {Image, "Image"},             {DroneDB, "DroneDB"},       {Markdown, "Markdown"},
        {Video, "Video"},             {GeoVideo, "GeoVideo"},     {Model, "Model"},
        {Panorama, "Panorama"},       {GeoPanorama, "GeoPanorama"}, {Vector, "Vector"},
        {GaussianSplat, "GaussianSplat"}, {Tiles3D, "Tiles3D"}};

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
    EXPECT_EQ(typeFromHuman("geoRASTER"), GeoRaster);
    EXPECT_EQ(typeFromHuman("nope"), Undefined);

    const std::vector<std::string> expectedNames = {
        "generic", "geoimage", "georaster", "pointcloud", "image", "dronedb", "markdown", "video",
        "geovideo", "model", "panorama", "geopanorama", "vector", "gaussiansplat", "tiles3d"};
    EXPECT_EQ(getEntryTypeNames(), expectedNames);
}

// The types isBuildableInternal() built, with their output subfolders
TEST(entryHandler, BuildableTypesAndSubfoldersArePinned) {
    const std::map<EntryType, std::string> buildable = {
        {PointCloud, "copc"}, {GeoRaster, "cog"},        {Model, "nxs"},
        {Vector, "vec"},      {GaussianSplat, "gsplat"}, {Tiles3D, "3dtiles"}};

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        const EntryHandler& handler = entryHandler(t);
        const auto it = buildable.find(t);
        EXPECT_EQ(handler.buildable(), it != buildable.end());
        EXPECT_EQ(handler.subfolder(), it != buildable.end() ? it->second : "");
    }

    EXPECT_EQ(buildableEntryTypes(),
              (std::vector<EntryType>{GeoRaster, PointCloud, Model, Vector, GaussianSplat, Tiles3D}));

    // A point cloud inside a legacy EPT dataset is not built
    Entry leaf;
    leaf.type = PointCloud;
    leaf.path = "survey/ept-data/0-0-0-0.laz";
    EXPECT_FALSE(entryHandler(PointCloud).isBuildable(leaf));
    leaf.path = "survey/cloud.laz";
    EXPECT_TRUE(entryHandler(PointCloud).isBuildable(leaf));

    // Only Vector publishes its outputs itself
    for (const auto t : buildableEntryTypes())
        EXPECT_EQ(entryHandler(t).usesTempFolder(), t != Vector) << typeToHuman(t);
}

// supportsThumbnails() (Image, GeoImage, GeoRaster) and the STAC thumbnail asset (those plus
// PointCloud and GaussianSplat)
TEST(entryHandler, ThumbnailSourcesArePinned) {
    const std::map<EntryType, ThumbnailSource> sources = {
        {Image, ThumbnailSource::File},
        {GeoImage, ThumbnailSource::File},
        {GeoRaster, ThumbnailSource::File},
        {PointCloud, ThumbnailSource::BuildArtifact},
        {GaussianSplat, ThumbnailSource::BuildArtifact}};

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
    const ThumbnailGenerator* copc = entryHandler(PointCloud).thumbnailGenerator();
    const ThumbnailGenerator* splat = entryHandler(GaussianSplat).thumbnailGenerator();
    ASSERT_NE(copc, nullptr);
    ASSERT_NE(splat, nullptr);
    EXPECT_EQ(findThumbnailGenerator("build/h/copc/cloud.copc.laz"), copc);
    EXPECT_EQ(findThumbnailGenerator("build/h/gsplat/model.rad"), splat);
    EXPECT_EQ(findThumbnailGenerator("scene.SPZ"), splat);
    EXPECT_EQ(findThumbnailGenerator("cloud.laz"), nullptr);
    EXPECT_EQ(findThumbnailGenerator("photo.jpg"), nullptr);
    EXPECT_EQ(findThumbnailGenerator("ortho.tif"), nullptr);

    const TilerFactory* copcTiles = entryHandler(PointCloud).tilerFactory();
    ASSERT_NE(copcTiles, nullptr);
    EXPECT_EQ(findTilerFactory("build/h/copc/cloud.copc.laz"), copcTiles);
    EXPECT_EQ(findTilerFactory("ortho.tif"), nullptr);
    EXPECT_EQ(findTilerFactory("cloud.laz"), nullptr);

    for (const auto t : AllBuiltinTypes) {
        SCOPED_TRACE(typeToHuman(t));
        EXPECT_EQ(entryHandler(t).tilesWithoutReprojection(), t == GeoRaster);
        if (t != PointCloud && t != GaussianSplat)
            EXPECT_EQ(entryHandler(t).thumbnailGenerator(), nullptr);
        if (t != PointCloud)
            EXPECT_EQ(entryHandler(t).tilerFactory(), nullptr);
    }
}

// The XI-04 role vocabulary, with paths composed from build_layout.h
TEST(entryHandler, ArtifactManifestIsPinned) {
    using Row = std::tuple<std::string, std::string, ArtifactKind, std::string, bool>;  // role, path, kind, index, legacy
    const std::map<EntryType, std::vector<Row>> expected = {
        {GeoRaster, {Row{"raster.cog", "cog/cog.tif", ArtifactKind::File, "", false}}},
        {PointCloud,
         {Row{"pointcloud.copc", "copc/cloud.copc.laz", ArtifactKind::File, "", false},
          Row{"pointcloud.ept", "ept", ArtifactKind::Directory, "ept.json", true}}},
        {Model,
         {Row{"model.nexus", "nxs/model.nxz", ArtifactKind::File, "", false},
          Row{"model.3dtiles", "3dtiles", ArtifactKind::Directory, "tileset.json", false}}},
        {Vector,
         {Row{"vector.gpkg", "vec/source.gpkg", ArtifactKind::File, "", false},
          Row{"vector.mvt", "mvt", ArtifactKind::Directory, "metadata.json", false},
          Row{"vector.local", "vec/local.json", ArtifactKind::File, "", false}}},
        {GaussianSplat,
         {Row{"gsplat.lod", "gsplat/model.rad", ArtifactKind::File, "", false},
          Row{"gsplat.spz", "gsplat/model.spz", ArtifactKind::File, "", false},
          Row{"gsplat.bounds", "gsplat/bounds.json", ArtifactKind::File, "", false},
          Row{"gsplat.georef", "gsplat/georef.json", ArtifactKind::File, "", false}}},
        {Tiles3D, {Row{"model.3dtiles", "3dtiles", ArtifactKind::Directory, "tileset.json", false}}}};

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
    EXPECT_EQ(entryHandler(Vector).artifactFolders(), (std::vector<std::string>{"vec", "mvt"}));
    EXPECT_EQ(entryHandler(PointCloud).artifactFolders(), (std::vector<std::string>{"copc"}));
    EXPECT_EQ(entryHandler(Model).artifactFolders(), (std::vector<std::string>{"nxs", "3dtiles"}));
}

TEST(entryHandler, SidecarRulesArePinned) {
    EXPECT_EQ(entryHandler(Vector).sidecarExtensions(),
              (std::vector<std::string>{"cpg", "dbf", "shx", "prj"}));
    for (const auto t : AllBuiltinTypes)
        if (t != Vector)
            EXPECT_TRUE(entryHandler(t).sidecarRules().empty()) << typeToHuman(t);

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
    EXPECT_FALSE(complete(GeoRaster));
    writeFile(base / "cog" / "whatever.tif", "x");
    EXPECT_TRUE(complete(GeoRaster));

    // Vector: source.gpkg and (metadata.json or local.json)
    writeFile(base / "vec" / "source.gpkg", "x");
    EXPECT_FALSE(complete(Vector));
    writeFile(base / "vec" / "local.json", "{}");
    EXPECT_TRUE(complete(Vector));

    // Gaussian Splat: model.rad; Tiles3D: tileset.json
    writeFile(base / "gsplat" / "model.spz", "x");
    EXPECT_FALSE(complete(GaussianSplat));
    writeFile(base / "gsplat" / "model.rad", "x");
    EXPECT_TRUE(complete(GaussianSplat));
    fs::create_directories(base / "3dtiles");
    EXPECT_FALSE(complete(Tiles3D));
    writeFile(base / "3dtiles" / "tileset.json", "{}");
    EXPECT_TRUE(complete(Tiles3D));

    // Not built: never complete
    EXPECT_FALSE(complete(Image));
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
        return {{"fake.out", "fake/out.txt", ArtifactKind::File, "text/plain", "out.txt", "", false}};
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
        ASSERT_TRUE(getEntry(fx.db.get(), "a.ddbfake", a));
        EXPECT_EQ(a.type, FakeType);
        EXPECT_EQ(a.properties.value("content", ""), "alpha");

        std::string subfolder;
        ASSERT_TRUE(isBuildable(fx.db.get(), "a.ddbfake", subfolder));
        EXPECT_EQ(subfolder, "fake");
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
    EXPECT_EQ(fingerprint(fx.root / "a.ddbfake"), Generic);
    std::string subfolder;
    EXPECT_FALSE(isBuildable(fx.db.get(), "a.ddbfake", subfolder));
}

// The extension table fingerprint() used to spell out, now declared by the handlers
TEST(entryHandler, ExtensionsArePinned) {
    const std::map<EntryType, std::vector<std::string>> expected = {
        {Markdown, {"md"}},
        {PointCloud, {"laz", "las", "e57", "pts", "xyz"}},
        {GaussianSplat, {"spz", "splat", "ksplat"}},
        {Model, {"obj", "ply", "gltf", "glb"}},
        {Tiles3D, {"3tz"}},
        {Vector, {"geojson", "dxf", "dwg", "shp", "shz", "fgb", "topojson", "kml", "kmz", "gpkg"}},
        {Image, {"jpg", "jpeg", "dng", "tif", "tiff", "png", "gif", "webp", "mp4", "mov", "webm", "m4v", "avi",
                 "mkv"}}};
    for (const auto t : AllBuiltinTypes) {
        const auto it = expected.find(t);
        EXPECT_EQ(entryHandler(t).extensions(), it != expected.end() ? it->second : std::vector<std::string>{})
            << typeToHuman(t);
    }

    EXPECT_EQ(findEntryHandlerByExtension("a/b/notes.MD"), findEntryHandler(Markdown));
    EXPECT_EQ(findEntryHandlerByExtension("photo.JPG"), findEntryHandler(Image));
    EXPECT_EQ(findEntryHandlerByExtension("noext"), nullptr);
    EXPECT_EQ(findEntryHandlerByExtension(".md"), nullptr);  // a hidden file, no extension
    EXPECT_EQ(findEntryHandlerByExtension("archive.zip"), nullptr);
}

// Content-free detection through the registry (files that need no reading)
TEST(entryHandler, FingerprintByExtension) {
    TestArea ta(TEST_NAME, true);
    const auto file = [&](const std::string& name) { return writeFile(ta.getFolder() / name, "x"); };

    EXPECT_EQ(fingerprint(file("readme.md")), Markdown);
    EXPECT_EQ(fingerprint(file("cloud.LAZ")), PointCloud);
    EXPECT_EQ(fingerprint(file("model.obj")), Model);
    EXPECT_EQ(fingerprint(file("city.3tz")), Tiles3D);
    EXPECT_EQ(fingerprint(file("roads.shp")), Vector);
    EXPECT_EQ(fingerprint(file("roads.dbf")), Generic);
    EXPECT_EQ(fingerprint(file("splat.ksplat")), GaussianSplat);
    EXPECT_EQ(fingerprint(file("noext")), Generic);
    // Unreadable content: the Image handler falls back to Image, a broken .ply to Generic
    EXPECT_EQ(fingerprint(file("photo.png")), Image);
    EXPECT_EQ(fingerprint(file("broken.ply")), Generic);
}

TEST(entryHandler, ScopedHandlerPutsBackTheOneItReplaced) {
    const EntryHandler* original = findEntryHandler(Markdown);
    ASSERT_NE(original, nullptr);
    {
        class Notes : public EntryHandler {
          public:
            Notes() : EntryHandler(Markdown, "Notes") {}
        };
        ScopedEntryHandler replaced(std::make_unique<Notes>());
        EXPECT_EQ(typeToHuman(Markdown), "Notes");
    }
    EXPECT_EQ(findEntryHandler(Markdown), original);
    EXPECT_EQ(typeToHuman(Markdown), "Markdown");
}

// ---- Sidecars ------------------------------------------------------------------------------

void insertEntry(Database* db, const std::string& path, const std::string& hash, EntryType type) {
    auto q = db->query("INSERT INTO entries (path, hash, type, properties, mtime, size, depth) "
                       "VALUES (?, ?, ?, '{}', 0, 1, ?)");
    q->bind(1, path);
    q->bind(2, hash);
    q->bind(3, static_cast<int>(type));
    q->bind(4, static_cast<int>(std::count(path.begin(), path.end(), '/')));
    q->execute();
}

// The main file of a sidecar matches whatever the letter case of its extension (Registry's
// rule, XI-04); it used to be found only as .shp or .SHP. The stem stays exact.
TEST(entryHandler, SidecarMainExtensionMatchesAnyLetterCase) {
    IndexFixture fx(TEST_NAME);
    const std::string mainHash(64, 'a');
    insertEntry(fx.db.get(), "maps/roads.Shp", mainHash, Vector);
    insertEntry(fx.db.get(), "maps/roads.dbf", std::string(64, 'b'), Generic);
    insertEntry(fx.db.get(), "maps/Roads.shx", std::string(64, 'c'), Generic);  // other stem
    writeFile(fx.db->buildDirectory() / mainHash / "vec" / "source.gpkg", "x");
    writeFile(fx.db->buildDirectory() / mainHash / "mvt" / "metadata.json", "x");

    std::string subfolder;
    EXPECT_TRUE(isBuildable(fx.db.get(), "maps/roads.dbf", subfolder));
    EXPECT_EQ(subfolder, "vec");
    EXPECT_TRUE(isBuildComplete(fx.db.get(), "maps/roads.dbf"));
    EXPECT_FALSE(isBuildable(fx.db.get(), "maps/Roads.shx", subfolder));

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
        insertEntry(fx.db.get(), p, std::string(64, 'a'), Vector);

    EXPECT_EQ(findSidecarMains(fx.db.get(), "d/x.prj"),
              (std::vector<std::string>{"d/x.shp", "d/x.SHP", "d/x.Shp", "d/x.sHp", "d/x.dxf", "d/x.DWG"}));
    EXPECT_EQ(findSidecarMains(fx.db.get(), "d/x.DBF"),
              (std::vector<std::string>{"d/x.shp", "d/x.SHP", "d/x.Shp", "d/x.sHp"}));
    EXPECT_TRUE(findSidecarMains(fx.db.get(), "d/x.shp").empty());   // not a sidecar
    EXPECT_TRUE(findSidecarMains(fx.db.get(), "d/z.dbf").empty());   // no main file
    EXPECT_EQ(findSidecarMains(fx.db.get(), "x.dbf"), std::vector<std::string>{});  // root folder
}

}  // namespace
