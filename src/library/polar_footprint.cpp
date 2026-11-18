/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "polar_footprint.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "coordstransformer.h"
#include "cpl_error.h"

namespace ddb
{
    namespace
    {
        // How far outside its box a pole may fall and still be in it, in grid units (GDAL's own margin)
        constexpr double kGridMargin = 1e-5;
        // The points of each side of the box between its corners, as GDAL densifies a box
        constexpr int kDensify = 21;
        // A corner within this of the latitude of a pole is at the pole (degrees, about a centimetre)
        constexpr double kPoleTolerance = 1e-7;

        bool mapPoint(const GridMapping &mapping, double &x, double &y)
        {
            return !mapping || mapping(x, y);
        }

        // Whether the pole at `latitude` of the geographic CRS lies in the box
        bool holdsPole(const GridBox &box, CoordsTransformer &fromLonLat, double latitude)
        {
            double x = 0.0, y = latitude;
            if (!fromLonLat.tryTransform(1, &x, &y) || !std::isfinite(x) || !std::isfinite(y) ||
                !mapPoint(box.fromCrs, x, y) || !std::isfinite(x) || !std::isfinite(y))
                return false;
            return x >= box.minX - kGridMargin && x <= box.maxX + kGridMargin && y >= box.minY - kGridMargin &&
                   y <= box.maxY + kGridMargin;
        }

        // The latitudes of the edges of the box, the corners first: each side from one corner to the
        // next in kDensify + 1 steps. The points that do not transform are left out; NaN for a corner
        // that does not.
        struct EdgeLatitudes
        {
            std::vector<double> corners;
            std::vector<double> all;
        };

        EdgeLatitudes edgeLatitudes(const GridBox &box, CoordsTransformer &toLonLat)
        {
            const int steps = kDensify + 1;
            const double corners[5][2] = {{box.minX, box.minY}, {box.maxX, box.minY}, {box.maxX, box.maxY},
                                          {box.minX, box.maxY}, {box.minX, box.minY}};
            std::vector<double> xs, ys;
            std::vector<bool> mapped;
            for (int side = 0; side < 4; side++)
            {
                for (int i = 0; i < steps; i++)
                {
                    const double t = static_cast<double>(i) / steps;
                    double x = corners[side][0] + (corners[side + 1][0] - corners[side][0]) * t;
                    double y = corners[side][1] + (corners[side + 1][1] - corners[side][1]) * t;
                    const bool ok = mapPoint(box.toCrs, x, y) && std::isfinite(x) && std::isfinite(y);
                    xs.push_back(ok ? x : 0.0);
                    ys.push_back(ok ? y : 0.0);
                    mapped.push_back(ok);
                }
            }
            // A point that does not transform comes back as HUGE_VAL
            toLonLat.transform(xs.size(), xs.data(), ys.data());

            EdgeLatitudes out;
            for (size_t i = 0; i < ys.size(); i++)
            {
                const bool ok = mapped[i] && std::isfinite(xs[i]) && std::isfinite(ys[i]) && xs[i] != HUGE_VAL &&
                                ys[i] != HUGE_VAL;
                if (ok)
                    out.all.push_back(ys[i]);
                if (i % static_cast<size_t>(steps) == 0)
                    out.corners.push_back(ok ? ys[i] : std::nan(""));
            }
            return out;
        }

        bool aCornerIsAt(const EdgeLatitudes &edges, double poleLatitude)
        {
            return std::any_of(edges.corners.begin(), edges.corners.end(),
                               [poleLatitude](double lat) { return std::abs(lat - poleLatitude) <= kPoleTolerance; });
        }
    } // namespace

    std::optional<LonLatEnvelope> polarEnvelope(const GridBox &box, const OGRSpatialReference &crs,
                                                const OGRSpatialReference &lonLat)
    {
        // A pole outside the domain of the projection (a Mercator) fails to transform: not an error
        CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

        const auto fromLonLat = CoordsTransformer::tryCreate(lonLat, crs);
        const auto toLonLat = CoordsTransformer::tryCreate(crs, lonLat);
        if (!fromLonLat || !toLonLat)
            return std::nullopt;

        bool north = holdsPole(box, *fromLonLat, 90.0);
        bool south = holdsPole(box, *fromLonLat, -90.0);
        if (!north && !south)
            return std::nullopt;

        const EdgeLatitudes edges = edgeLatitudes(box, *toLonLat);
        if (edges.all.empty())
            return std::nullopt;
        north = north && !aCornerIsAt(edges, 90.0);
        south = south && !aCornerIsAt(edges, -90.0);
        if (!north && !south)
            return std::nullopt;

        const auto [lowest, highest] = std::minmax_element(edges.all.begin(), edges.all.end());
        LonLatEnvelope envelope;
        envelope.minLon = -180.0;
        envelope.maxLon = 180.0;
        envelope.minLat = south ? -90.0 : std::max(-90.0, *lowest);
        envelope.maxLat = north ? 90.0 : std::min(90.0, *highest);
        return envelope;
    }
}
