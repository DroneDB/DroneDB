/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The least-squares planes the volume and the stockpile detection fit to the ground

#include <cmath>

#include "gtest/gtest.h"

#include "plane_fit.h"

namespace {

using namespace ddb;

TEST(PlaneFit, aTiltedPlaneIsFoundExactlyAtLargeMapCoordinates) {
    PlanePoints points;
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            const double x = 578000.0 + i * 3.0, y = 4985000.0 - j * 2.0;
            points.add(x, y, 0.4 * x - 0.25 * y + 12.0);
        }
    }
    const Plane p = bestFitPlane(points);
    EXPECT_NEAR(p.a, 0.4, 1e-9);
    EXPECT_NEAR(p.b, -0.25, 1e-9);
    EXPECT_NEAR(p.at(578010.0, 4984990.0), 0.4 * 578010.0 - 0.25 * 4984990.0 + 12.0, 1e-6);
}

TEST(PlaneFit, pointsOnALineGiveTheirMeanElevation) {
    PlanePoints points;
    for (int i = 0; i < 5; i++) points.add(i, 2.0 * i, i);
    const Plane p = bestFitPlane(points);
    EXPECT_DOUBLE_EQ(p.a, 0.0);
    EXPECT_DOUBLE_EQ(p.b, 0.0);
    EXPECT_DOUBLE_EQ(p.c, 2.0);
}

TEST(PlaneFit, theWeightsOfTheSumsCountAsRepeatedPoints) {
    PlaneSums weighted, repeated;
    const double pts[4][3] = {{0, 0, 1}, {1, 0, 2}, {0, 1, 4}, {1, 1, 4}};
    for (const auto &q : pts) {
        weighted.add(q[0], q[1], q[2], q[0] == 1 && q[1] == 1 ? 3.0 : 1.0);
        for (int k = 0; k < (q[0] == 1 && q[1] == 1 ? 3 : 1); k++) repeated.add(q[0], q[1], q[2]);
    }
    Plane a, b;
    ASSERT_TRUE(solvePlane(weighted, a));
    ASSERT_TRUE(solvePlane(repeated, b));
    EXPECT_NEAR(a.a, b.a, 1e-12);
    EXPECT_NEAR(a.b, b.b, 1e-12);
    EXPECT_NEAR(a.c, b.c, 1e-12);
}

// A ring of sloping ground with a vehicle on a part of it: the robust plane is the ground's
TEST(PlaneFit, theRobustPlaneLeavesOutWhatIsNotGround) {
    PlanePoints points;
    for (int i = 0; i < 360; i++) {
        const double t = i * 3.141592653589793 / 180.0;
        const double x = 10.0 * std::cos(t), y = 10.0 * std::sin(t);
        const double ground = 100.0 + 0.5 * x + 0.2 * y;
        points.add(x, y, ground + (i < 30 ? 2.5 : 0.0));
    }
    const Plane plain = bestFitPlane(points);
    const RobustPlane robust = robustPlaneFit(points);
    EXPECT_GT(std::fabs(plain.c - 100.0), 0.1);
    EXPECT_NEAR(robust.plane.a, 0.5, 1e-9);
    EXPECT_NEAR(robust.plane.b, 0.2, 1e-9);
    EXPECT_NEAR(robust.plane.c, 100.0, 1e-9);
    EXPECT_EQ(robust.inliers, 330u);
    EXPECT_NEAR(robust.rms, 0.0, 1e-9);
}

} // namespace
