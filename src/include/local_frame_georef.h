/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef LOCAL_FRAME_GEOREF_H
#define LOCAL_FRAME_GEOREF_H

#include <array>
#include <optional>
#include <string>

#include "basicgeometry.h"
#include "json_fwd.h"

namespace ddb
{
    struct Entry;

    /** A WGS84 position: degrees, and meters above the ellipsoid. */
    struct Wgs84Position
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;
    };

    /**
     * @brief Where the local frame of a model or a splat sits on the Earth.
     *
     * Reconstructions (OpenDroneMap, Obj2Tiles, a splat trained from either) are made in a
     * local frame whose X is East, Y North and Z up, in meters, and say where that frame is in
     * one of two ways:
     *  - @c Enu: the frame is a local tangent plane centered at a WGS84 position
     *    (`reference_lla.json`, `<stem>.geo.json`, `georef.json` with latitude/longitude);
     *  - @c Projected: the frame is a projected CRS shifted by an offset, i.e. a point of the
     *    scene is (x, y, z) + offset in that CRS (ODM `coords.txt`, `georef.json` with
     *    crs/offset). Its axes are those of the CRS, so a footprint is exact in it.
     */
    struct LocalFrameGeoref
    {
        enum class Kind
        {
            Enu,
            Projected
        };

        Kind kind = Kind::Enu;
        /// Enu: the WGS84 position of the origin of the local frame.
        Wgs84Position origin{};
        /// Projected: the CRS as OGRSpatialReference::SetFromUserInput reads it (EPSG:xxxx, a PROJ
        /// string or WKT).
        std::string crs{};
        /// Projected: what is added to a local point to get its coordinates in @c crs (CRS units).
        std::array<double, 3> offset{0.0, 0.0, 0.0};

        static LocalFrameGeoref enu(double latitude, double longitude, double altitude = 0.0);
        static LocalFrameGeoref projected(std::string crs, double x, double y, double z = 0.0);
    };

    /// The extent of a local frame, in its own units (meters): the box a footprint is made of.
    struct LocalBounds
    {
        double minX = 0.0, minY = 0.0;
        double maxX = 0.0, maxY = 0.0;
    };

    /**
     * @brief The `georef` property of an entry.
     *
     * `{"kind": "enu", "origin": [lat, lon, alt]}` or
     * `{"kind": "projected", "crs": "<EPSG:xxxx, PROJ string or WKT>", "offset": [x, y, z]}`.
     */
    void to_json(json &j, const LocalFrameGeoref &georef);

    /// Reads back what to_json wrote: nothing when @p j is not a usable `georef` property.
    std::optional<LocalFrameGeoref> localFrameGeorefFromProperty(const json &j);

    /**
     * @brief Detect the georeferencing of a model or a splat from sidecar files.
     *
     * Looks for these, in order, and returns the first usable one:
     *  1. next to @p inputFile: `<stem>.geo.json`, `georef.json`, `reference_lla.json`,
     *     `coords.txt`, `odm_georeferencing/coords.txt`;
     *  2. one directory up: `reference_lla.json`, `opensfm/reference_lla.json`,
     *     `odm_georeferencing/coords.txt`.
     *
     * A `.geo.json` / `georef.json` is an object with `latitude`/`longitude` (required) and
     * `altitude` (optional, default 0; `lat`/`lon`/`lng`/`alt`/`elevation` are accepted too), or
     * with `crs` and `offset` ([x, y] or [x, y, z], z defaults to 0), which is the projected kind.
     * `coords.txt` is the ODM file: line 1 is the CRS ("WGS84 UTM 15N", or an EPSG code, PROJ
     * string or WKT) and line 2 the offset ("easting northing"). Whatever follows is ignored.
     *
     * A sidecar that does not parse, whose coordinates are out of range or whose CRS is not a
     * projected one GDAL knows is skipped (and the search goes on).
     *
     * @return The georeferencing, or std::nullopt when there is none (the file is in local space).
     */
    std::optional<LocalFrameGeoref> detectLocalFrameGeoref(const std::string &inputFile);

    /**
     * @brief The georeferencing of an ODM `coords.txt` file, and of nothing else.
     *
     * Reads the file named, without the search of detectLocalFrameGeoref: a `georef.json` or a
     * `reference_lla.json` next to it does not take its place. The result is the projected kind;
     * std::nullopt when the file is missing, is not "CRS line, then offset line", or its CRS is not a
     * projected one GDAL knows.
     */
    std::optional<LocalFrameGeoref> parseOdmCoords(const std::string &coordsFile);

    /**
     * @brief The WGS84 position of the origin of the local frame.
     *
     * Enu: its origin. Projected: the offset point converted from the CRS. Throws AppException when
     * the point cannot be converted.
     */
    Wgs84Position localFrameOrigin(const LocalFrameGeoref &georef);

    /**
     * @brief The WGS84 footprint of a local extent: a closed ring of 5 points (west-south, east-south,
     * east-north, west-north, west-south; x is longitude and y latitude, z 0).
     *
     * Projected: the 4 corners are transformed exactly from the CRS. Enu: an equirectangular
     * approximation around the origin (111320 m per degree of latitude, scaled by cos(latitude) for
     * longitude), which is accurate at the scale of a model or a scene.
     * Throws AppException when a corner cannot be converted.
     */
    BasicPolygonGeometry localFrameFootprint(const LocalFrameGeoref &georef, const LocalBounds &bounds);

    /// The footprint as a GeoJSON Polygon geometry (`{"type": "Polygon", "coordinates": [[[lon, lat], ...]]}`)
    json localFrameFootprintGeometry(const LocalFrameGeoref &georef, const LocalBounds &bounds);

    /**
     * @brief Marks an entry as georeferenced: `georeferenced`, the `georef` property and its
     * origin as the point geometry. Throws AppException when the origin cannot be converted.
     */
    void applyLocalFrameOrigin(Entry &entry, const LocalFrameGeoref &georef);

    /// Sets the polygon geometry of an entry to the footprint of a local extent (see localFrameFootprint).
    void applyLocalFrameFootprint(Entry &entry, const LocalFrameGeoref &georef, const LocalBounds &bounds);

} // namespace ddb

#endif // LOCAL_FRAME_GEOREF_H
