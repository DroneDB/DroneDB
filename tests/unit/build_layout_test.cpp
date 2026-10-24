/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// build_layout.h names the on-disk build layout once. Registry, the Hub and the
// datasets already on disk depend on these exact bytes, so every value is pinned here with a
// literal: changing one is a layout migration, never a rename.

#include "build_layout.h"
#include "gtest/gtest.h"

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
    EXPECT_STREQ(NexusBuildSubfolder, "nxs");
    EXPECT_STREQ(VectorBuildSubfolder, "vec");
    EXPECT_STREQ(MvtBuildSubfolder, "mvt");
    EXPECT_STREQ(GsplatBuildSubfolder, "gsplat");
    EXPECT_STREQ(Tiles3DBuildSubfolder, "3dtiles");
    EXPECT_STREQ(LegacyEptBuildSubfolder, "ept");
}

TEST(buildLayout, ArtifactFileNamesArePinned) {
    EXPECT_STREQ(CogFileName, "cog.tif");
    EXPECT_STREQ(CopcFileName, "cloud.copc.laz");
    EXPECT_STREQ(NexusFileName, "model.nxz");
    EXPECT_STREQ(VectorGpkgFileName, "source.gpkg");
    EXPECT_STREQ(MvtMetadataFileName, "metadata.json");
    EXPECT_STREQ(VectorLocalFileName, "local.json");
    EXPECT_STREQ(TilesetFileName, "tileset.json");
    EXPECT_STREQ(GsplatFileName, "model.spz");
    EXPECT_STREQ(GsplatRadFileName, "model.rad");
    EXPECT_STREQ(GsplatBoundsFileName, "bounds.json");
    EXPECT_STREQ(GsplatThumbFileName, "thumbnail.webp");
    EXPECT_STREQ(LegacyEptFileName, "ept.json");
    EXPECT_STREQ(LegacyEptDataFolder, "ept-data");
    EXPECT_STREQ(LegacyEptHierarchyFolder, "ept-hierarchy");
}

TEST(buildLayout, ComposedNamesAreByteIdentical) {
    EXPECT_EQ(buildLockPath("b/0123/cog"), "b/0123/cog.building");
    EXPECT_EQ(buildLockPath(""), ".building");
    EXPECT_EQ(pendingMarkerPath(fs::path("b") / "0123"), fs::path("b") / "0123.pending");
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

}  // namespace
