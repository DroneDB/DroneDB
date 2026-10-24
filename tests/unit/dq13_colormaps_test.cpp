/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-13: the colormaps moved from 265 lines of code into a table. Each keeps its name, its place in the
// list, whether clients can pick it, and every one of its 256 entries (a hash of them, recorded from the
// code before the move).

#include <cstdint>
#include <iterator>
#include <string>

#include "gtest/gtest.h"
#include "json.h"
#include "vegetation.h"

namespace
{
    using namespace ddb;

    uint64_t fnv1a(const Colormap &c)
    {
        uint64_t h = 1469598103934665603ULL;
        for (const auto &e : c.entries)
            for (unsigned v : {unsigned{e.r}, unsigned{e.g}, unsigned{e.b}, unsigned{e.a}})
            {
                h ^= v;
                h *= 1099511628211ULL;
            }
        return h;
    }

    struct Expected
    {
        const char *id;
        const char *name;
        bool listed;
        uint64_t hash;
    };

    TEST(Dq13Colormaps, EveryColormapKeepsItsNameOrderAndEntries)
    {
        const Expected expected[] = {
            {"rdylgn", "Red-Yellow-Green", true, 0x244587b3f7c0ed99ULL},
            {"discrete_ndvi", "Contrast NDVI", true, 0xb1abae782926740dULL},
            {"spectral", "Spectral", true, 0xa6cc836132ef2cf7ULL},
            {"viridis", "Viridis", true, 0x7b53b96533e317c0ULL},
            {"plasma", "Plasma", true, 0x6ffc940a87bb54d0ULL},
            {"inferno", "Inferno", true, 0x816cb71c5b98735bULL},
            {"magma", "Magma", true, 0xeb4b1220d345818fULL},
            {"grayscale", "Grayscale", true, 0x2ff9380f25bdbb83ULL},
            {"ironbow", "Ironbow", true, 0x224e4c36622c5b36ULL},
            {"rainbow", "Rainbow", true, 0xc425ac86013e4dfdULL},
            {"bugn", "Blue-Green", true, 0x49edb8a85e01382bULL},
            {"whitehot", "White Hot", true, 0x2ff9380f25bdbb83ULL},
            {"blackhot", "Black Hot", true, 0xaba423fdee0f7f83ULL},
            {"arctic", "Arctic", true, 0x18829f5e1d4af975ULL},
            {"lava", "Lava", true, 0x5452424fad1d3d7cULL},
            {"terrain", "Terrain", true, 0x1ee792461ddd19ddULL},
            {"greys", "Greys", true, 0xaba423fdee0f7f83ULL},
            {"ryg", "Red-Yellow-Green (pure)", false, 0x2972fb019bf67c9cULL}
        };

        const auto &colormaps = VegetationEngine::instance().getColormaps();
        ASSERT_EQ(colormaps.size(), std::size(expected));
        for (size_t i = 0; i < colormaps.size(); i++)
        {
            EXPECT_EQ(colormaps[i].id, expected[i].id) << i;
            EXPECT_EQ(colormaps[i].name, expected[i].name) << expected[i].id;
            EXPECT_EQ(colormaps[i].listed, expected[i].listed) << expected[i].id;
            EXPECT_EQ(fnv1a(colormaps[i]), expected[i].hash) << expected[i].id;
        }
    }

    TEST(Dq13Colormaps, OnlyTheListedOnesAreOfferedAndTheEngineRampIsNotNamedByRequests)
    {
        const json listed = VegetationEngine::instance().getColormapsJson();
        EXPECT_EQ(listed.size(), 17u);
        for (const auto &c : listed)
            EXPECT_NE(c["id"], "ryg");
        EXPECT_EQ(VegetationEngine::instance().getColormap("ryg"), nullptr);
        EXPECT_NE(VegetationEngine::instance().getColormap("ryg", true), nullptr);
    }
}
