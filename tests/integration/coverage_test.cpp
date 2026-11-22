/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The search of the images that see a place, on the images of a real flight (DJI_0018..DJI_0035) that the
// index holds with their footprints and cameras

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "coverage.h"
#include "dbops.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

namespace
{
    using namespace ddb;

    class CoverageFlight : public ::testing::Test
    {
    protected:
        std::unique_ptr<TestArea> ta;
        std::unique_ptr<Database> db;
        std::vector<Entry> entries;

        void SetUp() override
        {
            ta = std::make_unique<TestArea>(TEST_NAME, true);
            std::vector<std::string> files;
            for (int number = 18; number <= 35; number++)
            {
                const std::string name = "DJI_00" + std::to_string(number) + ".JPG";
                files.push_back(ta->downloadTestAsset(testDataUrl("images/" + name), name).string());
            }
            initIndex(ta->getFolder().string());
            db = open(ta->getFolder().string(), true);
            addToIndex(db.get(), files);

            for (const auto &name : imageNames())
            {
                auto entry = getEntry(db.get(), name);
                ASSERT_TRUE(entry.has_value()) << name;
                entries.push_back(std::move(*entry));
            }
            ASSERT_EQ(entries.size(), 18u);
        }

        static std::vector<std::string> imageNames()
        {
            std::vector<std::string> paths;
            for (int number = 18; number <= 35; number++)
                paths.push_back("DJI_00" + std::to_string(number) + ".JPG");
            return paths;
        }

        // The polygon that is the box of the camera positions of some of the images
        CoverageGeometry boxOver(const std::vector<std::string> &names) const
        {
            double minX = 180, maxX = -180, minY = 90, maxY = -90;
            for (const auto &entry : entries)
            {
                if (std::find(names.begin(), names.end(), entry.path) == names.end())
                    continue;
                const Point p = entry.point_geom.points.front();
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            CoverageGeometry geometry;
            geometry.kind = CoverageGeometry::Kind::Polygon;
            geometry.parts.push_back({Point(minX, minY), Point(maxX, minY), Point(maxX, maxY), Point(minX, maxY), Point(minX, minY)});
            return geometry;
        }
    };

    // A polygon over several images, a limit of 3: the 3 with the lowest score, in ascending order
    TEST_F(CoverageFlight, LimitKeepsTheLowestScores)
    {
        const auto polygon = boxOver({"DJI_0018.JPG", "DJI_0019.JPG", "DJI_0020.JPG", "DJI_0021.JPG", "DJI_0022.JPG", "DJI_0023.JPG"});

        CoverageOptions all;
        all.limit = CoverageOptions::MaxLimit;
        const auto everything = findCoverage(db.get(), polygon, all);
        ASSERT_GE(everything.size(), 5u);
        EXPECT_TRUE(std::is_sorted(everything.begin(), everything.end(), [](const CoverageMatch &a, const CoverageMatch &b)
                                   { return a.score != b.score ? a.score < b.score : a.path < b.path; }));

        CoverageOptions three;
        three.limit = 3;
        const auto best = findCoverage(db.get(), polygon, three);
        ASSERT_EQ(best.size(), 3u);
        for (size_t i = 0; i < 3; i++)
        {
            EXPECT_EQ(best[i].path, everything[i].path);
            EXPECT_EQ(best[i].score, everything[i].score);
            EXPECT_GT(best[i].overlap, 0.0);
            EXPECT_LE(best[i].overlap, 1.0);
        }
    }

    // Every image intersects its own camera position: the camera of an image is at the middle of its footprint
    // for a nadir camera, so the image is the best for that point, and the pixel is near the middle of the image
    TEST_F(CoverageFlight, TheCameraPositionIsTheMiddleOfItsImage)
    {
        int nadir = 0;
        for (const auto &entry : entries)
        {
            if (entry.properties.value("cameraPitch", 0.0) > -89.0)
                continue;
            nadir++;
            const Point at = entry.point_geom.points.front();
            CoverageGeometry point;
            point.kind = CoverageGeometry::Kind::Point;
            point.parts.push_back({Point(at.x, at.y)});

            const auto matches = findCoverage(db.get(), point, {});
            ASSERT_FALSE(matches.empty()) << entry.path;
            EXPECT_EQ(matches.front().path, entry.path);
            EXPECT_LT(matches.front().score, 0.05) << entry.path;
            ASSERT_TRUE(matches.front().pixel.has_value()) << entry.path;
            const int width = entry.properties["width"].get<int>();
            const int height = entry.properties["height"].get<int>();
            // The camera sits at a position that the index keeps to 0.1 m
            EXPECT_NEAR(matches.front().pixel->first, width / 2.0, width * 0.02) << entry.path;
            EXPECT_NEAR(matches.front().pixel->second, height / 2.0, height * 0.02) << entry.path;
        }
        EXPECT_GT(nadir, 0);
    }

    TEST_F(CoverageFlight, AGeometryAwayFromTheFlightIsEmpty)
    {
        CoverageGeometry point;
        point.kind = CoverageGeometry::Kind::Point;
        point.parts.push_back({Point(0.0, 0.0)});
        EXPECT_TRUE(findCoverage(db.get(), point, {}).empty());
    }

    // The same through the C API, with the limits of the option
    TEST_F(CoverageFlight, CApi)
    {
        DDBRegisterProcess(false);
        const Point at = entries.front().point_geom.points.front();
        const std::string geometry = json{{"type", "Point"}, {"coordinates", {at.x, at.y}}}.dump();

        char *out = nullptr;
        ASSERT_EQ(DDBFindCoverage(ta->getFolder().string().c_str(), geometry.c_str(), R"({"limit":3})", &out), DDBERR_NONE)
            << DDBGetLastError();
        const auto result = json::parse(out);
        DDBFree(out);
        ASSERT_GE(result.size(), 1u);
        EXPECT_LE(result.size(), 3u);
        EXPECT_EQ(result[0]["path"], entries.front().path);

        for (const char *options : {R"({"limit":0})", R"({"limit":101})"})
        {
            out = nullptr;
            EXPECT_EQ(DDBFindCoverage(ta->getFolder().string().c_str(), geometry.c_str(), options, &out), DDBERR_INVALIDARGS) << options;
            EXPECT_EQ(out, nullptr);
        }
    }
}
