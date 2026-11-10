/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "entry_handler.h"
#include "stac.h"

namespace
{

    using namespace ddb;

    // FR-DEV-15: the media type of a build artifact comes from the declaration of its entry handler, so a
    // plain .laz is no longer COPC
    TEST(stacMediaType, theCopcArtifactIsCopc)
    {
        EXPECT_EQ(stacArtifactMediaType("copc/cloud.copc.laz"), "application/vnd.laszip+copc");
        EXPECT_EQ(stacArtifactMediaType("5d41402abc4b/copc/cloud.copc.laz"), "application/vnd.laszip+copc");
        EXPECT_EQ(stacArtifactMediaType("5d41402abc4b/COPC/Cloud.COPC.laz"), "application/vnd.laszip+copc");
    }

    // The declared types are those of the files of a build folder only: the entries of a user, whatever
    // their folder is called, have the type of their extension
    TEST(stacMediaType, aUserFileInAFolderNamedLikeAnArtifactHasTheTypeOfItsExtension)
    {
        EXPECT_EQ(stacMediaType("ortho/cog/cog.tif"), "image/tiff");
        EXPECT_EQ(stacMediaType("x/copc/cloud.copc.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacMediaType("copc/cloud.copc.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacMediaType("cog/cog.tif"), "image/tiff");
        // The same paths in a build folder are the artifacts
        EXPECT_EQ(stacArtifactMediaType("ortho/cog/cog.tif"), "image/tiff; application=geotiff; profile=cloud-optimized");
        EXPECT_EQ(stacArtifactMediaType("x/copc/cloud.copc.laz"), "application/vnd.laszip+copc");
    }

    TEST(stacMediaType, aPlainLazIsLaszipNotCopc)
    {
        EXPECT_EQ(stacMediaType("tile.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacMediaType("Tile.LAZ"), "application/vnd.laszip");
        // An EPT tile of an uploaded entwine output
        EXPECT_EQ(stacMediaType("entwine_pointcloud/ept-data/0-0-0-0.laz"), "application/vnd.laszip");
        // A file named like the COPC artifact, but not in its place in a build folder
        EXPECT_EQ(stacMediaType("cloud.copc.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacMediaType("elsewhere/cloud.copc.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacArtifactMediaType("cloud.copc.laz"), "application/vnd.laszip");
        EXPECT_EQ(stacArtifactMediaType("tile.laz"), "application/vnd.laszip");
    }

    TEST(stacMediaType, theBuildFilesOfOtherTypesHaveTheirDeclaredTypes)
    {
        EXPECT_EQ(stacMediaType("model/model.bm3d"), "application/vnd.dronedb.bm3d");
        EXPECT_EQ(stacMediaType("scene.bm3d"), "application/vnd.dronedb.bm3d");
        EXPECT_EQ(stacMediaType("vec/source.gpkg"), "application/geopackage+sqlite3");
        EXPECT_EQ(stacMediaType("survey.gpkg"), "application/geopackage+sqlite3");
        EXPECT_EQ(stacArtifactMediaType("cog/cog.tif"), "image/tiff; application=geotiff; profile=cloud-optimized");
        EXPECT_EQ(stacArtifactMediaType("vec/source.gpkg"), "application/geopackage+sqlite3");
        EXPECT_EQ(stacArtifactMediaType("model/model.bm3d"), "application/vnd.dronedb.bm3d");
    }

    // Plain files keep the media type of their extension
    TEST(stacMediaType, plainFilesKeepTheTypeOfTheirExtension)
    {
        EXPECT_EQ(stacMediaType("ortho.tif"), "image/tiff");
        EXPECT_EQ(stacMediaType("cog.tif"), "image/tiff"); // a user file named like the COG artifact
        EXPECT_EQ(stacMediaType("DJI_0018.JPG"), "image/jpeg");
        EXPECT_EQ(stacMediaType("cloud.las"), "application/vnd.las");
        EXPECT_EQ(stacMediaType("area.geojson"), "application/geo+json");
        EXPECT_EQ(stacMediaType("notes.md"), "text/markdown");
        EXPECT_EQ(stacMediaType("unknown.zzz"), "application/octet-stream");
    }

    // Every file artifact that a handler declares has its declared type as a STAC media type
    TEST(stacMediaType, everyDeclaredFileArtifactResolvesToItsType)
    {
        int checked = 0;
        for (const EntryHandler *handler : entryHandlers())
            for (const auto &a : handler->artifacts())
            {
                if (a.kind != ArtifactKind::File) continue;
                EXPECT_EQ(stacArtifactMediaType(a.path), a.mediaType) << a.role << " " << a.path;
                checked++;
            }
        EXPECT_GE(checked, 8);
    }

} // namespace
