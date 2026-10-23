/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: the small geo and raster primitives (angle factors, nodata rule, GeoTIFF options, DEM
// opening, coordinate transformer axis order) on their own.

#include <cmath>

#include <memory>

#include "gtest/gtest.h"

#include "coordstransformer.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "mathutil.h"

namespace
{
    using namespace ddb;

    // DQ-05 left the private `kPi / 180.0` factors of stockpile, volume and raster_profile alone
    // because deg2rad(x) = (x * kPi) / 180.0 rounds differently. DQ-06 replaces them with
    // kDegToRad, which is that same factor, so no output changes; and the expressions that were
    // written as (x * kPi) / 180.0 or (x * 180.0) / kPi became deg2rad / rad2deg.
    TEST(Dq06Math, theTwoWaysToConvertDegreesDifferInTheLastBit)
    {
        int differing = 0;
        for (int i = 0; i <= 3600; i++)
        {
            const double degrees = i * 0.1;
            if (degrees * kDegToRad != deg2rad(degrees))
                differing++;
        }
        EXPECT_GT(differing, 0) << "if this ever fails the two forms agree and kDegToRad is redundant";
        EXPECT_EQ(kDegToRad, kPi / 180.0);
        EXPECT_EQ(kRadToDeg, 180.0 / kPi);
    }

    TEST(Dq06Math, deg2radAndRad2degAreTheExpressionsTheyReplaced)
    {
        for (int i = -3600; i <= 3600; i++)
        {
            const double x = i * 0.1234567;
            EXPECT_EQ(deg2rad(x), x * kPi / 180.0);  // merge_multispectral, tiler
            EXPECT_EQ(rad2deg(x), x * 180.0 / kPi);  // align
            EXPECT_EQ(kRadToDeg * x, 180.0 / kPi * x); // tiler
        }
    }

    // ---- coordinate transformer -----------------------------------------------------------

    // The process-wide OGR_CT_FORCE_TRADITIONAL_GIS_ORDER setting, for this thread only
    class ThreadForceOrder
    {
        const char *value_;

    public:
        explicit ThreadForceOrder(const char *value) : value_(value)
        {
            CPLSetThreadLocalConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", value);
        }
        ~ThreadForceOrder() { CPLSetThreadLocalConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", nullptr); }
    };

    // The transformer gives lon/lat in, easting/northing out whatever the global setting: it sets
    // the axis order of its two ends itself (DQ-06 removed the global from process_init)
    TEST(Dq06Coords, theTransformerDoesNotNeedTheGlobalAxisOrderSetting)
    {
        for (const char *setting : {"NO", "YES"})
        {
            SCOPED_TRACE(setting);
            const ThreadForceOrder force(setting);

            double x = 10.0, y = 45.0;
            CoordsTransformer(4326, 4258).transform(&x, &y);
            EXPECT_NEAR(x, 10.0, 1e-6);
            EXPECT_NEAR(y, 45.0, 1e-6);

            x = 10.0;
            y = 45.0;
            CoordsTransformer(4326, 3035).transform(&x, &y);
            EXPECT_NEAR(x, 4321000.0 + 0.0, 4e5) << "easting first";
            EXPECT_LT(y, 3000000.0);

            x = 4300000.0;
            y = 2500000.0;
            CoordsTransformer(3035, 4326).transform(&x, &y);
            EXPECT_NEAR(x, 9.7, 0.1);
            EXPECT_NEAR(y, 45.6, 0.1);
        }
    }

    // The raw GDAL object is what the setting changes: without it, the same two SRSs from EPSG
    // codes take latitude first. This is the dependency the transformer removes.
    TEST(Dq06Coords, rawGdalTransformsDependOnTheGlobalSetting)
    {
        OGRSpatialReference wgs84, laea;
        wgs84.importFromEPSG(4326);
        laea.importFromEPSG(3035);

        double x = 45.0, y = 10.0; // latitude first, what the authority order says
        {
            const ThreadForceOrder force("NO");
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&wgs84, &laea));
            ASSERT_NE(ct, nullptr);
            ASSERT_TRUE(ct->Transform(1, &x, &y));
            EXPECT_GT(y, 4000000.0) << "authority order in, northing first out";
        }
        x = 10.0;
        y = 45.0;
        {
            const ThreadForceOrder force("YES");
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&wgs84, &laea));
            ASSERT_NE(ct, nullptr);
            ASSERT_TRUE(ct->Transform(1, &x, &y));
            EXPECT_GT(x, 4000000.0);
        }
    }

    TEST(Dq06Coords, theSrsPassedInAreCopiedNotChanged)
    {
        OGRSpatialReference wgs84, laea;
        wgs84.importFromEPSG(4326);
        laea.importFromEPSG(3035);
        ASSERT_EQ(wgs84.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);

        const CoordsTransformer t(wgs84, laea);
        EXPECT_EQ(wgs84.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);
        EXPECT_EQ(laea.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);
        EXPECT_NE(t.get(), nullptr);
    }

    TEST(Dq06Coords, tryCreateReportsAMissingTransformationWithoutThrowing)
    {
        OGRSpatialReference local, wgs84;
        local.SetLocalCS("site");
        wgs84.importFromEPSG(4326);
        EXPECT_EQ(CoordsTransformer::tryCreate(local, wgs84), nullptr);
        EXPECT_THROW(CoordsTransformer(local, wgs84), GDALException);
        EXPECT_NE(CoordsTransformer::tryCreate(wgs84, wgs84), nullptr);
    }

    TEST(Dq06Coords, tryTransformReportsAFailedPoint)
    {
        CoordsTransformer toUtm(4326, 32632);
        double x[2] = {10.0, 10.0}, y[2] = {45.0, 45.0};
        EXPECT_TRUE(toUtm.tryTransform(2, x, y));
        EXPECT_NEAR(x[0], 578000.0, 2000.0);

        double bad = 1e30, alsoBad = 1e30;
        EXPECT_FALSE(toUtm.tryTransform(1, &bad, &alsoBad));
        EXPECT_TRUE(toUtm.tryTransform(0, nullptr, nullptr)) << "nothing to transform is a success";
    }

    TEST(Dq06Coords, srsFactoriesGiveTheTraditionalOrder)
    {
        OGRSpatialReference authority;
        authority.importFromEPSG(4326);
        char *wkt = nullptr;
        authority.exportToWkt(&wkt);
        const std::string wgs84Wkt = takeGdalString(wkt);

        SrsPtr made[] = {srsFromEpsg(4258), srsFromUserInput("EPSG:4258"),
                         srsFromProj4("+proj=longlat +datum=WGS84 +no_defs"), srsFromWkt(wgs84Wkt.c_str())};
        for (const auto &srs : made)
        {
            ASSERT_TRUE(srs);
            EXPECT_EQ(srsOf(srs).GetAxisMappingStrategy(), OAMS_TRADITIONAL_GIS_ORDER);
        }
        EXPECT_FALSE(srsFromUserInput("not a crs"));
        EXPECT_FALSE(srsFromUserInput(""));
        EXPECT_FALSE(srsFromProj4(""));
        EXPECT_FALSE(srsFromProj4(nullptr));
    }
}
