/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The CRS a COPC is built in (PointCloudCrsPolicy): how the CRS a point cloud states is judged
// against its coordinates, and which projected CRS a geographic or geocentric cloud gets. Every
// row of the policy, from the WKT and the bounds a LAS header gives.

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "coordstransformer.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "pointcloud_crs.h"

namespace
{
    using namespace ddb;

    std::string wktOf(const std::string &userInput)
    {
        const SrsPtr srs = srsFromUserInput(userInput.c_str());
        EXPECT_TRUE(srs) << userInput;
        if (!srs)
            return {};
        char *wkt = nullptr;
        srsOf(srs).exportToWkt(&wkt);
        return takeGdalString(wkt);
    }

    // The bounds of a cloud of `size` degrees around lon, lat, heights from 582 to 730 m
    std::vector<double> degreesAround(double lon, double lat, double size = 0.014)
    {
        return {lon - size / 2, lat - size / 2, 582.0, lon + size / 2, lat + size / 2, 730.0};
    }

    LonLatEnvelope envelopeOf(const std::vector<double> &b)
    {
        return {b[0], b[1], b[3], b[4]};
    }

    // The status and the target of a geographic cloud of these bounds
    std::string targetOf(const std::string &crs, const std::vector<double> &bounds)
    {
        const std::string wkt = wktOf(crs);
        const PointCloudCrsCheck check = PointCloudCrsPolicy::classify(wkt, bounds);
        EXPECT_EQ(check.status, PointCloudCrsStatus::Geographic) << crs;
        EXPECT_TRUE(check.warning.empty());
        return PointCloudCrsPolicy::target(wkt, check.status, envelopeOf(bounds));
    }

    PointCloudCrsStatus PointCloudPolicyStatus(const std::string &wkt, const std::vector<double> &bounds)
    {
        return PointCloudCrsPolicy::classify(wkt, bounds).status;
    }

    OGRSpatialReference read(const std::string &text)
    {
        OGRSpatialReference srs;
        EXPECT_EQ(setSrsFromUserInput(srs, text.c_str()), OGRERR_NONE) << text;
        return srs;
    }

    TEST(PointCloudCrsPolicy, theStatusesHaveTheirNames)
    {
        for (const auto status : {PointCloudCrsStatus::Projected, PointCloudCrsStatus::Geographic,
                                  PointCloudCrsStatus::Geocentric, PointCloudCrsStatus::Local, PointCloudCrsStatus::None,
                                  PointCloudCrsStatus::Implausible})
            EXPECT_EQ(parsePointCloudCrsStatus(toString(status)), status) << toString(status);
        EXPECT_STREQ(toString(PointCloudCrsStatus::Geographic), "geographic");
        EXPECT_STREQ(toString(PointCloudCrsStatus::Implausible), "implausible");
        EXPECT_FALSE(parsePointCloudCrsStatus("Geographic").has_value());
    }

    TEST(PointCloudCrsPolicy, aGeographicCloudInTheNorthGetsTheUtmZoneOfItsCentre)
    {
        // The Metashape export of the release benchmark: 46.68 N, 13.84 E
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(13.84, 46.68)), "EPSG:32633");
    }

    TEST(PointCloudCrsPolicy, aGeographicCloudInTheSouthGetsASouthernUtmZone)
    {
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(151.21, -33.87)), "EPSG:32756");
    }

    TEST(PointCloudCrsPolicy, aCloudNearTheEdgeOfAZoneStaysInTheZoneOfItsCentre)
    {
        // Within zone 32 (6 to 12 E), up to 0.0005 degrees from its edge
        EXPECT_EQ(targetOf("EPSG:4326", {11.985, 46.0, 500.0, 11.9995, 46.01, 510.0}), "EPSG:32632");
        // Within zone 33, just past the edge
        EXPECT_EQ(targetOf("EPSG:4326", {12.0005, 46.0, 500.0, 12.015, 46.01, 510.0}), "EPSG:32633");
    }

    TEST(PointCloudCrsPolicy, aSmallCloudAcrossTheEdgeOfAZoneStaysInTheUtmZoneOfItsCentre)
    {
        // 18 E, between zones 33 (central meridian 15) and 34 (21): the zone of the centre
        EXPECT_EQ(targetOf("EPSG:4326", {17.98, 46.0, 500.0, 18.01, 46.01, 510.0}), "EPSG:32633");
        EXPECT_EQ(targetOf("EPSG:4326", {17.99, 46.0, 500.0, 18.02, 46.01, 510.0}), "EPSG:32634");
        // Up to 4.5 degrees from the central meridian: 19.4 E is 4.4 degrees from 15
        EXPECT_EQ(targetOf("EPSG:4326", {16.0, 46.0, 500.0, 19.4, 46.5, 510.0}), "EPSG:32633");
    }

    // 10 degrees wide: 5 degrees from the central meridian of any zone of its centre
    TEST(PointCloudCrsPolicy, aCloudWiderThanAZoneGetsATransverseMercatorCentredOnIt)
    {
        const std::string target = targetOf("EPSG:4326", {10.0, 45.0, 0.0, 20.0, 46.0, 100.0});
        const OGRSpatialReference tm = read(target);
        ASSERT_TRUE(tm.IsProjected()) << target;
        EXPECT_STREQ(tm.GetAttrValue("PROJECTION"), SRS_PT_TRANSVERSE_MERCATOR);
        EXPECT_NEAR(tm.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN), 15.0, 1e-9);
        // The same footprint gives the same text, so the COPC of a rebuild is in the same CRS
        EXPECT_EQ(targetOf("EPSG:4326", {10.0, 45.0, 0.0, 20.0, 46.0, 100.0}), target);

        // Past 4.5 degrees from the central meridian: 19.6 E is 4.6 degrees from 15
        const OGRSpatialReference past = read(targetOf("EPSG:4326", {16.0, 46.0, 500.0, 19.6, 46.5, 510.0}));
        EXPECT_STREQ(past.GetAttrValue("PROJECTION"), SRS_PT_TRANSVERSE_MERCATOR);
        EXPECT_NEAR(past.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN), 17.8, 1e-9);
    }

    // Fiji, the Aleutians: clouds across the antimeridian, with longitudes written from -180 to 180 (a
    // header then says -179.99 to 179.99), past 180 (179.9 to 180.1) or from 0 to 360
    TEST(PointCloudCrsPolicy, aCloudAcrossTheAntimeridianIsCentredThere)
    {
        const std::string wgs84 = wktOf("EPSG:4326");
        for (const std::vector<double> &bounds :
             {std::vector<double>{-179.99, -17.01, 0.0, 179.99, -17.0, 50.0}, {179.9, -17.01, 0.0, 180.1, -17.0, 50.0},
              {190.0, 51.8, 0.0, 190.01, 51.81, 50.0}, {0.5, 10.0, 0.0, 359.5, 10.1, 5.0}})
            EXPECT_EQ(PointCloudPolicyStatus(wgs84, bounds), PointCloudCrsStatus::Geographic) << bounds[0];
        EXPECT_EQ(PointCloudPolicyStatus(wgs84, {-181.0, 0.0, 0.0, -170.0, 1.0, 1.0}), PointCloudCrsStatus::Implausible);
        EXPECT_EQ(PointCloudPolicyStatus(wgs84, {350.0, 0.0, 0.0, 361.0, 1.0, 1.0}), PointCloudCrsStatus::Implausible);

        const auto target = [&](const LonLatEnvelope &footprint) {
            return PointCloudCrsPolicy::target(wgs84, PointCloudCrsStatus::Geographic, footprint);
        };
        // The least and greatest longitudes of a header: the cloud is 179.99 to 180.01, zone 1 by its
        // centre, 180
        EXPECT_EQ(target({-179.99, -17.01, 179.99, -17.0}), "EPSG:32701");
        EXPECT_EQ(target({179.9, -17.01, 180.1, -17.0}), "EPSG:32701");
        EXPECT_EQ(target({179.9, -17.01, 179.99, -17.0}), "EPSG:32760");
        // 0 to 360: 190 E is 170 W, zone 2
        EXPECT_EQ(target({190.0, 51.8, 190.01, 51.81}), "EPSG:32602");
        // 10 degrees across the antimeridian: a transverse Mercator centred on it
        const OGRSpatialReference tm = read(target({175.0, -17.1, 185.0, -17.0}));
        EXPECT_STREQ(tm.GetAttrValue("PROJECTION"), SRS_PT_TRANSVERSE_MERCATOR);
        EXPECT_NEAR(std::abs(tm.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN)), 180.0, 1e-9);
    }

    TEST(PointCloudCrsPolicy, theLongitudesOfACloudAreOneRangeOnTheCircle)
    {
        LongitudeRange across;
        for (const double lon : {179.99, -179.99, 179.995, -179.995})
            across.add(lon);
        EXPECT_NEAR(across.west(), 179.99, 1e-9);
        EXPECT_NEAR(across.east(), 180.01, 1e-9);
        LongitudeRange greenwich;
        for (const double lon : {-1.0, 1.0, 359.5})
            greenwich.add(lon);
        EXPECT_NEAR(greenwich.west(), -1.0, 1e-9);
        EXPECT_NEAR(greenwich.east(), 1.0, 1e-9);
        EXPECT_TRUE(LongitudeRange().empty());

        const LonLatEnvelope wrapped = continuousEnvelope({-179.99, -17.01, 179.99, -17.0});
        EXPECT_NEAR(wrapped.minLon, 179.99, 1e-9);
        EXPECT_NEAR(wrapped.maxLon, 180.01, 1e-9);
        const LonLatEnvelope east = continuousEnvelope({190.0, 51.8, 190.01, 51.81});
        EXPECT_NEAR(east.minLon, -170.0, 1e-9);
        EXPECT_NEAR(east.maxLon, -169.99, 1e-9);
    }

    TEST(PointCloudCrsPolicy, aPolarCloudGetsTheUniversalPolarStereographic)
    {
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(15.6, 84.5)), "EPSG:5041");
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(166.7, -80.5)), "EPSG:5042");
        // Below the limits of UTM, UTM
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(15.6, 83.5)), "EPSG:32633");
        EXPECT_EQ(targetOf("EPSG:4326", degreesAround(166.7, -79.5)), "EPSG:32758");
    }

    TEST(PointCloudCrsPolicy, theUtmZoneIsOnTheDatumOfTheCloudWhenEpsgHasOne)
    {
        EXPECT_EQ(targetOf("EPSG:4258", degreesAround(13.84, 46.68)), "EPSG:25833");  // ETRS89
        EXPECT_EQ(targetOf("EPSG:6318", degreesAround(-82.59, 27.53)), "EPSG:6346");  // NAD83(2011)
        // NAD83 has no UTM zone in Europe: WGS 84's
        EXPECT_EQ(targetOf("EPSG:4269", degreesAround(13.84, 46.68)), "EPSG:32633");
    }

    TEST(PointCloudCrsPolicy, aCompoundGeographicCloudKeepsItsVerticalCrs)
    {
        // The Florida file of the corpus: NAD83(2011) + NAVD88 height
        EXPECT_EQ(targetOf("EPSG:6318+5703", degreesAround(-82.59, 27.53)), "EPSG:6346+5703");
        EXPECT_EQ(targetOf("EPSG:4326+5773", degreesAround(13.84, 46.68)), "EPSG:32633+5773");
    }

    // The COPC is metric on its three axes, so that its octree is a cube on the ground: a vertical CRS
    // in feet becomes the one of its datum in metres
    TEST(PointCloudCrsPolicy, aVerticalCrsInFeetBecomesTheOneOfItsDatumInMetres)
    {
        // NAD83(2011) + NAVD88 height (ftUS), NAVD88 height (ft)
        EXPECT_EQ(targetOf("EPSG:6318+6360", degreesAround(-82.59, 27.53)), "EPSG:6346+5703");
        EXPECT_EQ(targetOf("EPSG:6318+8228", degreesAround(-82.59, 27.53)), "EPSG:6346+5703");

        // Poolbeg height (ft(Br36)) has no variant in metres: its heights are converted
        const std::string target = targetOf("EPSG:4299+5754", degreesAround(-6.26, 53.35));
        const OGRSpatialReference compound = read(target);
        ASSERT_TRUE(compound.IsCompound()) << target;
        EXPECT_NEAR(verticalUnitInMeters(compound), 1.0, 1e-12) << target;
        EXPECT_NE(target.find("Poolbeg height"), std::string::npos) << target;
    }

    TEST(PointCloudCrsPolicy, aGeographic3DCloudGetsUtmWithTheEllipsoidalHeight)
    {
        const std::string target = targetOf("EPSG:4979", degreesAround(13.84, 46.68));
        const OGRSpatialReference utm3d = read(target);
        EXPECT_TRUE(utm3d.IsProjected()) << target;
        EXPECT_EQ(utm3d.GetAxesCount(), 3) << target;
        EXPECT_STREQ(utm3d.GetName(), "WGS 84 / UTM zone 33N");
    }

    // A grid of points around lon, lat, at heights of h to h + 50 m, in EPSG:4978
    std::vector<double> geocentricBounds(double lon, double lat, double h)
    {
        const SrsPtr from = srsFromEpsg(4979);
        const SrsPtr to = srsFromEpsg(4978);
        CoordsTransformer transform(srsOf(from), srsOf(to));
        std::vector<double> b = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
        for (const double dLon : {-0.005, 0.005})
            for (const double dLat : {-0.005, 0.005})
                for (const double dh : {0.0, 50.0})
                {
                    double x = lon + dLon, y = lat + dLat, z = h + dh;
                    EXPECT_TRUE(transform.tryTransform(1, &x, &y, &z));
                    const double c[3] = {x, y, z};
                    for (int i = 0; i < 3; i++)
                    {
                        b[i] = std::min(b[i], c[i]);
                        b[i + 3] = std::max(b[i + 3], c[i]);
                    }
                }
        return b;
    }

    TEST(PointCloudCrsPolicy, aGeocentricCloudOnTheEarthGetsUtmWithTheEllipsoidalHeight)
    {
        const std::string wkt = wktOf("EPSG:4978");
        const auto bounds = geocentricBounds(-83.6, 41.65, 180.0);
        const PointCloudCrsCheck check = PointCloudCrsPolicy::classify(wkt, bounds);
        EXPECT_EQ(check.status, PointCloudCrsStatus::Geocentric);
        EXPECT_TRUE(check.warning.empty());

        const std::string target = PointCloudCrsPolicy::target(wkt, check.status, {-83.605, 41.645, -83.595, 41.655});
        const OGRSpatialReference utm3d = read(target);
        EXPECT_EQ(utm3d.GetAxesCount(), 3) << target;
        EXPECT_STREQ(utm3d.GetName(), "WGS 84 / UTM zone 17N");
    }

    TEST(PointCloudCrsPolicy, aGeocentricCrsOverLocalCoordinatesIsImplausible)
    {
        // The header of the toledo cloud says EPSG:4978, its points are within 300 m of 0, 0, 0
        const PointCloudCrsCheck check =
            PointCloudCrsPolicy::classify(wktOf("EPSG:4978"), {-88.0, -95.0, -12.0, 212.0, 190.0, 40.0});
        EXPECT_EQ(check.status, PointCloudCrsStatus::Implausible);
        EXPECT_EQ(check.warning, "coordinate system ignored: the coordinates do not fit WGS 84");
        EXPECT_EQ(PointCloudCrsPolicy::target(wktOf("EPSG:4978"), check.status, {}), "");
    }

    TEST(PointCloudCrsPolicy, aGeographicCrsBeyondThePolesIsImplausible)
    {
        const PointCloudCrsCheck check =
            PointCloudCrsPolicy::classify(wktOf("EPSG:4326"), {13.8, 46.6, 500.0, 13.9, 95.0, 600.0});
        EXPECT_EQ(check.status, PointCloudCrsStatus::Implausible);
        EXPECT_EQ(check.warning, "coordinate system ignored: the coordinates do not fit WGS 84");
        // Projected coordinates in a geographic CRS
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:4326"), {576662.6, 5188115.1, 156.0, 576767.5, 5188210.8, 169.4})
                      .status,
                  PointCloudCrsStatus::Implausible);
    }

    TEST(PointCloudCrsPolicy, aProjectedCrsFarFromItsAreaOfUseIsImplausible)
    {
        // Swiss LV95 (2.4 to 2.9 million metres east) given coordinates of UTM: 55 W 73 N, 60 degrees
        // west of Switzerland
        const PointCloudCrsCheck check =
            PointCloudCrsPolicy::classify(wktOf("EPSG:2056"), {576662.6, 5188115.1, 156.0, 576767.5, 5188210.8, 169.4});
        EXPECT_EQ(check.status, PointCloudCrsStatus::Implausible);
        EXPECT_EQ(check.warning.rfind("coordinate system ignored: the coordinates do not fit CH1903+ / LV95", 0), 0u)
            << check.warning;
        // A UTM zone used 3 degrees past its edge (a country in one zone) is not
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:32632"), {955000.0, 6100000.0, 0.0, 956000.0, 6101000.0, 10.0})
                      .status,
                  PointCloudCrsStatus::Projected);
        // Nor EUREF89 / UTM 33 (EPSG:25833, an area from 12 to 18 E) for the north of Norway at 30 E
        // 70 N and for Svalbard at 34 E 79 N, as Norway maps them
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:25833"), {1067500.0, 7836100.0, 0.0, 1067600.0, 7836200.0, 10.0})
                      .status,
                  PointCloudCrsStatus::Projected);
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:25833"), {897800.0, 8835300.0, 0.0, 897900.0, 8835400.0, 10.0})
                      .status,
                  PointCloudCrsStatus::Projected);
        // Coordinates PROJ cannot take back to WGS 84
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:32633"), {1e12, 1e12, 0.0, 1.1e12, 1.1e12, 1.0}).status,
                  PointCloudCrsStatus::Implausible);
    }

    TEST(PointCloudCrsPolicy, aProjectedCloudKeepsItsCrsInAnyUnit)
    {
        // Autzen: NAD83(HARN) / Oregon GIC Lambert (ft), coordinates in international feet
        const std::string feet = wktOf("EPSG:2994");
        const std::vector<double> autzen = {635577.79, 848882.15, 406.14, 639003.73, 853537.66, 615.26};
        const PointCloudCrsCheck check = PointCloudCrsPolicy::classify(feet, autzen);
        EXPECT_EQ(check.status, PointCloudCrsStatus::Projected);
        EXPECT_EQ(PointCloudCrsPolicy::target(feet, check.status, {-123.08, 44.05, -123.06, 44.07}), "");

        // A compound CRS with a projected horizontal part
        const std::string compound = wktOf("EPSG:6405+6360");
        EXPECT_EQ(PointCloudCrsPolicy::classify(compound, {738000.0, 991500.0, 2480.0, 738200.0, 991600.0, 2500.0}).status,
                  PointCloudCrsStatus::Projected);
    }

    TEST(PointCloudCrsPolicy, aCloudWithoutACrsOrInALocalOneKeepsIt)
    {
        const std::vector<double> local = {100.0, 200.0, 5.0, 110.0, 210.0, 8.0};
        EXPECT_EQ(PointCloudCrsPolicy::classify("", local).status, PointCloudCrsStatus::None);
        EXPECT_EQ(PointCloudCrsPolicy::classify("not a crs", local).status, PointCloudCrsStatus::None);
        const std::string site =
            R"wkt(LOCAL_CS["Site grid",LOCAL_DATUM["Site datum",0],UNIT["metre",1],AXIS["X",EAST],AXIS["Y",NORTH]])wkt";
        EXPECT_EQ(PointCloudCrsPolicy::classify(site, local).status, PointCloudCrsStatus::Local);
        EXPECT_EQ(PointCloudCrsPolicy::target(site, PointCloudCrsStatus::Local, {}), "");
        EXPECT_EQ(PointCloudCrsPolicy::target("", PointCloudCrsStatus::None, {}), "");
    }

    TEST(PointCloudCrsPolicy, withoutBoundsOnlyTheKindOfTheCrsIsKnown)
    {
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:4326"), {}).status, PointCloudCrsStatus::Geographic);
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:4978"), {}).status, PointCloudCrsStatus::Geocentric);
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:32633"), {}).status, PointCloudCrsStatus::Projected);
        EXPECT_EQ(PointCloudCrsPolicy::classify(wktOf("EPSG:4326+5773"), {}).status, PointCloudCrsStatus::Geographic);
    }

    TEST(PointCloudCrsPolicy, theSameCrsIsRecognisedWhateverItsSpelling)
    {
        EXPECT_TRUE(PointCloudCrsPolicy::isSame("EPSG:32633", wktOf("EPSG:32633")));
        EXPECT_TRUE(PointCloudCrsPolicy::isSame("EPSG:6346+5703", wktOf("EPSG:6346+5703")));
        EXPECT_FALSE(PointCloudCrsPolicy::isSame("EPSG:32633", "EPSG:32632"));
        EXPECT_FALSE(PointCloudCrsPolicy::isSame("EPSG:32633", ""));

        const auto name = crsNameOf(wktOf("EPSG:32633"));
        ASSERT_TRUE(name.has_value());
        EXPECT_EQ(name->name, "WGS 84 / UTM zone 33N");
        EXPECT_EQ(name->epsg, 32633);
        const auto compound = crsNameOf("EPSG:6346+5703");
        ASSERT_TRUE(compound.has_value());
        EXPECT_EQ(compound->name, "NAD83(2011) / UTM zone 17N + NAVD88 height");
        EXPECT_FALSE(compound->epsg.has_value());
        EXPECT_FALSE(crsNameOf("").has_value());
    }
}
