/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The sample of a point cloud that the raster helper writes for the training export of Gaussian
// splats and the host reads: its file, and the gate that keeps the cloud of a user out of the host

#include <fstream>
#include <stdexcept>
#include <vector>

#include "gtest/gtest.h"
#include "cloud_sample.h"
#include "exceptions.h"
#include "mio.h"
#include "raster_protocol.h"
#include "test.h"
#include "testarea.h"
#include "user_open_child.h"

namespace
{
    using namespace ddb;

    TEST(CloudSample, aSampleSurvivesItsFile)
    {
        TestArea ta(TEST_NAME, true);
        const std::vector<SampledPoint> points = {{1.5, -2.25, 1e7, 0, 255, 65535}, {-0.0, 3e-9, -4.0, 1, 2, 3}};
        const fs::path file = ta.getPath("sample.bin");
        writeCloudSample(file, points);
        const auto back = readCloudSample(file);
        ASSERT_EQ(back.size(), points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            EXPECT_EQ(back[i].x, points[i].x);
            EXPECT_EQ(back[i].y, points[i].y);
            EXPECT_EQ(back[i].z, points[i].z);
            EXPECT_EQ(back[i].r, points[i].r);
            EXPECT_EQ(back[i].g, points[i].g);
            EXPECT_EQ(back[i].b, points[i].b);
        }
        writeCloudSample(file, {});
        EXPECT_TRUE(readCloudSample(file).empty());
    }

    TEST(CloudSample, aFileThatIsNotAWholeSampleIsRefused)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("sample.bin");
        writeCloudSample(file, {{1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12}});
        const auto size = fs::file_size(file);
        fs::resize_file(file, size - 1);
        EXPECT_THROW(readCloudSample(file), AppException) << "a point cut short";
        writeCloudSample(file, {{1, 2, 3, 4, 5, 6}});
        {
            std::ofstream more(file, std::ios::binary | std::ios::app);
            more << "garbage";
        }
        EXPECT_THROW(readCloudSample(file), AppException) << "bytes after the points";
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "LASF not a sample at all";
        EXPECT_THROW(readCloudSample(file), AppException);
        EXPECT_THROW(readCloudSample(ta.getPath("missing.bin")), AppException);
    }

    TEST(CloudSample, theHostDoesNotReadTheCloudOfAUserAndTheHelperDoes)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path cloud = ta.getPath("cloud.xyz");
        std::ofstream(cloud) << "10 20 30\n11 21 31\n";
        CloudSampleParams params;
        params.shift = {10, 20, 30};
        params.limit = 10;
        EXPECT_THROW(sampleCloudFile(cloud, params, {}), std::logic_error);
        testutils::runWithUserFileOpens([&] {
            std::vector<double> fractions;
            const auto points = sampleCloudFile(cloud, params, [&](double f) { fractions.push_back(f); });
            ASSERT_EQ(points.size(), 2u);
            EXPECT_EQ(points[0].x, 0.0);
            EXPECT_EQ(points[1].z, 1.0);
            ASSERT_FALSE(fractions.empty());
            EXPECT_EQ(fractions.back(), 1.0);
        });
    }

    TEST(CloudSample, theParamsSurviveTheProtocolAndBadOnesAreRefused)
    {
        CloudSampleParams params;
        params.shift = {576000.5, -5188000.25, 12.0};
        params.limit = 200000;
        const auto back = raster::cloudSampleParamsFromJson(json::parse(raster::toJson(params).dump()));
        EXPECT_EQ(back.shift, params.shift);
        EXPECT_EQ(back.limit, params.limit);
        for (const json &bad : {json::array(), json{{"limit", 1}}, json{{"shift", {1, 2}}, {"limit", 1}},
                                json{{"shift", {1, 2, "3"}}, {"limit", 1}}, json{{"shift", {1, 2, 3}}},
                                json{{"shift", {1, 2, 3}}, {"limit", -1}}, json{{"shift", {1, 2, 3}}, {"limit", 1.5}}})
            EXPECT_THROW(raster::cloudSampleParamsFromJson(bad), raster::ProtocolError) << bad.dump();
    }
}
