/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The footprint an image gets from its EXIF and XMP. Its exact numbers are pinned: the camera
// model that the footprint and the coverage search share must not move a corner of it.

#include <string>

#include "entry.h"
#include "golden_support.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

namespace
{
    using namespace ddb;

    std::string footprintWkt(double pitch, double yaw, double roll, const GeoLocation &geo, double relAltitude)
    {
        BasicPolygonGeometry geom;
        calculateFootprint(SensorSize(36, 24), geo, Focal(50, 0), CameraOrientation(pitch, yaw, roll), relAltitude, geom);
        return geom.toWkt();
    }

    // A tilted and rolled camera of a drone that flies at 100 m over a ground 98.31 m above the sea
    TEST(footprintFixed, Roll)
    {
        EXPECT_EQ(footprintWkt(-60, 30, 15, GeoLocation(46.842607, -91.99456, 198.31), 100.0), "POLYGONZ ((-91.99401873599 46.84337947923 98.31, -91.9944560599 46.84287477738 98.31, -91.99357802069 46.84251657143 98.31, -91.99314069157 46.84302126991 98.31, -91.99401873599 46.84337947923 98.31))");
    }

    // A camera that looks closer to the horizon than 30 degrees below it is taken as nadir
    TEST(footprintFixed, PitchAboveTheCapIsNadir)
    {
        const GeoLocation geo(46.842607, -91.99456, 198.31);
        const std::string capped = footprintWkt(-10, 30, 0, geo, 100.0);
        EXPECT_EQ(capped, "POLYGONZ ((-91.99480495707 46.84295817812 98.31, -91.99512664539 46.84258691731 98.31, -91.99431504609 46.84225582136 98.31, -91.99399335423 46.84262707988 98.31, -91.99480495707 46.84295817812 98.31))");
        EXPECT_EQ(capped, footprintWkt(-90, 30, 0, geo, 100.0));
        // Exactly on the cap it is a tilted camera
        EXPECT_EQ(footprintWkt(-30, 30, 0, geo, 100.0), "POLYGONZ ((-91.99270380678 46.84538296742 98.31, -91.99425945911 46.84358772387 98.31, -91.99344785027 46.84325662176 98.31, -91.99189218082 46.84505185425 98.31, -91.99270380678 46.84538296742 98.31))");
    }

    // A GPS altitude of 0 (no altitude in the tags) puts the ground at the relative altitude
    TEST(footprintFixed, NoGpsAltitude)
    {
        EXPECT_EQ(footprintWkt(-90, 135, 0, GeoLocation(46.842607, -91.99456, 0.0), 80.0), "POLYGONZ ((-91.99411379058 46.84266416732 80, -91.99446528764 46.84291160752 80, -91.9950062085 46.84254983093 80, -91.99465471127 46.84230239237 80, -91.99411379058 46.84266416732 80))");
    }

    // The southern hemisphere (a zone with a false northing) and the east, a yaw beyond 180
    TEST(footprintFixed, SouthernHemisphere)
    {
        EXPECT_EQ(footprintWkt(-75, -200, -8, GeoLocation(-33.8688, 151.2093, 120.5), 60.0), "POLYGONZ ((151.2097378959 -33.86898480195 60.5, 151.2096288142 -33.86872056936 60.5, 151.2091774607 -33.86885022508 60.5, 151.2092865412 -33.86911445803 60.5, 151.2097378959 -33.86898480195 60.5))");
    }

    TEST(footprintFixed, DroneImages)
    {
        TestArea ta(TEST_NAME);
        json footprints = json::object();
        json wkts = json::object();
        for (int number = 18; number <= 35; number++)
        {
            const std::string name = "DJI_00" + std::to_string(number) + ".JPG";
            const fs::path image = ta.downloadTestAsset(testDataUrl("images/" + name), name);
            Entry entry;
            parseEntry(image, ta.getFolder(), entry, false);
            ASSERT_EQ(entry.polygon_geom.size(), 5) << name;

            json ring = json::array();
            for (const auto &p : entry.polygon_geom.points)
                ring.push_back({p.x, p.y, p.z});
            footprints[name] = {{"ring", ring},
                                {"point", {entry.point_geom.points.at(0).x, entry.point_geom.points.at(0).y, entry.point_geom.points.at(0).z}}};
            wkts[name] = entry.polygon_geom.toWkt();
        }
        golden::expectGolden("footprint_drone_images", footprints);
        // The bytes of the WKT, compared between two builds with DDB_GOLDEN_DUMP_DIR
        golden::dumpOnly("footprint_drone_images_wkt", wkts);
    }
}
