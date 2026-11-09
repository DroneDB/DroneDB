/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// build_layout.h names the on-disk build layout once. Registry, the Hub and the
// datasets already on disk depend on these exact bytes, so every value is pinned here with a
// literal: changing one is a layout migration, never a rename.

#include "build_layout.h"
#include "gtest/gtest.h"
#include "json.h"

#include <string>

namespace {

using namespace ddb;

TEST(buildLayout, MarkerAndScratchNamesArePinned) {
    EXPECT_STREQ(PendingMarkerExtension, ".pending");
    EXPECT_STREQ(PendingMarkerTempTag, ".tmp-");
    EXPECT_STREQ(BuildLockSuffix, ".building");
    EXPECT_STREQ(TempFolderInfix, "-temp-");
    EXPECT_STREQ(BackupFolderSuffix, "-old");
    EXPECT_STREQ(TrashFolderTag, "trash-");
    EXPECT_STREQ(VectorStagingPrefix, ".vec-stage-");
    EXPECT_STREQ(VectorBackupPrefix, ".vec-backup-");
}

TEST(buildLayout, SubfolderNamesArePinned) {
    EXPECT_STREQ(CogBuildSubfolder, "cog");
    EXPECT_STREQ(CopcBuildSubfolder, "copc");
    EXPECT_STREQ(Bm3dBuildSubfolder, "bm3d");
    EXPECT_STREQ(VectorBuildSubfolder, "vec");
    EXPECT_STREQ(MvtBuildSubfolder, "mvt");
    EXPECT_STREQ(GsplatBuildSubfolder, "gsplat");
    EXPECT_STREQ(Tiles3DBuildSubfolder, "3dtiles");
    EXPECT_STREQ(LegacyEptBuildSubfolder, "ept");
    EXPECT_STREQ(LegacyNexusBuildSubfolder, "nxs");
}

TEST(buildLayout, ArtifactFileNamesArePinned) {
    EXPECT_STREQ(CogFileName, "cog.tif");
    EXPECT_STREQ(CopcFileName, "cloud.copc.laz");
    EXPECT_STREQ(Bm3dFileName, "model.bm3d");
    EXPECT_STREQ(LegacyNexusFileName, "model.nxz");
    EXPECT_STREQ(VectorGpkgFileName, "source.gpkg");
    EXPECT_STREQ(MvtMetadataFileName, "metadata.json");
    EXPECT_STREQ(VectorLocalFileName, "local.json");
    EXPECT_STREQ(TilesetFileName, "tileset.json");
    EXPECT_STREQ(GsplatFileName, "model.spz");
    EXPECT_STREQ(GsplatRadFileName, "model.rad");
    EXPECT_STREQ(GsplatExportFileName, "export.spz");
    EXPECT_STREQ(GsplatBoundsFileName, "bounds.json");
    EXPECT_STREQ(GsplatThumbFileName, "thumbnail.webp");
    EXPECT_STREQ(LegacyEptFileName, "ept.json");
    EXPECT_STREQ(LegacyEptDataFolder, "ept-data");
    EXPECT_STREQ(LegacyEptHierarchyFolder, "ept-hierarchy");
}

TEST(buildLayout, ComposedNamesAreByteIdentical) {
    EXPECT_EQ(buildLockPath("b/0123/cog"), "b/0123/cog.building");
    EXPECT_EQ(buildLockPath(""), ".building");
    EXPECT_EQ(pendingMarkerTempInfix(), ".pending.tmp-");
    EXPECT_EQ(backupFolderPath(fs::path("b") / "0123" / "3dtiles"),
              fs::path("b") / "0123" / "3dtiles-old");
    EXPECT_EQ(tiles3DBackupFolderName(), "3dtiles-old");
    EXPECT_EQ(tiles3DTrashPrefix(), "3dtiles-temp-trash-");
}

TEST(buildLayout, HashNamesAre64HexCharacters) {
    const std::string lower(64, 'a');
    EXPECT_TRUE(isHashLike(lower));
    EXPECT_TRUE(isHashLike(std::string(32, '0') + std::string(32, 'F')));
    EXPECT_FALSE(isHashLike(std::string(63, 'a')));
    EXPECT_FALSE(isHashLike(std::string(65, 'a')));
    EXPECT_FALSE(isHashLike(std::string(63, 'a') + "g"));
    EXPECT_FALSE(isHashLike(std::string(63, 'a') + "\xe9"));
    EXPECT_FALSE(isHashLike(""));
}

// The build key (FR-XC-28, D-020): the literal values are computed with sha256sum, not with the code
// under test:  printf '%s' '{"kind":"enu","origin":[10.0,20.0,30.0]}' | sha256sum
//              printf '%s' 'a/b.obj' | sha256sum
const std::string Hash(64, 'a');

TEST(buildKey, APlaceIndependentBuildIsNamedByItsHash) {
    EXPECT_EQ(buildKey(Hash), Hash);
    // No georeference, however it is spelled
    const json none = nullptr, empty = json::object(), text = "x";
    EXPECT_EQ(buildKey(Hash, &none), Hash);
    EXPECT_EQ(buildKey(Hash, &empty), Hash);
    EXPECT_EQ(buildKey(Hash, &text), Hash);
}

TEST(buildKey, AGeoreferencedBuildIsNamedByItsHashAndTheDigestOfItsGeoreference) {
    const json georef = {{"kind", "enu"}, {"origin", {10.0, 20.0, 30.0}}};
    EXPECT_EQ(georef.dump(), R"({"kind":"enu","origin":[10.0,20.0,30.0]})");
    EXPECT_EQ(buildKey(Hash, &georef), Hash + "-" + "16a3603c564a75c2");
}

TEST(buildKey, TheDigestDependsOnTheGeoreferenceOnly) {
    const json one = {{"kind", "enu"}, {"origin", {10.0, 20.0, 30.0}}};
    const json same = json::parse(R"({"origin":[10.0,20.0,30.0],"kind":"enu"})");
    const json other = {{"kind", "enu"}, {"origin", {10.0, 20.0, 31.0}}};
    EXPECT_EQ(buildKey(Hash, &one), buildKey(Hash, &same));
    EXPECT_NE(buildKey(Hash, &one), buildKey(Hash, &other));
    EXPECT_NE(buildKey(Hash, &one), buildKey(std::string(64, 'b'), &one));
}

TEST(buildKey, KeysAreRecognised) {
    const std::string digest(16, '0');
    EXPECT_TRUE(isBuildKey(Hash));
    EXPECT_TRUE(isBuildKey(Hash + "-" + digest));
    EXPECT_TRUE(isBuildKey(Hash + "-" + "0123456789abcdef"));
    EXPECT_FALSE(isBuildKey(Hash + "-"));
    EXPECT_FALSE(isBuildKey(Hash + "-" + std::string(15, '0')));
    EXPECT_FALSE(isBuildKey(Hash + "-" + std::string(17, '0')));
    EXPECT_FALSE(isBuildKey(Hash + "_" + digest));
    EXPECT_FALSE(isBuildKey(Hash + "-" + std::string(15, '0') + "g"));
    EXPECT_FALSE(isBuildKey(std::string(63, 'a') + "-" + digest));
    EXPECT_FALSE(isBuildKey(""));
    EXPECT_EQ(buildKeyHash(Hash + "-" + digest), Hash);
    EXPECT_EQ(buildKeyHash(Hash), Hash);
}

// A pending marker is the marker of an entry path: two entries of the same content have two
TEST(pendingMarkerNames, AMarkerIsNamedByTheKeyAndThePathOfItsEntry) {
    EXPECT_EQ(entryPathId("a/b.obj"), "fb24e3f468aad4a6");
    EXPECT_EQ(pendingMarkerName(Hash, "a/b.obj"), Hash + ".fb24e3f468aad4a6.pending");
    EXPECT_EQ(pendingMarkerPath(fs::path("b"), Hash, "a/b.obj"), fs::path("b") / (Hash + ".fb24e3f468aad4a6.pending"));
    EXPECT_NE(pendingMarkerName(Hash, "mixA/model.obj"), pendingMarkerName(Hash, "mixB/model.obj"));
    EXPECT_EQ(legacyPendingMarkerPath(fs::path("b"), Hash), fs::path("b") / (Hash + ".pending"));
}

TEST(pendingMarkerNames, TheNameIsParsedBack) {
    const std::string digest = "0123456789abcdef";
    const auto plain = parsePendingMarkerName(Hash + ".fb24e3f468aad4a6.pending");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->key, Hash);
    EXPECT_EQ(plain->pathId, "fb24e3f468aad4a6");
    EXPECT_FALSE(plain->legacy());

    const auto keyed = parsePendingMarkerName(Hash + "-" + digest + ".fb24e3f468aad4a6.pending");
    ASSERT_TRUE(keyed);
    EXPECT_EQ(keyed->key, Hash + "-" + digest);
    EXPECT_EQ(keyed->pathId, "fb24e3f468aad4a6");

    const auto legacy = parsePendingMarkerName(Hash + ".pending");
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->key, Hash);
    EXPECT_TRUE(legacy->legacy());
}

TEST(pendingMarkerNames, OtherNamesAreNotMarkers) {
    EXPECT_FALSE(parsePendingMarkerName(""));
    EXPECT_FALSE(parsePendingMarkerName(".pending"));
    EXPECT_FALSE(parsePendingMarkerName("notes.pending"));
    EXPECT_FALSE(parsePendingMarkerName(Hash));
    EXPECT_FALSE(parsePendingMarkerName(Hash + ".fb24e3f468aad4a6"));
    EXPECT_FALSE(parsePendingMarkerName(Hash + ".3a2f3a1c6ffb3e8.pending"));
    EXPECT_FALSE(parsePendingMarkerName(Hash + ".3a2f3a1c6ffb3e8g.pending"));
    EXPECT_FALSE(parsePendingMarkerName(Hash + ".fb24e3f468aad4a6.pending.tmp-abc"));
    EXPECT_FALSE(parsePendingMarkerName(std::string(63, 'a') + ".fb24e3f468aad4a6.pending"));
}

}  // namespace
