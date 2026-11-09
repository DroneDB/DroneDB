/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The integrity label of a published output (Windows). A private folder made for the confined
// raster helper has a low integrity label, and what is made in it keeps the label when it is
// renamed to its place. The writers that never run the helper (mask, merge, bm3d, COPC) stage in
// a folder without it: their outputs are files that a low integrity process cannot change.

#include <fstream>
#include <string>
#include <vector>

#include "3d.h"
#include "build_layout.h"
#include "gtest/gtest.h"
#include "mask.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "pointcloud.h"
#include "raster_artefacts.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

#ifdef _WIN32
#include "sandbox/sandbox.h"
#endif

namespace
{
    using namespace ddb;

#ifdef _WIN32
    // Whether `path` has a low integrity label, its own or inherited; a label that cannot be read
    // fails the test
    bool lowLabelled(const fs::path &path)
    {
        bool low = false;
        std::string error;
        EXPECT_TRUE(sandbox::windows::hasLowIntegrityLabel(io::toUtf8(path), &low, &error)) << path << ": " << error;
        return low;
    }

    // ─── The folders ────────────────────────────────────────────────────────────

    TEST(OutputLabel, onlyAFolderForTheHelperHasTheLowLabel)
    {
        TestArea ta(TEST_NAME, true);
        const io::ScopedTempDir helper = raster::makePrivateFolder(ta.getFolder(), ".helper-");
        const io::ScopedTempDir host =
            raster::makePrivateFolder(ta.getFolder(), ".host-", raster::PrivateFolderWriter::Host);
        EXPECT_TRUE(lowLabelled(helper.path()));
        EXPECT_FALSE(lowLabelled(host.path()));

        // A file made in the folder of the helper keeps the label once it is published: what the
        // checks of the writers below would see without a folder of their own
        std::ofstream(helper.path() / "made.txt") << "made";
        raster::publishArtefact(helper.path() / "made.txt", ta.getPath("published.txt"));
        EXPECT_TRUE(lowLabelled(ta.getPath("published.txt")));
    }

    // ─── The writers that do not run the helper ─────────────────────────────────

    TEST(OutputLabel, aMaskHasNoLowLabel)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        const fs::path input = testutils::createGeoRaster(ta.getPath("ortho.tif"), spec);
        const fs::path output = ta.getPath("masked.tif");
        maskBorders(io::toUtf8(input), io::toUtf8(output));
        ASSERT_TRUE(fs::exists(output));
        EXPECT_FALSE(lowLabelled(output));
    }

    TEST(OutputLabel, aMergeHasNoLowLabel)
    {
        TestArea ta(TEST_NAME, true);
        std::vector<std::string> bands;
        for (int band = 0; band < 2; ++band)
        {
            testutils::GeoRasterSpec spec;
            spec.width = spec.height = 32;
            spec.bands = 1;
            bands.push_back(io::toUtf8(testutils::createGeoRaster(ta.getPath("band" + std::to_string(band) + ".tif"), spec)));
        }
        const fs::path output = ta.getPath("merged.tif");
        mergeMultispectral(bands, io::toUtf8(output));
        ASSERT_TRUE(fs::exists(output));
        EXPECT_FALSE(lowLabelled(output));
    }

    TEST(OutputLabel, aBm3dFileHasNoLowLabel)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path model = ta.getPath("model.obj");
        std::ofstream(model) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const fs::path output = ta.getPath("model.bm3d");
        buildBm3d(io::toUtf8(model), io::toUtf8(output));
        ASSERT_TRUE(fs::exists(output));
        EXPECT_FALSE(lowLabelled(output));
    }

    TEST(OutputLabel, aCopcHasNoLowLabel)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path cloud = ta.getPath("cloud.ply");
        std::ofstream(cloud) << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\n"
                                "property float z\nend_header\n0 0 0\n10 0 1\n10 10 2\n0 10 3\n";
        const fs::path folder = ta.getFolder("copc");
        buildCopc({io::toUtf8(cloud)}, io::toUtf8(folder));
        const fs::path output = folder / CopcFileName;
        ASSERT_TRUE(fs::exists(output));
        EXPECT_FALSE(lowLabelled(output));
    }
#else
    TEST(OutputLabel, aPosixFileHasNoIntegrityLabel)
    {
        GTEST_SKIP() << "integrity labels are Windows only";
    }
#endif
} // namespace
