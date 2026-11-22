/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "coverage.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>

#include "camera_model.h"
#include "entry.h"
#include "entry_types.h"
#include "exceptions.h"
#include "geo.h"
#include "index_sql.h"
#include "json_options.h"
#include "logger.h"
#include "mathutil.h"

namespace ddb
{

    namespace
    {
        // The metres of a part of a geometry, in the UTM zone of a projector
        using Planar = std::vector<Projected2D>;

        std::string positionWkt(const Point &p)
        {
            std::ostringstream out;
            out << std::setprecision(17) << p.x << ' ' << p.y;
            return out.str();
        }

        std::string listWkt(const std::vector<Point> &positions)
        {
            std::string wkt = "(";
            for (size_t i = 0; i < positions.size(); i++)
                wkt += (i == 0 ? "" : ", ") + positionWkt(positions[i]);
            return wkt + ")";
        }

        double number(const json &value, const char *what)
        {
            if (!value.is_number())
                throw InvalidArgsException(std::string("A ") + what + " must be a number");
            const double v = value.get<double>();
            if (!std::isfinite(v))
                throw InvalidArgsException(std::string("A ") + what + " must be a finite number");
            return v;
        }

        // A GeoJSON position [longitude, latitude, ...]: the height, when there is one, is not used
        Point positionOf(const json &position)
        {
            if (!position.is_array() || position.size() < 2)
                throw InvalidArgsException("A position must be an array of a longitude and a latitude");
            const double longitude = number(position[0], "longitude");
            const double latitude = number(position[1], "latitude");
            if (longitude < -180.0 || longitude > 180.0)
                throw InvalidArgsException("A longitude must be from -180 to 180");
            if (latitude < -90.0 || latitude > 90.0)
                throw InvalidArgsException("A latitude must be from -90 to 90");
            return Point(longitude, latitude);
        }

        std::vector<Point> positionsOf(const json &list, size_t minimum, const char *what)
        {
            if (!list.is_array() || list.size() < minimum)
                throw InvalidArgsException(std::string("A ") + what + " needs at least " + std::to_string(minimum) +
                                           " positions");
            std::vector<Point> positions;
            positions.reserve(list.size());
            for (const auto &position : list)
                positions.push_back(positionOf(position));
            return positions;
        }

        // The planar centroid of a geometry that is already in metres: the area centroid of a polygon (its
        // holes take away), the length centroid of a line, a point itself. A geometry with no area or no
        // length is the mean of its positions.
        Projected2D centroidOf(CoverageGeometry::Kind kind, const std::vector<Planar> &parts)
        {
            double weight = 0.0;
            double sumX = 0.0;
            double sumY = 0.0;

            if (kind == CoverageGeometry::Kind::Polygon)
            {
                for (size_t r = 0; r < parts.size(); r++)
                {
                    const Planar &ring = parts[r];
                    double twiceArea = 0.0;
                    double cx = 0.0;
                    double cy = 0.0;
                    for (size_t i = 0; i + 1 < ring.size(); i++)
                    {
                        const double cross = ring[i].x * ring[i + 1].y - ring[i + 1].x * ring[i].y;
                        twiceArea += cross;
                        cx += (ring[i].x + ring[i + 1].x) * cross;
                        cy += (ring[i].y + ring[i + 1].y) * cross;
                    }
                    if (twiceArea == 0.0)
                        continue;
                    // The centroid of the ring, weighed by its area; the sign of the ring does not matter
                    const double area = std::abs(twiceArea) / 2.0;
                    const double sign = r == 0 ? 1.0 : -1.0;
                    weight += sign * area;
                    sumX += sign * area * cx / (3.0 * twiceArea);
                    sumY += sign * area * cy / (3.0 * twiceArea);
                }
            }
            else if (kind == CoverageGeometry::Kind::Line)
            {
                const Planar &line = parts.front();
                for (size_t i = 0; i + 1 < line.size(); i++)
                {
                    const double length = std::hypot(line[i + 1].x - line[i].x, line[i + 1].y - line[i].y);
                    weight += length;
                    sumX += length * (line[i].x + line[i + 1].x) / 2.0;
                    sumY += length * (line[i].y + line[i + 1].y) / 2.0;
                }
            }

            if (weight > 0.0)
                return Projected2D(sumX / weight, sumY / weight);

            const Planar &first = parts.front();
            size_t count = first.size();
            // A closed ring repeats its first position
            if (kind == CoverageGeometry::Kind::Polygon && count > 1)
                count--;
            Projected2D mean(0.0, 0.0);
            for (size_t i = 0; i < count; i++)
            {
                mean.x += first[i].x / static_cast<double>(count);
                mean.y += first[i].y / static_cast<double>(count);
            }
            return mean;
        }

        Planar project(const UtmProjector &projector, const std::vector<Point> &positions)
        {
            Planar planar;
            planar.reserve(positions.size());
            for (const auto &p : positions)
                planar.push_back(projector.toUTM(p.y, p.x));
            return planar;
        }

        // The box of a geometry, in longitude and latitude
        struct Box
        {
            double minX = 180.0;
            double maxX = -180.0;
            double minY = 90.0;
            double maxY = -90.0;

            Point center() const { return Point((minX + maxX) / 2.0, (minY + maxY) / 2.0); }
        };

        Box boxOf(const CoverageGeometry &geometry)
        {
            Box box;
            for (const auto &part : geometry.parts)
                for (const auto &p : part)
                {
                    box.minX = std::min(box.minX, p.x);
                    box.maxX = std::max(box.maxX, p.x);
                    box.minY = std::min(box.minY, p.y);
                    box.maxY = std::max(box.maxY, p.y);
                }
            return box;
        }

        // The projectors of the zones met by a search, one per zone: building one is the dearest part of
        // projecting a few points
        class Projectors
        {
        public:
            const UtmProjector &of(const UTMZone &zone)
            {
                const int key = zone.zone * (zone.north ? 1 : -1);
                auto it = projectors_.find(key);
                if (it == projectors_.end())
                    it = projectors_.emplace(key, std::make_unique<UtmProjector>(zone)).first;
                return *it->second;
            }

        private:
            std::map<int, std::unique_ptr<UtmProjector>> projectors_;
        };

        // The diagonal of a footprint: its longest distance between two corners
        double diagonalOf(const Planar &ring)
        {
            double longest = 0.0;
            for (size_t i = 0; i < ring.size(); i++)
                for (size_t j = i + 1; j < ring.size(); j++)
                    longest = std::max(longest, std::hypot(ring[j].x - ring[i].x, ring[j].y - ring[i].y));
            return longest;
        }

        // The image position of a geometry centroid (longitude and latitude) in an image, from what the index
        // keeps of it: the properties of its camera, the footprint and the point it was taken at
        std::optional<std::pair<int, int>> pixelOf(const Entry &entry, const Point &centroid, Projectors &projectors)
        {
            if (entry.point_geom.empty() || entry.polygon_geom.empty())
                return std::nullopt;

            const Point camera = entry.point_geom.points.front();
            const double relAltitude = CameraModel::relAltitudeOf(camera.z, entry.polygon_geom.points.front().z);
            const auto model = CameraModel::fromProperties(entry.properties, relAltitude);
            if (!model)
                return std::nullopt;

            // The footprint is built in the grid of the zone of the camera, and this is its inverse
            const UtmProjector &projector = projectors.of(getUTMZone(camera.y, camera.x));
            const Projected2D under = projector.toUTM(camera.y, camera.x);
            const Projected2D ground = projector.toUTM(centroid.y, centroid.x);
            const auto position = model->imagePosition(ground.x - under.x, ground.y - under.y);
            if (!position)
                return std::nullopt;

            // The pixel that holds the position; a position on the far edge is in the last pixel
            return std::make_pair(std::clamp(static_cast<int>(std::floor(position->x)), 0, model->imageWidth() - 1),
                                  std::clamp(static_cast<int>(std::floor(position->y)), 0, model->imageHeight() - 1));
        }

        // Where the SQL of the search tests a footprint against the geometry: the box test first (it is
        // cheap), then the exact one. Intersects is -1 when GEOS fails, which only "= 1" treats as no.
        constexpr const char *kCandidatesWhere =
            " WHERE e.type = ? AND e.polygon_geom IS NOT NULL "
            "AND MbrIntersects(e.polygon_geom, BuildMbr(?, ?, ?, ?, 4326)) "
            "AND Intersects(e.polygon_geom, GeomFromText(?, 4326)) = 1";

        void requireValid(Database *db, const std::string &wkt)
        {
            auto q = db->query("SELECT IsValid(GeomFromText(?, 4326))");
            q->bind(1, wkt);
            if (!q->fetch() || q->getInt(0) != 1)
                throw InvalidArgsException("The geometry is not valid");
        }

        // The part of a polygon that the footprint of an entry covers. The areas are those of the geometries
        // with the longitude scaled by the cosine of the latitude of the polygon (xScale): a plane in which a
        // degree is as long both ways, so the ratio of the areas is the ratio of the ground
        double overlapOf(Database *db, const std::string &path, const std::string &wkt, double xScale)
        {
            auto q = db->query("SELECT Area(Intersection(ScaleCoordinates(polygon_geom, ?1, 1.0), "
                               "ScaleCoordinates(GeomFromText(?2, 4326), ?1, 1.0))), "
                               "Area(ScaleCoordinates(GeomFromText(?2, 4326), ?1, 1.0)) FROM entries WHERE path = ?3");
            q->bind(1, xScale);
            q->bind(2, wkt);
            q->bind(3, path);
            if (!q->fetch())
                return 0.0;
            const double common = q->getDouble(0);
            const double whole = q->getDouble(1);
            // A polygon with no area (a sliver) is covered when it intersects
            if (!(whole > 0.0))
                return 1.0;
            return std::clamp(common / whole, 0.0, 1.0);
        }
    } // namespace

    std::string CoverageGeometry::toWkt() const
    {
        switch (kind)
        {
        case Kind::Point:
            return "POINT(" + positionWkt(parts.front().front()) + ")";
        case Kind::Line:
            return "LINESTRING" + listWkt(parts.front());
        case Kind::Polygon:
        {
            std::string wkt = "POLYGON(";
            for (size_t i = 0; i < parts.size(); i++)
                wkt += (i == 0 ? "" : ", ") + listWkt(parts[i]);
            return wkt + ")";
        }
        }
        return {};
    }

    CoverageGeometry parseCoverageGeometry(const json &geoJson)
    {
        if (!geoJson.is_object())
            throw InvalidArgsException("The geometry must be a GeoJSON geometry object");
        const auto type = geoJson.find("type");
        if (type == geoJson.end() || !type->is_string())
            throw InvalidArgsException("The geometry has no type");
        const auto coordinates = geoJson.find("coordinates");
        if (coordinates == geoJson.end())
            throw InvalidArgsException("The geometry has no coordinates");

        CoverageGeometry geometry;
        const auto name = type->get<std::string>();
        if (name == "Point")
        {
            geometry.kind = CoverageGeometry::Kind::Point;
            geometry.parts.push_back({positionOf(*coordinates)});
        }
        else if (name == "LineString")
        {
            geometry.kind = CoverageGeometry::Kind::Line;
            geometry.parts.push_back(positionsOf(*coordinates, 2, "line"));
        }
        else if (name == "Polygon")
        {
            geometry.kind = CoverageGeometry::Kind::Polygon;
            if (!coordinates->is_array() || coordinates->empty())
                throw InvalidArgsException("A polygon needs at least one ring");
            for (const auto &ring : *coordinates)
            {
                auto positions = positionsOf(ring, 3, "ring");
                const Point &first = positions.front();
                const Point &last = positions.back();
                if (first.x != last.x || first.y != last.y)
                    positions.push_back(first);
                if (positions.size() < 4)
                    throw InvalidArgsException("A ring needs at least 3 positions");
                geometry.parts.push_back(std::move(positions));
            }
        }
        else
        {
            throw InvalidArgsException("Unsupported geometry type \"" + name + "\": use Point, LineString or Polygon");
        }
        return geometry;
    }

    CoverageOptions parseCoverageOptions(const json &options)
    {
        CoverageOptions parsed;
        if (!options.is_object())
            throw InvalidArgsException("The coverage options must be a JSON object");
        rejectUnknownMembers(options, {"limit"}, "coverage option");

        const auto limit = options.find("limit");
        if (limit != options.end())
        {
            const std::string rule = "\"limit\" must be a whole number from 1 to " + std::to_string(CoverageOptions::MaxLimit);
            if (!limit->is_number_integer())
                throw InvalidArgsException(rule);
            // An unsigned value beyond int64 is refused like any value above the maximum
            const int64_t value = limit->is_number_unsigned()
                                      ? static_cast<int64_t>(std::min<uint64_t>(limit->get<uint64_t>(), INT64_MAX))
                                      : limit->get<int64_t>();
            if (value < 1 || value > CoverageOptions::MaxLimit)
                throw InvalidArgsException(rule);
            parsed.limit = static_cast<int>(value);
        }
        return parsed;
    }

    std::vector<CoverageMatch> findCoverage(Database *db, const CoverageGeometry &geometry,
                                            const CoverageOptions &options)
    {
        const std::string wkt = geometry.toWkt();
        requireValid(db, wkt);

        // The distances are in metres, in the UTM zone of the middle of the geometry
        Projectors projectors;
        const Box box = boxOf(geometry);
        const Point middle = box.center();
        const UtmProjector &frame = projectors.of(getUTMZone(middle.y, middle.x));

        std::vector<Planar> parts;
        for (const auto &part : geometry.parts)
            parts.push_back(project(frame, part));
        const Projected2D centre = centroidOf(geometry.kind, parts);
        const Geographic2D centroid = frame.fromUTM(centre.x, centre.y);

        // The footprints that intersect it. The rows are read whole: the properties and the point are what
        // the pixel of the best of them needs
        auto q = db->query(selectEntries(EntryColumns::Geometries) + kCandidatesWhere);
        q->bind(1, entryTypeValue(EntryType::GeoImage));
        q->bind(2, box.minX);
        q->bind(3, box.minY);
        q->bind(4, box.maxX);
        q->bind(5, box.maxY);
        q->bind(6, wkt);

        struct Candidate
        {
            double score;
            Entry entry;
        };
        std::vector<Candidate> candidates;
        while (q->fetch())
        {
            Entry entry;
            readEntry(*q, EntryColumns::Geometries, entry);
            if (entry.polygon_geom.size() < 4)
                continue;

            const Planar footprint = project(frame, entry.polygon_geom.points);
            const double diagonal = diagonalOf(footprint);
            // A footprint with no extent has no middle to be near: it cannot be ranked
            if (!(diagonal > 0.0))
                continue;
            const Projected2D middleOfPicture = centroidOf(CoverageGeometry::Kind::Polygon, {footprint});
            const double distance = std::hypot(centre.x - middleOfPicture.x, centre.y - middleOfPicture.y);
            candidates.push_back({distance / diagonal, std::move(entry)});
        }

        const auto better = [](const Candidate &a, const Candidate &b)
        {
            return a.score != b.score ? a.score < b.score : a.entry.path < b.entry.path;
        };
        const size_t count = std::min(candidates.size(), static_cast<size_t>(options.limit));
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), candidates.end(), better);

        const Point where(centroid.longitude, centroid.latitude);
        const double xScale = std::cos(deg2rad(centroid.latitude));
        std::vector<CoverageMatch> matches;
        matches.reserve(count);
        for (size_t i = 0; i < count; i++)
        {
            const Candidate &candidate = candidates[i];
            CoverageMatch match;
            match.path = candidate.entry.path;
            match.score = candidate.score;
            match.overlap = geometry.kind == CoverageGeometry::Kind::Polygon ? overlapOf(db, match.path, wkt, xScale) : 1.0;
            match.pixel = pixelOf(candidate.entry, where, projectors);
            matches.push_back(std::move(match));
        }
        return matches;
    }

    json coverageToJson(const std::vector<CoverageMatch> &matches)
    {
        json result = json::array();
        for (const auto &match : matches)
            result.push_back({{"path", match.path},
                              {"score", match.score},
                              {"overlap", match.overlap},
                              {"pixel", match.pixel ? json::array({match.pixel->first, match.pixel->second}) : json(nullptr)}});
        return result;
    }

}
