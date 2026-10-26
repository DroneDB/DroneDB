/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// How much of a model's texture a Nexus file keeps. A Nexus node holds one texture for all its
// triangles, so a model with few triangles and large textures (what photogrammetry produces)
// keeps its resolution only when the nodes are cut by the texels they cover, at every level,
// and not by their triangle count alone. These tests build a flat grid textured by one large
// picture and read the node textures back from the file.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "3d.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "nexus_file_support.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;
    using testutils::NexusFile;

    // The longest side libnexus gives a node texture
    constexpr int maxNodeTextureSide = 1024;

    // A flat cells x cells grid of squares, two triangles each, covered once by the texture
    void writeGrid(const fs::path &folder, int cells, const std::string &texture)
    {
        std::ofstream obj(folder / "grid.obj");
        obj << "mtllib grid.mtl\n";
        for (int y = 0; y <= cells; y++)
            for (int x = 0; x <= cells; x++)
                obj << "v " << x << " " << y << " 0\n";
        for (int y = 0; y <= cells; y++)
            for (int x = 0; x <= cells; x++)
                obj << "vt " << double(x) / cells << " " << double(y) / cells << "\n";
        obj << "usemtl m\n";
        auto index = [cells](int x, int y) { return y * (cells + 1) + x + 1; };
        for (int y = 0; y < cells; y++)
            for (int x = 0; x < cells; x++)
            {
                const int a = index(x, y), b = index(x + 1, y), c = index(x + 1, y + 1), d = index(x, y + 1);
                obj << "f " << a << "/" << a << " " << b << "/" << b << " " << c << "/" << c << "\n";
                obj << "f " << a << "/" << a << " " << c << "/" << c << " " << d << "/" << d << "\n";
            }
        std::ofstream(folder / "grid.mtl") << "newmtl m\nKd 1 1 1\nmap_Kd " << texture << "\n";
    }

    void writeTexture(const fs::path &file, int side, std::function<double(int band, int x, int y)> value)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = side;
        spec.value = std::move(value);
        testutils::createGeoRaster(file, spec);
    }

    fs::path buildGrid(TestArea &ta, int cells, int textureSide, std::function<double(int, int, int)> value)
    {
        writeTexture(ta.getPath("texture.tif"), textureSide, std::move(value));
        writeGrid(ta.getPath("."), cells, "texture.tif");
        const fs::path out = ta.getPath("grid.nxs");
        EXPECT_NO_THROW(buildNexus(io::toUtf8(ta.getPath("grid.obj")), io::toUtf8(out), true));
        return out;
    }

    double detail(int band, int x, int y)
    {
        return double((x * 7 + y * 13 + band * 50) % 256);
    }

    struct TexelCount
    {
        double leaves = 0;
        double inner = 0;
        int largestSide = 0;
    };

    TexelCount countTexels(const NexusFile &n)
    {
        TexelCount c;
        std::vector<testutils::DecodedImage> images;
        for (const auto &t : n.textures)
            images.push_back(testutils::decodeImage(t));
        std::vector<bool> counted(images.size(), false);
        for (size_t node = 0; node < n.nodeTexture.size(); node++)
        {
            const int t = n.nodeTexture[node];
            if (t < 0 || size_t(t) >= images.size() || counted[size_t(t)])
                continue;
            counted[size_t(t)] = true;
            const double texels = double(images[size_t(t)].width) * images[size_t(t)].height;
            (n.leaf[node] ? c.leaves : c.inner) += texels;
            c.largestSide = std::max({c.largestSide, images[size_t(t)].width, images[size_t(t)].height});
        }
        return c;
    }

    TEST(NexusTextureResolution, theNodeTexturesAreWebp)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path out = buildGrid(ta, 4, 64, detail);
        const NexusFile n = testutils::readNexusFile(out);
        ASSERT_FALSE(n.textures.empty());
        for (const auto &t : n.textures)
            EXPECT_TRUE(testutils::isWebp(t));
    }

    // 2048 triangles over 16 million texels: each node must cover only a few hundred triangles
    // for its texels to fit in one node texture, far fewer than the triangles a node holds
    TEST(NexusTextureResolution, theLeavesKeepTheResolutionOfAFewTrianglesWithALargeTexture)
    {
        TestArea ta(TEST_NAME, true);
        const int side = 4096;
        const NexusFile n = testutils::readNexusFile(buildGrid(ta, 32, side, detail));
        const TexelCount c = countTexels(n);
        EXPECT_GE(c.leaves, 0.9 * side * side) << "the leaves hold " << c.leaves << " texels";
        EXPECT_LE(c.largestSide, maxNodeTextureSide);
    }

    // A leaf is as precise as its texels: the viewer refines down to it only once a texel covers
    // enough of the screen. Measured by the length of its triangles' edges, as Nexus does for a
    // node about to be simplified, the error would be a third of a grid square (over 40 texels),
    // and the viewer would load the finest textures far before it could show them.
    TEST(NexusTextureResolution, theErrorOfALeafIsTheSizeOfItsTexels)
    {
        TestArea ta(TEST_NAME, true);
        const int cells = 32, side = 4096;
        const NexusFile n = testutils::readNexusFile(buildGrid(ta, cells, side, detail));
        const double texel = double(cells) / side; // the grid squares have a side of 1
        bool sawLeaf = false;
        for (size_t node = 0; node < n.leaf.size(); node++)
        {
            if (!n.leaf[node])
                continue;
            sawLeaf = true;
            EXPECT_LT(n.error[node], 3 * texel) << "node " << node;
        }
        EXPECT_TRUE(sawLeaf);
    }

    // A texture far finer than the triangles is made coarser for several levels before the
    // triangles are simplified: the nodes cut small to keep their textures in size would
    // otherwise be simplified into fans around a few vertices. The levels that keep the
    // triangles are all made, however few the triangles, so the top carries a coarse texture.
    TEST(NexusTextureResolution, theCoarserLevelsKeepTheTrianglesWhileTheTextureIsFinerThanThem)
    {
        TestArea ta(TEST_NAME, true);
        const int cells = 32, triangles = 2 * cells * cells;
        const NexusFile n = testutils::readNexusFile(buildGrid(ta, cells, 4096, detail));
        EXPECT_GE(n.nface, uint64_t(4 * triangles)) << "the levels hold " << n.nface << " triangles";
        const TexelCount c = countTexels(n);
        EXPECT_LT(c.inner, c.leaves) << "the coarser levels hold " << c.inner << " texels";
    }

    // The coarser levels hold a third of the texels of the leaves: the levels that only make a
    // texture finer than its triangles coarser halve its side, as mipmaps do, and the simplified
    // levels above them keep the texels of their triangles. Cut by triangles alone, a coarser
    // node would get the texels of many leaves, more than its texture holds; with levels that
    // halve the texels instead of the side, the coarser levels would hold as many as the leaves.
    TEST(NexusTextureResolution, theCoarserLevelsHoldAThirdOfTheTexelsOfTheLeaves)
    {
        TestArea ta(TEST_NAME, true);
        const int side = 4096;
        const NexusFile n = testutils::readNexusFile(buildGrid(ta, 72, side, detail));
        const TexelCount c = countTexels(n);
        EXPECT_GE(c.leaves, 0.9 * side * side) << "the leaves hold " << c.leaves << " texels";
        EXPECT_GE(c.inner, 0.2 * c.leaves) << "the coarser levels hold " << c.inner << " texels";
        EXPECT_LE(c.inner, 0.45 * c.leaves) << "the coarser levels hold " << c.inner << " texels";
        EXPECT_LE(c.largestSide, maxNodeTextureSide);
    }

    // The texels of a node texture that no triangle draws are one flat color, past a margin of a
    // few texels along the triangles: they hold whatever the model's texture had around the
    // triangles, and cost bytes. One triangle over the lower left half of a detailed texture
    // keeps the detail there and nothing in the upper right half (rows run from v = 1 down).
    TEST(NexusTextureResolution, theTexelsNoTriangleDrawsAreBlank)
    {
        TestArea ta(TEST_NAME, true);
        writeTexture(ta.getPath("texture.tif"), 256, detail);
        std::ofstream(ta.getPath("half.obj")) << "mtllib half.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                               << "vt 0 0\nvt 1 0\nvt 0 1\nusemtl m\nf 1/1 2/2 3/3\n";
        std::ofstream(ta.getPath("half.mtl")) << "newmtl m\nKd 1 1 1\nmap_Kd texture.tif\n";
        const fs::path out = ta.getPath("half.nxs");
        ASSERT_NO_THROW(buildNexus(io::toUtf8(ta.getPath("half.obj")), io::toUtf8(out), true));
        const NexusFile n = testutils::readNexusFile(out);
        ASSERT_FALSE(n.textures.empty());
        const testutils::DecodedImage img = testutils::decodeImage(n.textures.front());
        ASSERT_GE(img.width, 64);
        ASSERT_GE(img.height, 64);

        auto deviation = [&img](bool drawnHalf) {
            double sum = 0, squares = 0, count = 0;
            for (int y = 0; y < img.height; y++)
                for (int x = 0; x < img.width; x++)
                {
                    const double u = (x + 0.5) / img.width, v = 1.0 - (y + 0.5) / img.height;
                    if (drawnHalf ? u + v > 0.85 : u + v < 1.15)
                        continue;
                    const double p = img.bands[0][size_t(y) * size_t(img.width) + size_t(x)];
                    sum += p;
                    squares += p * p;
                    count++;
                }
            const double mean = sum / count;
            return std::sqrt(std::max(0.0, squares / count - mean * mean));
        };
        EXPECT_LT(deviation(false), 2.0) << "the half no triangle draws is not blank";
        EXPECT_GT(deviation(true), 20.0) << "the half the triangle draws lost its detail";
    }

    // The node textures are encoded on several threads and written in the order of the nodes:
    // each node gets its own texture. Over a texture whose red grows with u and green with v,
    // the mean color of each node's texture tells where the node lies on the grid.
    TEST(NexusTextureResolution, eachNodeHasTheTextureOfItsPlace)
    {
        TestArea ta(TEST_NAME, true);
        const int cells = 72, side = 2048;
        // bands count from 1, and texture rows run from v = 1 at the top
        const NexusFile n = testutils::readNexusFile(buildGrid(ta, cells, side, [side](int band, int x, int y) {
            return band == 1 ? 255.0 * x / side : band == 2 ? 255.0 * (side - 1 - y) / side : 128.0;
        }));
        ASSERT_GT(n.textures.size(), 4u);
        for (size_t node = 0; node < n.nodeTexture.size(); node++)
        {
            const testutils::DecodedImage img = testutils::decodeImage(n.textures[size_t(n.nodeTexture[node])]);
            double red = 0, green = 0;
            for (size_t i = 0; i < img.bands[0].size(); i++)
            {
                red += img.bands[0][i];
                green += img.bands[1][i];
            }
            red /= double(img.bands[0].size());
            green /= double(img.bands[1].size());
            EXPECT_NEAR(red, 255.0 * n.center[node][0] / cells, 30.0) << "node " << node;
            EXPECT_NEAR(green, 255.0 * n.center[node][1] / cells, 30.0) << "node " << node;
        }
    }

    // The texture of a coarser level is the average of the one below: a checkerboard of single
    // pixels becomes a gray, not a scattering of the black and white pixels a nearest neighbour
    // resize picks
    TEST(NexusTextureResolution, aCoarserTextureAveragesTheDetailsOfTheFinerOne)
    {
        TestArea ta(TEST_NAME, true);
        const NexusFile n =
            testutils::readNexusFile(buildGrid(ta, 72, 1024, [](int, int x, int y) { return ((x + y) % 2) ? 255.0 : 0.0; }));
        bool sawInner = false;
        for (size_t node = 0; node < n.nodeTexture.size(); node++)
        {
            if (n.leaf[node])
                continue;
            sawInner = true;
            const auto &t = n.textures[size_t(n.nodeTexture[node])];
            const testutils::DecodedImage img = testutils::decodeImage(t);
            const std::vector<uint8_t> &red = img.bands[0];
            double sum = 0, squares = 0;
            for (uint8_t v : red)
            {
                sum += v;
                squares += double(v) * v;
            }
            const double count = double(red.size());
            const double mean = sum / count;
            const double deviation = std::sqrt(std::max(0.0, squares / count - mean * mean));
            EXPECT_NEAR(mean, 127.5, 20.0) << "node " << node;
            EXPECT_LT(deviation, 60.0) << "node " << node;
        }
        EXPECT_TRUE(sawInner) << "the model has no coarser level";
    }
} // namespace
