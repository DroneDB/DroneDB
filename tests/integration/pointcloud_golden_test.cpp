/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Golden outputs of the point cloud footprint: the bounds of the cloud in its own CRS
// transformed to WGS84, which depends on the axis order of the transformation.

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

#include "golden_support.h"
#include "entry.h"
#include "pointcloud.h"

namespace
{
    using namespace ddb;

    json describe(const std::string &path)
    {
        PointCloudInfo info;
        if (!getPointCloudInfo(path, info))
            return {{"error", "no info"}};
        return {{"pointCount", info.pointCount},
                {"bounds", info.bounds},
                {"centroid", info.centroid.toWkt()},
                {"polyBounds", info.polyBounds.toWkt()},
                {"projection", golden::fnv1a(info.wktProjection.data(), info.wktProjection.size())}};
    }

    TEST(PointCloudGolden, boundsAndFootprint)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &[key, url, name] : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"brighton", "brighton/point_cloud.laz", "brighton.laz"},
                 {"toledo", "point-clouds/toledo.laz", "toledo.laz"}})
        {
            const fs::path pc = ta.downloadTestAsset(testDataUrl(url), name);
            json one = describe(pc.string());
            Entry e;
            parseEntry(pc, ta.getFolder(), e, false);
            one["entryPoint"] = e.point_geom.toWkt();
            one["entryPolygon"] = e.polygon_geom.toWkt();
            out[key] = one;
        }
        golden::expectGolden("pointcloud_bounds", out);
    }
}
