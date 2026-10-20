/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-00: the Windows CI crashed (0xC0000005) inside GEOS KdTree::query, reached through
// HotPixelIndex::addRounded <- SnapRoundingNoder <- OverlayNG, called from GDAL's MVT
// writer. Cause: GEOS issue #1449, an MSVC 19.51 optimizer bug in KdTree::queryNodePoint,
// fixed upstream in libgeos PR 1453 and shipped in GEOS 3.15.0.
//
// These tests drive the same GEOS code path through GDAL/OGR: OGRGeometry::SetPrecision
// reduces polygonal geometries with a fixed-precision OverlayNG union, which nodes through
// SnapRoundingNoder -> HotPixelIndex -> KdTree::query (query by point). (Linear inputs are
// only rounded pointwise, so every case here is polygonal.) Built with
// MSVC 19.51 against GEOS 3.14.1 they crash; on any other toolchain they pass and pin the
// expected snap-rounded geometry.

#include <cmath>
#include <memory>
#include <string>

#include "gtest/gtest.h"

#include "mathutil.h"
#include "ogr_api.h"
#include "ogr_geometry.h"

namespace
{
    std::unique_ptr<OGRGeometry> fromWkt(const char *wkt)
    {
        OGRGeometry *geom = nullptr;
        if (OGRGeometryFactory::createFromWkt(wkt, nullptr, &geom) != OGRERR_NONE)
            return nullptr;
        return std::unique_ptr<OGRGeometry>(geom);
    }

    std::string toWkt(const OGRGeometry &geom)
    {
        return geom.exportToWkt();
    }

    // OGRGeometry::Equals compares structure (vertex order and start point); snap rounding
    // only guarantees the same point set, so compare topologically through GEOS.
    bool topologicallyEqual(const OGRGeometry &a, const OGRGeometry &b)
    {
        return a.Within(&b) && b.Within(&a);
    }

    // True when every vertex of a line or polygon geometry lies on the grid.
    bool allVerticesOnGrid(const OGRGeometry &geom, double gridSize)
    {
        bool onGrid = true;
        const auto check = [&](const OGRSimpleCurve &curve)
        {
            for (const auto &p : curve)
            {
                const double gx = p.getX() / gridSize;
                const double gy = p.getY() / gridSize;
                if (std::fabs(gx - std::round(gx)) > 1e-9 || std::fabs(gy - std::round(gy)) > 1e-9)
                    onGrid = false;
            }
        };
        const auto visit = [&](const OGRGeometry &g, const auto &self) -> void
        {
            const auto type = wkbFlatten(g.getGeometryType());
            if (type == wkbLineString)
                check(*g.toLineString());
            else if (type == wkbPolygon)
                for (const auto *ring : *g.toPolygon())
                    check(*ring);
            else if (OGR_GT_IsSubClassOf(type, wkbGeometryCollection))
                for (const auto *part : *g.toGeometryCollection())
                    self(*part, self);
        };
        visit(geom, visit);
        return onGrid;
    }

    TEST(Dq00GeosKdTree, Target_GeosRuntimeCarriesTheKdTreeFix)
    {
        ASSERT_TRUE(OGRGeometryFactory::haveGEOS()) << "GDAL was built without GEOS";

        int major = 0, minor = 0, patch = 0;
        ASSERT_TRUE(OGRGetGEOSVersion(&major, &minor, &patch));

        // GEOS 3.15.0 is the first release with libgeos PR 1453 (issue #1449).
        EXPECT_TRUE(major > 3 || (major == 3 && minor >= 15))
            << "GEOS " << major << "." << minor << "." << patch
            << " predates the KdTree::queryNodePoint fix (GEOS >= 3.15.0 required)";
    }

    TEST(Dq00GeosKdTree, Target_SnapRoundingNodesCollapsingHoleEdges)
    {
        // A hole 0.4 units inside the shell: on a unit grid its edges snap onto the shell
        // edges, so the noder must split the coincident edges at shared hot pixels and the
        // hole opens into an L-shaped polygon.
        auto polygon = fromWkt("POLYGON ((0 0,10 0,10 10,0 10,0 0),"
                               "(0.4 0.4,0.4 5.1,4.9 5.1,4.9 0.4,0.4 0.4))");
        ASSERT_NE(polygon, nullptr);
        ASSERT_TRUE(polygon->IsValid());

        std::unique_ptr<OGRGeometry> reduced(polygon->SetPrecision(1.0, 0));
        ASSERT_NE(reduced, nullptr);
        EXPECT_TRUE(reduced->IsValid()) << toWkt(*reduced);
        EXPECT_TRUE(allVerticesOnGrid(*reduced, 1.0)) << toWkt(*reduced);

        auto expected = fromWkt("POLYGON ((0 5,5 5,5 0,10 0,10 10,0 10,0 5))");
        ASSERT_NE(expected, nullptr);
        EXPECT_TRUE(topologicallyEqual(*reduced, *expected)) << toWkt(*reduced);
    }

    TEST(Dq00GeosKdTree, Target_SnapRoundingCollapsesNearCoincidentVertices)
    {
        // Vertices a hair apart fall into the same hot pixel, so the KdTree point query
        // finds the existing node instead of inserting a new one.
        auto polygon = fromWkt("POLYGON ((0 0,10 0,10.0000004 0.0000003,10.0000001 9.9999998,"
                               "10 10,0.0000002 10.0000001,0 10,0 0))");
        ASSERT_NE(polygon, nullptr);

        std::unique_ptr<OGRGeometry> reduced(polygon->SetPrecision(0.001, 0));
        ASSERT_NE(reduced, nullptr);
        EXPECT_TRUE(reduced->IsValid()) << toWkt(*reduced);
        EXPECT_TRUE(allVerticesOnGrid(*reduced, 0.001)) << toWkt(*reduced);

        auto expected = fromWkt("POLYGON ((0 0,10 0,10 10,0 10,0 0))");
        ASSERT_NE(expected, nullptr);
        EXPECT_TRUE(topologicallyEqual(*reduced, *expected)) << toWkt(*reduced);
        EXPECT_EQ(reduced->toPolygon()->getExteriorRing()->getNumPoints(), 5) << toWkt(*reduced);
    }

    TEST(Dq00GeosKdTree, Target_SnapRoundingDenseRingFillsTheKdTree)
    {
        // A dense, slightly wobbly ring: thousands of vertices share hot pixels, which
        // builds a deep KdTree and queries it once per vertex - the crash site in CI.
        constexpr int nVertices = 4000;
        constexpr double radius = 100.0;
        OGRLinearRing ring;
        for (int i = 0; i < nVertices; i++)
        {
            const double a = 2.0 * ddb::kPi * i / nVertices;
            const double r = radius + 0.3 * std::sin(37.0 * a);
            ring.addPoint(r * std::cos(a), r * std::sin(a));
        }
        ring.closeRings();
        OGRPolygon polygon;
        polygon.addRing(&ring);
        ASSERT_TRUE(polygon.IsValid());

        std::unique_ptr<OGRGeometry> reduced(polygon.SetPrecision(0.5, 0));
        ASSERT_NE(reduced, nullptr);
        ASSERT_EQ(wkbFlatten(reduced->getGeometryType()), wkbPolygon) << toWkt(*reduced);
        EXPECT_TRUE(reduced->IsValid());
        EXPECT_TRUE(allVerticesOnGrid(*reduced, 0.5));

        // Rounding to a 0.5 grid moves the boundary by at most ~0.35, so the area stays
        // within 1% of the original.
        const double originalArea = polygon.get_Area();
        const double reducedArea = reduced->toPolygon()->get_Area();
        EXPECT_NEAR(reducedArea, originalArea, originalArea * 0.01);
        EXPECT_LT(reduced->toPolygon()->getExteriorRing()->getNumPoints(), nVertices);
    }
} // namespace
