/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "local_frame_georef.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <locale>
#include <regex>
#include <sstream>
#include <vector>

#include "coordstransformer.h"
#include "entry.h"
#include "exceptions.h"
#include "fs.h"
#include "gdal_inc.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "stringutil.h"

namespace ddb
{
    namespace
    {
        constexpr double Deg2Rad = 0.017453292519943295;
        constexpr double MetersPerDegreeLatitude = 111320.0;

        bool inRange(const Wgs84Position &p)
        {
            return std::isfinite(p.latitude) && std::isfinite(p.longitude) && std::isfinite(p.altitude) &&
                   p.latitude >= -90.0 && p.latitude <= 90.0 && p.longitude >= -180.0 && p.longitude <= 180.0;
        }

        // The CRS of a projected georef. A definition never names a file or reaches the network: it comes
        // from a sidecar next to a dataset file.
        bool loadProjectedCrs(const std::string &definition, OGRSpatialReference &srs)
        {
            const char *options[] = {"ALLOW_NETWORK_ACCESS=NO", "ALLOW_FILE_ACCESS=NO", nullptr};
            return !definition.empty() && srs.SetFromUserInput(definition.c_str(), options) == OGRERR_NONE &&
                   srs.IsProjected();
        }

        OGRSpatialReference wgs84()
        {
            OGRSpatialReference srs;
            srs.SetWellKnownGeogCS("WGS84");
            return srs;
        }

        // Transformer from the CRS of a projected georef to WGS84 (x is longitude, y latitude)
        CoordsTransformer toWgs84(const LocalFrameGeoref &georef)
        {
            OGRSpatialReference from;
            if (!loadProjectedCrs(georef.crs, from))
                throw AppException("'" + georef.crs + "' is not a projected coordinate system");
            return CoordsTransformer(from, wgs84());
        }

        // Converts points in the CRS of a projected georef to WGS84, in place; false when any fails
        bool projectedToWgs84(const LocalFrameGeoref &georef, size_t n, double *x, double *y, double *z)
        {
            try
            {
                return toWgs84(georef).tryTransform(n, x, y, z);
            }
            catch (const std::exception &e)
            {
                LOGD << "Cannot convert from " << georef.crs << " to WGS84: " << e.what();
                return false;
            }
        }

        // Whether a georef can be used: a position in range, or a projected CRS whose offset point
        // lands on the Earth
        bool isUsable(const LocalFrameGeoref &georef)
        {
            if (georef.kind == LocalFrameGeoref::Kind::Enu)
                return inRange(georef.origin);

            if (!std::all_of(georef.offset.begin(), georef.offset.end(), [](double v) { return std::isfinite(v); }))
                return false;
            try
            {
                return inRange(localFrameOrigin(georef));
            }
            catch (const std::exception &e)
            {
                LOGD << "Ignoring the georeference in " << georef.crs << ": " << e.what();
                return false;
            }
        }

        std::optional<LocalFrameGeoref> usable(LocalFrameGeoref georef)
        {
            if (!isUsable(georef))
                return std::nullopt;
            return georef;
        }

        // ---- Sidecar parsing ----

        // The first present number among the keys, or def
        std::optional<double> number(const json &j, std::initializer_list<const char *> keys,
                                     std::optional<double> def)
        {
            for (const char *k : keys)
            {
                if (j.contains(k) && j[k].is_number())
                    return j[k].get<double>();
            }
            return def;
        }

        // [x, y] or [x, y, z] of numbers
        std::optional<std::array<double, 3>> offsetOf(const json &a)
        {
            if (!a.is_array() || a.size() < 2 || a.size() > 3)
                return std::nullopt;
            std::array<double, 3> offset{0.0, 0.0, 0.0};
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (!a[i].is_number())
                    return std::nullopt;
                offset[i] = a[i].get<double>();
            }
            return offset;
        }

        // The georeference an object holds: the projected kind when it has a crs and an offset, the
        // Enu kind when it has a latitude and a longitude
        std::optional<LocalFrameGeoref> georefOfObject(const json &j)
        {
            if (!j.is_object())
                return std::nullopt;

            if (j.contains("crs") && j["crs"].is_string() && j.contains("offset"))
            {
                const auto offset = offsetOf(j["offset"]);
                if (!offset.has_value())
                    return std::nullopt;
                return usable(LocalFrameGeoref::projected(j["crs"].get<std::string>(), (*offset)[0], (*offset)[1],
                                                          (*offset)[2]));
            }

            const auto lat = number(j, {"latitude", "lat"}, std::nullopt);
            const auto lon = number(j, {"longitude", "lon", "lng"}, std::nullopt);
            const auto alt = number(j, {"altitude", "alt", "elevation"}, 0.0);
            if (!lat.has_value() || !lon.has_value())
                return std::nullopt;
            return usable(LocalFrameGeoref::enu(*lat, *lon, alt.value_or(0.0)));
        }

        std::optional<LocalFrameGeoref> georefOfJsonFile(const fs::path &file)
        {
            std::ifstream in(file);
            if (!in.good())
                return std::nullopt;
            json j;
            in >> j;
            return georefOfObject(j);
        }

        // "WGS84 UTM 15N" (what ODM writes) as the EPSG code of that UTM zone; any other line is a
        // CRS definition as it is
        std::string crsOfOdmLine(const std::string &line)
        {
            static const std::regex utm(R"(^WGS\s*84\s+UTM\s+(\d{1,2})\s*([NS])$)", std::regex::icase);
            std::smatch m;
            if (std::regex_match(line, m, utm))
            {
                const int zone = std::stoi(m[1].str());
                if (zone >= 1 && zone <= 60)
                {
                    const bool north = std::toupper(static_cast<unsigned char>(m[2].str()[0])) == 'N';
                    return "EPSG:" + std::to_string((north ? 32600 : 32700) + zone);
                }
            }
            return line;
        }

        // "x y" or "x y z": the offset of ODM's coords.txt (nothing else may be on the line)
        std::optional<std::array<double, 3>> offsetOfLine(const std::string &line)
        {
            std::istringstream in(line);
            in.imbue(std::locale::classic());
            std::array<double, 3> offset{0.0, 0.0, 0.0};
            size_t count = 0;
            double v = 0.0;
            while (count < 3 && (in >> v))
                offset[count++] = v;
            if (count < 2 || !in.eof())
                return std::nullopt;
            return offset;
        }

        // ODM's coords.txt: the CRS on the first line, the offset of the local frame on the second
        std::optional<LocalFrameGeoref> georefOfOdmCoords(const fs::path &file)
        {
            std::ifstream in(file);
            if (!in.good())
                return std::nullopt;
            std::string crsLine, offsetLine;
            if (!std::getline(in, crsLine) || !std::getline(in, offsetLine))
                return std::nullopt;

            const auto offset = offsetOfLine(str::trimCopy(offsetLine));
            if (!offset.has_value())
                return std::nullopt;
            return usable(LocalFrameGeoref::projected(crsOfOdmLine(str::trimCopy(crsLine)), (*offset)[0], (*offset)[1],
                                                      (*offset)[2]));
        }

        // ---- Footprints ----

        // The corners of a ring: west-south, east-south, east-north, west-north
        std::array<std::array<double, 2>, 4> cornersOf(const LocalBounds &b)
        {
            return {{{b.minX, b.minY}, {b.maxX, b.minY}, {b.maxX, b.maxY}, {b.minX, b.maxY}}};
        }

        BasicPolygonGeometry closedRing(const std::array<std::array<double, 2>, 4> &corners)
        {
            BasicPolygonGeometry ring;
            for (const auto &c : corners)
                ring.addPoint(c[0], c[1], 0.0);
            ring.addPoint(corners[0][0], corners[0][1], 0.0);
            return ring;
        }
    } // namespace

    LocalFrameGeoref LocalFrameGeoref::enu(double latitude, double longitude, double altitude)
    {
        LocalFrameGeoref g;
        g.kind = Kind::Enu;
        g.origin = {latitude, longitude, altitude};
        return g;
    }

    LocalFrameGeoref LocalFrameGeoref::projected(std::string crs, double x, double y, double z)
    {
        LocalFrameGeoref g;
        g.kind = Kind::Projected;
        g.crs = std::move(crs);
        g.offset = {x, y, z};
        return g;
    }

    void to_json(json &j, const LocalFrameGeoref &georef)
    {
        j = json::object();
        if (georef.kind == LocalFrameGeoref::Kind::Enu)
        {
            j["kind"] = "enu";
            j["origin"] = {georef.origin.latitude, georef.origin.longitude, georef.origin.altitude};
        }
        else
        {
            j["kind"] = "projected";
            j["crs"] = georef.crs;
            j["offset"] = {georef.offset[0], georef.offset[1], georef.offset[2]};
        }
    }

    std::optional<LocalFrameGeoref> localFrameGeorefFromProperty(const json &j)
    {
        if (!j.is_object() || !j.contains("kind") || !j["kind"].is_string())
            return std::nullopt;

        const std::string kind = j["kind"].get<std::string>();
        if (kind == "enu")
        {
            if (!j.contains("origin") || !j["origin"].is_array() || j["origin"].size() != 3)
                return std::nullopt;
            const auto &o = j["origin"];
            if (!std::all_of(o.begin(), o.end(), [](const json &v) { return v.is_number(); }))
                return std::nullopt;
            return usable(LocalFrameGeoref::enu(o[0].get<double>(), o[1].get<double>(), o[2].get<double>()));
        }
        if (kind == "projected" && j.contains("crs") && j["crs"].is_string() && j.contains("offset"))
        {
            const auto offset = offsetOf(j["offset"]);
            if (!offset.has_value() || j["offset"].size() != 3)
                return std::nullopt;
            return usable(
                LocalFrameGeoref::projected(j["crs"].get<std::string>(), (*offset)[0], (*offset)[1], (*offset)[2]));
        }
        return std::nullopt;
    }

    std::optional<LocalFrameGeoref> detectLocalFrameGeoref(const std::string &inputFile)
    {
        const fs::path file = io::fromUtf8(inputFile);
        const fs::path dir = file.parent_path();
        const fs::path parent = dir.parent_path();
        const std::string stem = io::toUtf8(file.stem());

        // From the most specific to the most general: the sidecar of this file, the one of its dataset
        // folder, OpenDroneMap's, then those of the project folder above it
        struct Candidate
        {
            fs::path path;
            bool odmCoords;
        };
        const std::vector<Candidate> candidates = {
            {dir / io::fromUtf8(stem + ".geo.json"), false},
            {dir / "georef.json", false},
            {dir / "reference_lla.json", false},
            {dir / "coords.txt", true},
            {dir / "odm_georeferencing" / "coords.txt", true},
            {parent / "reference_lla.json", false},
            {parent / "opensfm" / "reference_lla.json", false},
            {parent / "odm_georeferencing" / "coords.txt", true},
        };

        for (const auto &c : candidates)
        {
            std::error_code ec;
            if (!fs::is_regular_file(c.path, ec) || ec)
                continue;

            try
            {
                const auto georef = c.odmCoords ? georefOfOdmCoords(c.path) : georefOfJsonFile(c.path);
                if (georef.has_value())
                {
                    LOGD << "Georeference found in " << io::toUtf8(c.path);
                    return georef;
                }
                LOGD << "No usable georeference in " << io::toUtf8(c.path);
            }
            catch (const std::exception &e)
            {
                LOGD << "Could not parse georeference sidecar " << io::toUtf8(c.path) << ": " << e.what();
            }
        }

        return std::nullopt;
    }

    Wgs84Position localFrameOrigin(const LocalFrameGeoref &georef)
    {
        if (georef.kind == LocalFrameGeoref::Kind::Enu)
            return georef.origin;

        double x = georef.offset[0], y = georef.offset[1], z = georef.offset[2];
        if (!projectedToWgs84(georef, 1, &x, &y, &z))
            throw AppException("Cannot convert the offset of the georeference from " + georef.crs + " to WGS84");
        return {y, x, z};
    }

    BasicPolygonGeometry localFrameFootprint(const LocalFrameGeoref &georef, const LocalBounds &bounds)
    {
        auto corners = cornersOf(bounds);

        if (georef.kind == LocalFrameGeoref::Kind::Projected)
        {
            std::array<double, 4> x, y;
            for (size_t i = 0; i < corners.size(); ++i)
            {
                x[i] = georef.offset[0] + corners[i][0];
                y[i] = georef.offset[1] + corners[i][1];
            }
            if (!projectedToWgs84(georef, corners.size(), x.data(), y.data(), nullptr))
                throw AppException("Cannot convert the footprint from " + georef.crs + " to WGS84");
            for (size_t i = 0; i < corners.size(); ++i)
                corners[i] = {x[i], y[i]};
            return closedRing(corners);
        }

        // Meters East/North around the origin, equirectangular. The cosine is kept off 0 at the poles to
        // keep the longitude span finite.
        const double cosLat = std::cos(georef.origin.latitude * Deg2Rad);
        const double metersPerDegreeLongitude = MetersPerDegreeLatitude * std::max(0.01, std::abs(cosLat));
        for (auto &c : corners)
            c = {georef.origin.longitude + c[0] / metersPerDegreeLongitude,
                 georef.origin.latitude + c[1] / MetersPerDegreeLatitude};
        return closedRing(corners);
    }

    json localFrameFootprintGeometry(const LocalFrameGeoref &georef, const LocalBounds &bounds)
    {
        json ring = json::array();
        for (const auto &p : localFrameFootprint(georef, bounds).points)
            ring.push_back({p.x, p.y});
        return {{"type", "Polygon"}, {"coordinates", json::array({ring})}};
    }

    void applyLocalFrameOrigin(Entry &entry, const LocalFrameGeoref &georef)
    {
        const Wgs84Position origin = localFrameOrigin(georef);

        entry.properties["georeferenced"] = true;
        entry.properties["georef"] = georef;
        entry.point_geom.clear();
        entry.point_geom.addPoint(origin.longitude, origin.latitude, origin.altitude);
    }

    void applyLocalFrameFootprint(Entry &entry, const LocalFrameGeoref &georef, const LocalBounds &bounds)
    {
        entry.polygon_geom = localFrameFootprint(georef, bounds);
    }

} // namespace ddb
