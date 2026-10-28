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

// A ground of `side` metres square with a little roughness, and things standing on it: columns of
// splats that rise up to `height` from points of the ground. `upAxis` is the axis (with sign) the
// scene stands on, the frame the trainer chose.
Scene groundWithObjects(const Vec3& upAxis, int ground = 3000, int objects = 1500, double side = 20.0,
                        double height = 4.0) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> across(-side / 2, side / 2), rise(0.0, height);
    std::normal_distribution<double> rough(0.0, 0.03);

    // Two axes spanning the ground: the cross products of up with the axis it is not
    const Vec3 other = std::abs(upAxis[0]) < 0.5 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 u = {upAxis[1] * other[2] - upAxis[2] * other[1], upAxis[2] * other[0] - upAxis[0] * other[2],
              upAxis[0] * other[1] - upAxis[1] * other[0]};
    const double un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (double& c : u) c /= un;
    const Vec3 v = {upAxis[1] * u[2] - upAxis[2] * u[1], upAxis[2] * u[0] - upAxis[0] * u[2],
                    upAxis[0] * u[1] - upAxis[1] * u[0]};

    Scene scene;
    const auto at = [&](double a, double b, double h) {
        return Vec3{u[0] * a + v[0] * b + upAxis[0] * h, u[1] * a + v[1] * b + upAxis[1] * h,
                    u[2] * a + v[2] * b + upAxis[2] * h};
    };
    for (int i = 0; i < ground; ++i)
        scene.add(at(across(rng), across(rng), rough(rng)));
    // Objects: a few clusters, each a column
    for (int c = 0; c < 6; ++c) {
        const double cx = across(rng) * 0.8, cy = across(rng) * 0.8;
        for (int i = 0; i < objects / 6; ++i)
            scene.add(at(cx + rough(rng) * 20, cy + rough(rng) * 20, rise(rng)));
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

}  // namespace
