/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Which way is up in a splat scene, from the splats alone. The scenes are made here, in the frames
// the trainers give them: COLMAP and INRIA scenes are Y-down, ODM scenes are Z-up.

#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "json.h"
#include "splat_frame.h"

namespace {

using namespace ddb;

using Vec3 = std::array<double, 3>;

struct Scene {
    std::vector<float> positions;
    std::vector<float> opacities;

    void add(const Vec3& p, float opacity = 0.9f) {
        positions.insert(positions.end(), {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
        opacities.push_back(opacity);
    }
};

// The frame a trainer gave a scene: `up` (with its sign) and two axes across it, the cross products
// of up with the axis it is not
struct Frame {
    Vec3 u{}, v{}, up{};

    explicit Frame(const Vec3& upAxis) : up(upAxis) {
        const Vec3 other = std::abs(up[0]) < 0.5 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        u = {up[1] * other[2] - up[2] * other[1], up[2] * other[0] - up[0] * other[2],
             up[0] * other[1] - up[1] * other[0]};
        const double un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& c : u) c /= un;
        v = {up[1] * u[2] - up[2] * u[1], up[2] * u[0] - up[0] * u[2], up[0] * u[1] - up[1] * u[0]};
    }

    // The point `a` and `b` across the ground and `h` above it
    Vec3 at(double a, double b, double h) const {
        return Vec3{u[0] * a + v[0] * b + up[0] * h, u[1] * a + v[1] * b + up[1] * h, u[2] * a + v[2] * b + up[2] * h};
    }
};

// A ground of `side` metres square with a little roughness, and things standing on it: columns of
// splats that rise up to `height` from points of the ground. `upAxis` is the axis (with sign) the
// scene stands on, the frame the trainer chose.
Scene groundWithObjects(const Vec3& upAxis, int ground = 3000, int objects = 1500, double side = 20.0,
                        double height = 4.0) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> across(-side / 2, side / 2), rise(0.0, height);
    std::normal_distribution<double> rough(0.0, 0.03);
    const Frame frame(upAxis);

    Scene scene;
    for (int i = 0; i < ground; ++i)
        scene.add(frame.at(across(rng), across(rng), rough(rng)));
    // Objects: a few clusters, each a column
    for (int c = 0; c < 6; ++c) {
        const double cx = across(rng) * 0.8, cy = across(rng) * 0.8;
        for (int i = 0; i < objects / 6; ++i)
            scene.add(frame.at(cx + rough(rng) * 20, cy + rough(rng) * 20, rise(rng)));
    }
    return scene;
}

// How many splats each surface of a room holds
struct RoomSplats {
    int shelfWall = 0; // the wall the capture looked at most (a bookcase, a fireplace): more than the floor
    int otherWalls = 0; // each of the three others
    int floor = 0;
    int ceiling = 0;
    int furniture = 0; // in boxes up to a metre high, on the floor
};

// A room `width` by `width` and `height` high in the frame of `upAxis`, as an INRIA capture from inside
// it sees it: a floor with furniture, a plainer ceiling, and walls
Scene room(const Vec3& upAxis, double width, double height, const RoomSplats& splats) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> across(-width / 2, width / 2), rise(0.0, height), low(0.0, 1.0);
    std::normal_distribution<double> rough(0.0, 0.02);
    const Frame frame(upAxis);
    const double half = width / 2;

    Scene scene;
    for (int i = 0; i < splats.floor; ++i)
        scene.add(frame.at(across(rng), across(rng), rough(rng)));
    for (int i = 0; i < splats.ceiling; ++i)
        scene.add(frame.at(across(rng), across(rng), height + rough(rng)));
    for (int i = 0; i < splats.shelfWall; ++i)
        scene.add(frame.at(-half + rough(rng), across(rng), rise(rng)));
    for (int i = 0; i < splats.otherWalls; ++i) {
        scene.add(frame.at(half + rough(rng), across(rng), rise(rng)));
        scene.add(frame.at(across(rng), -half + rough(rng), rise(rng)));
        scene.add(frame.at(across(rng), half + rough(rng), rise(rng)));
    }
    for (int box = 0; box < 5; ++box) {
        const double cx = across(rng) * 0.7, cy = across(rng) * 0.7;
        for (int i = 0; i < splats.furniture / 5; ++i)
            scene.add(frame.at(cx + rough(rng) * 25, cy + rough(rng) * 25, low(rng)));
    }
    return scene;
}

double degreesBetween(const Vec3& a, const Vec3& b) {
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::acos(std::max(-1.0, std::min(1.0, d))) * 180.0 / 3.14159265358979323846;
}

Vec3 normalised(Vec3 v) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (double& c : v) c /= n;
    return v;
}

// INRIA and COLMAP scenes are Y-down: the ground is the plane y = 0 and what stands on it has a
// negative y
TEST(splatFrame, aYDownSceneStandsOnItsGroundWithUpAtMinusY) {
    const Scene scene = groundWithObjects({0, -1, 0});
    const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_EQ(up.source, SplatUpSource::GroundPlane);
    EXPECT_LT(degreesBetween(up.up, {0, -1, 0}), 3.0) << up.up[0] << " " << up.up[1] << " " << up.up[2];
    EXPECT_GE(up.confidence, SplatUpConfident);
}

// ODM scenes are Z-up
TEST(splatFrame, aZUpSceneStandsOnItsGroundWithUpAtPlusZ) {
    const Scene scene = groundWithObjects({0, 0, 1});
    const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_EQ(up.source, SplatUpSource::GroundPlane);
    EXPECT_LT(degreesBetween(up.up, {0, 0, 1}), 3.0) << up.up[0] << " " << up.up[1] << " " << up.up[2];
    EXPECT_GE(up.confidence, SplatUpConfident);
}

// The same scene under any rotation gives the rotated answer, the sign included
TEST(splatFrame, aTiltedSceneGivesTheTiltedUp) {
    for (const Vec3 axis : {Vec3{1, 1, 0}, Vec3{0.3, -0.5, 0.8}, Vec3{-0.4, 0.2, -0.9}}) {
        const Vec3 expected = normalised(axis);
        const Scene scene = groundWithObjects(expected);
        const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
        EXPECT_LT(degreesBetween(up.up, expected), 3.0) << axis[0] << " " << axis[1] << " " << axis[2];
        EXPECT_GE(up.confidence, SplatUpConfident);
    }
}

// A shell of splats has no ground and no flat axis: the answer is a guess, and says so
TEST(splatFrame, aSphereHasNoUpAndTheConfidenceSaysSo) {
    std::mt19937 rng(11);
    std::normal_distribution<double> gauss(0.0, 1.0);
    Scene shell, ball;
    for (int i = 0; i < 4000; ++i) {
        const Vec3 d = normalised({gauss(rng), gauss(rng), gauss(rng)});
        shell.add({d[0] * 5, d[1] * 5, d[2] * 5});
        ball.add({gauss(rng) * 5, gauss(rng) * 5, gauss(rng) * 5});
    }
    for (const Scene* scene : {&shell, &ball}) {
        const SplatUp up = estimateSplatUp(scene->positions, scene->opacities);
        EXPECT_LT(up.confidence, 0.2);
        const double length = std::sqrt(up.up[0] * up.up[0] + up.up[1] * up.up[1] + up.up[2] * up.up[2]);
        EXPECT_NEAR(length, 1.0, 1e-9) << "an answer is a unit vector even when it is a guess";
    }
}

// A single flat sheet with nothing on it has a plane but no side to stand on
TEST(splatFrame, aSheetWithNothingOnItHasNoSign) {
    Scene sheet = groundWithObjects({0, 0, 1}, 3000, 0);
    const SplatUp up = estimateSplatUp(sheet.positions, sheet.opacities);
    EXPECT_LT(up.confidence, SplatUpConfident);
}

// A capture from inside a room (INRIA's playroom): the wall the camera looked at most holds more splats
// than the floor, so the plane that holds the most is a wall, and the room stood on it is on its side.
// A room is as high as it is wide, it has no flat axis to tell its floor from its walls: the floor is
// the best guess, but not one to stand the scene on.
TEST(splatFrame, aRoomAsHighAsItIsWideIsNotStoodOnAWall) {
    const Vec3 yDown = {0, -1, 0};
    const Scene scene = room(yDown, 6.0, 4.5, {2500, 1200, 1500, 800, 1200});
    const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_LT(up.confidence, SplatUpConfident);
    EXPECT_LT(degreesBetween(up.up, yDown), 3.0) << up.up[0] << " " << up.up[1] << " " << up.up[2];
}

// A wide room (INRIA's drjohnson hall) is lower than it is wide: its floor is the plane along its
// flattest axis, even though a wall holds more splats, and the furnished floor is the side it stands on
TEST(splatFrame, aWideRoomStandsOnItsFloorWhenAWallHoldsMoreSplats) {
    for (const Vec3 axis : {Vec3{0, -1, 0}, Vec3{0.84, 0, -0.54}}) {
        const Vec3 expected = normalised(axis);
        const Scene scene = room(expected, 12.0, 3.0, {5000, 1000, 2500, 800, 1500});
        const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
        EXPECT_LT(degreesBetween(up.up, expected), 3.0) << up.up[0] << " " << up.up[1] << " " << up.up[2];
        EXPECT_GE(up.confidence, SplatUpConfident);
    }
}

// A subject on level ground beside a wooded slope that holds most of the splats (INRIA's train): the
// slope is the densest plane in a thick slab and the crisp level ground in a thin one. The scene has two
// grounds 20 degrees apart, and the estimate says it is not sure rather than tilt the subject.
TEST(splatFrame, aSubjectOnLevelGroundBesideALargerSlopeIsNotConfident) {
    std::mt19937 rng(13);
    std::uniform_real_distribution<double> level(-6.0, 6.0), uphill(6.0, 36.0), along(-15.0, 15.0), rise(0.0, 3.0);
    std::normal_distribution<double> crisp(0.0, 0.02), leafy(0.0, 1.5), spot(0.0, 0.4);
    const Frame frame({0, -1, 0});
    const double slope = std::tan(20.0 * 3.14159265358979323846 / 180.0);

    Scene scene;
    for (int i = 0; i < 4000; ++i)
        scene.add(frame.at(level(rng), level(rng), crisp(rng)));
    for (int column = 0; column < 4; ++column) { // the subject
        const double ca = level(rng) * 0.5, cb = level(rng) * 0.5;
        for (int i = 0; i < 200; ++i)
            scene.add(frame.at(ca + spot(rng), cb + spot(rng), rise(rng)));
    }
    for (int i = 0; i < 6000; ++i) { // the slope, its trees a thick layer of leaves
        const double a = uphill(rng);
        scene.add(frame.at(a, along(rng), (a - 6.0) * slope + std::abs(leafy(rng))));
    }
    const SplatUp up = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_LT(up.confidence, SplatUpConfident) << up.up[0] << " " << up.up[1] << " " << up.up[2];
}

// A georeferenced scene is in a local frame whose Z is up, whatever its splats look like
TEST(splatFrame, aGeoreferencedSceneIsZUpWithFullConfidence) {
    const Scene yDown = groundWithObjects({0, -1, 0});
    const SplatUp up = estimateSplatUp(yDown.positions, yDown.opacities, /*georeferenced=*/true);
    EXPECT_EQ(up.up, (Vec3{0, 0, 1}));
    EXPECT_EQ(up.confidence, 1.0);
    EXPECT_EQ(up.source, SplatUpSource::Georef);

    // No splats are looked at: an empty scene is as georeferenced as any
    EXPECT_EQ(estimateSplatUp({}, {}, true).source, SplatUpSource::Georef);
}

// The opacity is the weight: a haze of nearly transparent splats under the ground outnumbers what
// stands on it, and does not decide which side is up
TEST(splatFrame, opacityWeighsTheVoteForTheSide) {
    Scene scene = groundWithObjects({0, 0, 1}, 3000, 1200);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> across(-10.0, 10.0), depth(-4.0, -0.5);
    for (int i = 0; i < 2500; ++i)
        scene.add({across(rng), across(rng), depth(rng)}, 0.06f);

    const SplatUp weighted = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_LT(degreesBetween(weighted.up, {0, 0, 1}), 3.0);

    // Counting each splat as one, the haze wins (an empty opacity array weighs all as 1)
    const SplatUp counted = estimateSplatUp(scene.positions, {});
    EXPECT_LT(degreesBetween(counted.up, {0, 0, -1}), 3.0);
}

// The same splats give the same answer, however many times they are asked
TEST(splatFrame, theEstimateIsDeterministic) {
    const Scene scene = groundWithObjects({0, -1, 0});
    const SplatUp a = estimateSplatUp(scene.positions, scene.opacities);
    const SplatUp b = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_EQ(a.up, b.up);
    EXPECT_EQ(a.confidence, b.confidence);
}

// Splats with no position, no opacity or no number to weigh them with are not part of the scene
TEST(splatFrame, splatsThatAreNotNumbersAreLeftOut) {
    Scene scene = groundWithObjects({0, 0, 1});
    const SplatUp clean = estimateSplatUp(scene.positions, scene.opacities);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    scene.add({nan, 0, 0});
    scene.add({0, inf, 0});
    scene.add({0, 0, -inf});
    scene.add({1, 1, 1}, nan);
    const SplatUp dirty = estimateSplatUp(scene.positions, scene.opacities);
    EXPECT_LT(degreesBetween(dirty.up, clean.up), 0.5);
    EXPECT_NEAR(dirty.confidence, clean.confidence, 0.05);
}

TEST(splatFrame, tooFewSplatsGiveNoAnswerWorthTrusting) {
    EXPECT_EQ(estimateSplatUp({}, {}).confidence, 0.0);
    Scene few;
    for (int i = 0; i < 5; ++i) few.add({double(i), 0, 0});
    EXPECT_EQ(estimateSplatUp(few.positions, few.opacities).confidence, 0.0);
}

// The members bounds.json gets, named as the viewers read them
TEST(splatFrame, theSidecarMembersAreNamedAsTheViewersReadThem) {
    SplatUp up;
    up.up = {0.0, -1.0, 0.0};
    up.confidence = 0.8123456789;
    up.source = SplatUpSource::GroundPlane;
    const json j = up;
    ASSERT_TRUE(j.contains("up"));
    ASSERT_EQ(j["up"].size(), 3u);
    EXPECT_EQ(j["up"][1].get<double>(), -1.0);
    EXPECT_NEAR(j["upConfidence"].get<double>(), 0.812346, 1e-9);
    EXPECT_EQ(j["upSource"], "ground-plane");

    EXPECT_STREQ(splatUpSourceName(SplatUpSource::Georef), "georef");
    EXPECT_STREQ(splatUpSourceName(SplatUpSource::Pca), "pca");
}

// The principal axes of weighted points: the widest first, unit vectors, the mean weighted
TEST(splatFrame, principalAxesAreTheWidestFirst) {
    std::mt19937 rng(3);
    std::normal_distribution<double> wide(0.0, 10.0), middle(0.0, 3.0), thin(0.0, 0.5);
    std::vector<std::array<double, 3>> points;
    std::vector<double> weights;
    // Widest along y, then z, thinnest along x, around (1, 2, 3)
    for (int i = 0; i < 4000; ++i) {
        points.push_back({1.0 + thin(rng), 2.0 + wide(rng), 3.0 + middle(rng)});
        weights.push_back(1.0);
    }
    const SplatAxes axes = principalAxes(points, weights);
    EXPECT_GT(std::abs(axes.axes[0][1]), 0.99);
    EXPECT_GT(std::abs(axes.axes[1][2]), 0.99);
    EXPECT_GT(std::abs(axes.axes[2][0]), 0.99);
    EXPECT_GT(axes.variances[0], axes.variances[1]);
    EXPECT_GT(axes.variances[1], axes.variances[2]);
    EXPECT_NEAR(axes.variances[0], 100.0, 10.0);
    for (const auto& axis : axes.axes)
        EXPECT_NEAR(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2], 1.0, 1e-9);
    EXPECT_NEAR(axes.mean[0], 1.0, 0.1);

    // A point of all the weight is the mean
    points.push_back({100.0, 100.0, 100.0});
    weights.assign(weights.size(), 0.0);
    weights.push_back(1.0);
    const SplatAxes heavy = principalAxes(points, weights);
    EXPECT_NEAR(heavy.mean[0], 100.0, 1e-9);
    EXPECT_NEAR(heavy.variances[0], 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// How a viewer frames a scene
// ---------------------------------------------------------------------------

// `count` splats spread evenly through the ball of `radius` around `centre`
void addBall(Scene& scene, std::mt19937& rng, const Vec3& centre, double radius, int count, float opacity = 0.9f) {
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int added = 0; added < count;) {
        const Vec3 d = {unit(rng), unit(rng), unit(rng)};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1.0) continue;
        scene.add({centre[0] + radius * d[0], centre[1] + radius * d[1], centre[2] + radius * d[2]}, opacity);
        ++added;
    }
}

// `count` splats spread evenly through the box from `min` to `max`
void addBox(Scene& scene, std::mt19937& rng, const Vec3& min, const Vec3& max, int count) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int i = 0; i < count; ++i)
        scene.add({min[0] + (max[0] - min[0]) * unit(rng), min[1] + (max[1] - min[1]) * unit(rng),
                   min[2] + (max[2] - min[2]) * unit(rng)});
}

// An object capture (a train, a truck): the cameras went round the subject, a ball of 1 m here at
// (5, -1, 2), and saw what is around it from afar, a sparse shell from 15 to 30 m out
Scene objectCapture() {
    std::mt19937 rng(11);
    Scene scene;
    const Vec3 subject = {5.0, -1.0, 2.0};
    addBall(scene, rng, subject, 1.0, 12000);
    std::normal_distribution<double> direction(0.0, 1.0);
    std::uniform_real_distribution<double> distance(15.0, 30.0);
    for (int i = 0; i < 6000; ++i) {
        Vec3 d = {direction(rng), direction(rng), direction(rng)};
        const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double r = distance(rng);
        scene.add({subject[0] + d[0] / n * r, subject[1] + d[1] / n * r, subject[2] + d[2] / n * r});
    }
    return scene;
}

void expectCoreWithin(const SplatFraming& framing, const Vec3& min, const Vec3& max, double tolerance) {
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(framing.coreMin[k], min[k], tolerance) << "axis " << k;
        EXPECT_NEAR(framing.coreMax[k], max[k], tolerance) << "axis " << k;
    }
}

// The cameras went round a subject: it is framed from outside, on the subject alone, not on the
// shell of its surroundings that the box of all the splats would show it lost in
TEST(splatFraming, aDenseSubjectInASparseShellIsAnObjectFramedOnTheSubject) {
    const Scene scene = objectCapture();
    const auto framing = estimateSplatFraming(scene.positions, scene.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Object);
    EXPECT_LT(framing->concentration, SplatObjectConcentration);
    // The ball of 1 m and a little of what is around it
    expectCoreWithin(*framing, {4.0, -2.0, 1.0}, {6.0, 0.0, 3.0}, 0.3);
}

// A room seen from inside is about as dense everywhere: it is framed from inside, on the middle 80%
// of each axis
TEST(splatFraming, aBoxFilledEvenlyIsASceneFramedOnItsMiddle) {
    std::mt19937 rng(5);
    Scene scene;
    addBox(scene, rng, {-5.0, -2.0, -5.0}, {5.0, 2.0, 5.0}, 20000);
    const auto framing = estimateSplatFraming(scene.positions, scene.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Scene);
    EXPECT_GE(framing->concentration, SplatObjectConcentration);
    EXPECT_LT(framing->flatness, SplatSiteFlatness);
    // The 10th to 90th percentiles of the box from which its outermost 1% are left out
    expectCoreWithin(*framing, {-3.9, -1.56, -3.9}, {3.9, 1.56, 3.9}, 0.15);
}

// A room as a capture from inside it sees it: its walls, floor and ceiling, nothing in the middle
TEST(splatFraming, theWallsOfARoomAreAScene) {
    const Scene inside = room({0, -1, 0}, 8.0, 3.0, RoomSplats{3000, 1500, 2500, 1200, 1000});
    const auto framing = estimateSplatFraming(inside.positions, inside.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Scene);
}

// A piece of ground seen from the air is wide and flat: it is seen whole, from above
TEST(splatFraming, aWideFlatGroundIsASiteFramedWhole) {
    const Scene site = groundWithObjects({0, 0, 1}, 6000, 1500, 200.0, 6.0);
    const auto framing = estimateSplatFraming(site.positions, site.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Site);
    EXPECT_GE(framing->flatness, SplatSiteFlatness);
    // All of it but the outermost 1% of each axis
    EXPECT_NEAR(framing->coreMin[0], -98.0, 2.0);
    EXPECT_NEAR(framing->coreMax[0], 98.0, 2.0);
    EXPECT_NEAR(framing->coreMin[1], -98.0, 2.0);
    EXPECT_NEAR(framing->coreMax[1], 98.0, 2.0);
}

// A village seen from a drone that circled its centre is denser in the middle, but it is ground: it
// is a site, not an object to be seen from beside it
TEST(splatFraming, aFlatSiteDenserInTheMiddleIsStillASite) {
    Scene site = groundWithObjects({0, 0, 1}, 6000, 1500, 200.0, 6.0);
    std::mt19937 rng(9);
    addBox(site, rng, {-15.0, -15.0, 0.0}, {15.0, 15.0, 3.0}, 12000);
    const auto framing = estimateSplatFraming(site.positions, site.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_LT(framing->concentration, SplatObjectConcentration);
    EXPECT_EQ(framing->kind, SplatCaptureKind::Site);
}

// A georeferenced scene is a piece of the Earth, whatever its splats look like
TEST(splatFraming, aGeoreferencedSceneIsASite) {
    const Scene scene = objectCapture();
    const auto framing = estimateSplatFraming(scene.positions, scene.opacities, /*georeferenced=*/true);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Site);
}

// The opacity is the weight: a haze of faint splats far from the subject does not make it a scene
TEST(splatFraming, faintSplatsWeighLess) {
    Scene scene = objectCapture();
    std::mt19937 rng(13);
    Scene haze;
    addBox(haze, rng, {-60.0, -60.0, -60.0}, {60.0, 60.0, 60.0}, 20000);
    for (size_t i = 0; i < haze.opacities.size(); ++i)
        scene.add({haze.positions[i * 3], haze.positions[i * 3 + 1], haze.positions[i * 3 + 2]}, 0.06f);
    const auto framing = estimateSplatFraming(scene.positions, scene.opacities);
    ASSERT_TRUE(framing.has_value());
    EXPECT_EQ(framing->kind, SplatCaptureKind::Object);
}

// Too few splats, or splats with no extent, are not a scene to frame: the viewer frames on its own
TEST(splatFraming, tooFewSplatsOrNoExtentGiveNoFraming) {
    EXPECT_FALSE(estimateSplatFraming(std::vector<float>{}, std::vector<float>{}).has_value());
    Scene few;
    for (int i = 0; i < 5; ++i) few.add({double(i), 0, 0});
    EXPECT_FALSE(estimateSplatFraming(few.positions, few.opacities).has_value());
    Scene point;
    for (int i = 0; i < 100; ++i) point.add({1, 2, 3});
    EXPECT_FALSE(estimateSplatFraming(point.positions, point.opacities).has_value());
}

// The member bounds.json gets, named as the viewers read it
TEST(splatFraming, theSidecarMemberIsNamedAsTheViewersReadIt) {
    SplatFraming framing;
    framing.kind = SplatCaptureKind::Object;
    framing.coreMin = {-1.23456789, 0.0, 2.0};
    framing.coreMax = {1.0, 3.5, 4.0};
    const json j = framing;
    ASSERT_EQ(j.size(), 1u);
    ASSERT_TRUE(j.contains("framing"));
    EXPECT_EQ(j["framing"]["kind"], "object");
    EXPECT_EQ(j["framing"]["core"], (json::array({-1.234568, 0.0, 2.0, 1.0, 3.5, 4.0})));

    EXPECT_STREQ(splatCaptureKindName(SplatCaptureKind::Scene), "scene");
    EXPECT_STREQ(splatCaptureKindName(SplatCaptureKind::Site), "site");
}

}  // namespace
