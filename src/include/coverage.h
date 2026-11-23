/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COVERAGE_H
#define COVERAGE_H

// Which images of a dataset see a place: the images whose footprint intersects a point, a line or a
// polygon, best view first.

#include <optional>
#include <string>
#include <vector>

#include "basicgeometry.h"
#include "database.h"
#include "json.h"

namespace ddb
{

    /// A geometry to look for in the footprints: GeoJSON Point, LineString or Polygon in EPSG:4326
    struct CoverageGeometry
    {
        enum class Kind
        {
            Point,
            Line,
            Polygon
        };

        Kind kind = Kind::Point;
        /// A point: its position; a line: its positions; a polygon: its rings, the outer one first, each closed.
        /// x is the longitude and y the latitude
        std::vector<std::vector<Point>> parts;

        std::string toWkt() const;
    };

    /// The geometry of a GeoJSON geometry object. Throws InvalidArgsException for what is not a
    /// Point, LineString or Polygon, a position out of the range of longitude and latitude, or a ring
    /// of a polygon with fewer than three positions (a ring that is not closed is closed)
    CoverageGeometry parseCoverageGeometry(const json &geoJson);

    struct CoverageOptions
    {
        static constexpr int DefaultLimit = 20;
        static constexpr int MaxLimit = 100;

        int limit = DefaultLimit;
    };

    /// The options of a coverage search from a JSON object {"limit": 1..100}; every member is optional.
    /// Throws InvalidArgsException for a member that is not known or a limit that is not a whole number
    /// from 1 to MaxLimit
    CoverageOptions parseCoverageOptions(const json &options);

    struct CoverageMatch
    {
        std::string path;
        /// How far the geometry is from the middle of the picture: the distance of the centroid of the
        /// geometry to the centroid of the footprint, in metres, over the diagonal of the footprint. The
        /// lower the better; 0 is a picture whose middle is the geometry
        double score = 0.0;
        /// The part of a polygon that the footprint covers (area of the intersection over the area of
        /// the polygon); 1 for a point or a line that intersects it
        double overlap = 1.0;
        /// The pixel of the centroid of the geometry in the image, when the camera parameters of the
        /// image tell it and the point is in the image
        std::optional<std::pair<int, int>> pixel;
    };

    /// The images of the index whose footprint intersects the geometry: at most options.limit, in
    /// ascending order of score and then of path
    std::vector<CoverageMatch> findCoverage(Database *db, const CoverageGeometry &geometry,
                                            const CoverageOptions &options);

    /// [{"path", "score", "overlap", "pixel": [x, y] | null}]
    json coverageToJson(const std::vector<CoverageMatch> &matches);

}

#endif // COVERAGE_H
