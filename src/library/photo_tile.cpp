/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "photo_tile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tiler.h"

namespace ddb::raster
{
    std::array<Projected2D, 4> photoCornersInMeters(const PhotoGeoreference &photo)
    {
        std::array<Projected2D, 4> corners;
        for (size_t i = 0; i < corners.size(); i++)
            corners[i] = GlobalMercator::lonLatToMeters(photo.footprint[i][0], photo.footprint[i][1]);
        return corners;
    }

    BoundingBox<Projected2D> photoBoundsInMeters(const PhotoGeoreference &photo)
    {
        const auto corners = photoCornersInMeters(photo);
        BoundingBox<Projected2D> box(corners[0], corners[0]);
        for (const Projected2D &c : corners)
        {
            box.min.x = std::min(box.min.x, c.x);
            box.min.y = std::min(box.min.y, c.y);
            box.max.x = std::max(box.max.x, c.x);
            box.max.y = std::max(box.max.y, c.y);
        }
        return box;
    }

    double photoResolution(const PhotoGeoreference &photo)
    {
        // The pixel (0, 0) is the upper left corner of the footprint, (width, height) the lower right
        const auto corners = photoCornersInMeters(photo);
        const double ground = std::hypot(corners[2].x - corners[0].x, corners[2].y - corners[0].y);
        return ground / std::hypot(static_cast<double>(photo.width), static_cast<double>(photo.height));
    }

    BoundingBox<Projected2D> photoTileBounds(const PhotoTileParams &params)
    {
        // GlobalMercator counts its rows from the south (TMS)
        const int last = static_cast<int>((int64_t{1} << params.z) - 1);
        const int row = params.tms ? params.y : last - params.y;
        return GlobalMercator(params.tileSize).tileBounds(params.x, row, params.z);
    }

    namespace
    {
        // The sign of the turn from a to b to c: > 0 counterclockwise, < 0 clockwise, 0 on a line
        double turn(const Projected2D &a, const Projected2D &b, const Projected2D &c)
        {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }

        // Whether the segments ab and cd share a point (touching counts)
        bool segmentsMeet(const Projected2D &a, const Projected2D &b, const Projected2D &c, const Projected2D &d)
        {
            const double d1 = turn(c, d, a), d2 = turn(c, d, b), d3 = turn(a, b, c), d4 = turn(a, b, d);
            if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
                return true;
            const auto onSegment = [](const Projected2D &p, const Projected2D &q, const Projected2D &r) {
                return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y &&
                       r.y <= std::max(p.y, q.y);
            };
            return (d1 == 0 && onSegment(c, d, a)) || (d2 == 0 && onSegment(c, d, b)) ||
                   (d3 == 0 && onSegment(a, b, c)) || (d4 == 0 && onSegment(a, b, d));
        }

        // Whether p is inside the polygon (even-odd rule): a footprint of bad EXIF may cross itself
        bool insidePolygon(const std::array<Projected2D, 4> &polygon, const Projected2D &p)
        {
            bool inside = false;
            for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
            {
                const Projected2D &a = polygon[i], &b = polygon[j];
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                    inside = !inside;
            }
            return inside;
        }
    } // namespace

    bool footprintMeetsBox(const PhotoGeoreference &photo, const BoundingBox<Projected2D> &box, double margin)
    {
        const BoundingBox<Projected2D> grown(Projected2D(box.min.x - margin, box.min.y - margin),
                                             Projected2D(box.max.x + margin, box.max.y + margin));
        const auto footprint = photoCornersInMeters(photo);
        const std::array<Projected2D, 4> rectangle{grown.min, Projected2D(grown.min.x, grown.max.y), grown.max,
                                                   Projected2D(grown.max.x, grown.min.y)};

        // A corner of either inside the other, or two edges that cross
        for (const Projected2D &corner : footprint)
            if (corner.x >= grown.min.x && corner.x <= grown.max.x && corner.y >= grown.min.y &&
                corner.y <= grown.max.y)
                return true;
        for (const Projected2D &corner : rectangle)
            if (insidePolygon(footprint, corner))
                return true;
        for (size_t i = 0; i < footprint.size(); i++)
            for (size_t j = 0; j < rectangle.size(); j++)
                if (segmentsMeet(footprint[i], footprint[(i + 1) % footprint.size()], rectangle[j],
                                 rectangle[(j + 1) % rectangle.size()]))
                    return true;
        return false;
    }

} // namespace ddb::raster
