/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A tile written to a file appears whole or not at all. The tile cache serves any file that
// exists (getFromUserCache), and requests for the same tile run concurrently, so a tile encoded
// straight to its final path could be read while it was still being written, or stay truncated
// when the write failed. The encoder writes into a sibling and renames it onto the path.

#include <fstream>
#include <random>
#include <string>

#include "gtest/gtest.h"
#include "cpl_vsi.h"
#include "file_size_limit.h"
#include "mio.h"
#include "testarea.h"
#include "tile_encoder.h"

namespace
{
    using namespace ddb;

    // Noise: its PNG is far larger than SmallFileLimit
    RasterImage noiseImage()
    {
        RasterImage image(256, 256, 3, false);
        std::mt19937 rng(42);
        for (auto &v : image.color)
            v = static_cast<uint8_t>(rng() & 0xFF);
        return image;
    }

    std::string readAll(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    size_t filesIn(const fs::path &folder)
    {
        size_t n = 0;
        for ([[maybe_unused]] const auto &e : fs::directory_iterator(folder))
            n++;
        return n;
    }

    TEST(TileEncoderPublish, aFailedWriteLeavesNoTileAtThePath)
    {
        if (!FileSizeLimit::supported)
            GTEST_SKIP() << "RLIMIT_FSIZE is POSIX only";
        TestArea area("TileEncoderPublish.failedWrite");
        const fs::path tile = area.getPath("tile.png");
        {
            const FileSizeLimit limit(SmallFileLimit);
            EXPECT_ANY_THROW(TileEncoder::write(noiseImage(), {}, io::toUtf8(tile)));
        }
        EXPECT_FALSE(fs::exists(tile)) << "a truncated tile would be served from the cache";
        EXPECT_EQ(filesIn(tile.parent_path()), 0u) << "no temporary sibling is left behind";
    }

    TEST(TileEncoderPublish, aFailedWriteKeepsThePreviousTile)
    {
        if (!FileSizeLimit::supported)
            GTEST_SKIP() << "RLIMIT_FSIZE is POSIX only";
        TestArea area("TileEncoderPublish.keepPrevious");
        const fs::path tile = area.getPath("tile.png");
        TileEncoder::write(noiseImage(), {}, io::toUtf8(tile));
        const std::string previous = readAll(tile);
        ASSERT_GT(previous.size(), SmallFileLimit);
        {
            const FileSizeLimit limit(SmallFileLimit);
            RasterImage other = noiseImage();
            other.color[0] ^= 0xFF;
            EXPECT_ANY_THROW(TileEncoder::write(other, {}, io::toUtf8(tile)));
        }
        const std::string after = readAll(tile);
        EXPECT_TRUE(after == previous) << "the previous tile (" << previous.size() << " bytes) became " << after.size() << " bytes";
        EXPECT_EQ(filesIn(tile.parent_path()), 1u);
    }

    TEST(TileEncoderPublish, aWrittenTileIsTheEncodedImage)
    {
        TestArea area("TileEncoderPublish.written");
        const fs::path tile = area.getPath("tile.png");
        const RasterImage image = noiseImage();
        TileEncoder::write(image, {}, io::toUtf8(tile));

        uint8_t *buffer = nullptr;
        int size = 0;
        TileEncoder::encode(image, {}, &buffer, &size);
        const std::string encoded(reinterpret_cast<const char *>(buffer), static_cast<size_t>(size));
        VSIFree(buffer);
        EXPECT_TRUE(readAll(tile) == encoded);
        EXPECT_EQ(filesIn(tile.parent_path()), 1u);
    }
}
