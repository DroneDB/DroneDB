/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tiles3d.h"

#include <array>
#include <cmath>
#include <vector>
#include <zip.h>

#include "json.h"
#include "logger.h"
#include "ogr_spatialref.h"
#include "ogr_api.h"

namespace ddb
{

    namespace
    {
        constexpr double RAD2DEG = 57.29577951308232;
        constexpr double METERS_PER_DEG_LAT = 111320.0;

        // Reads a single entry from a ZIP archive fully into memory. Returns false when
        // the archive cannot be opened or the entry is missing/too large. Used to pull
        // only tileset.json out of a .3tz without extracting the whole archive.
        bool readEntryFromZip(const std::string &zipFile, const std::string &entryName,
                              std::string &out)
        {
            int err = 0;
            zip_t *z = zip_open(zipFile.c_str(), ZIP_RDONLY, &err);
            if (z == nullptr)
                return false;

            bool ok = false;
            const zip_int64_t idx = zip_name_locate(z, entryName.c_str(), 0);
            if (idx >= 0)
            {
                zip_stat_t st;
                zip_stat_init(&st);
                if (zip_stat_index(z, idx, 0, &st) == 0 && (st.valid & ZIP_STAT_SIZE) &&
                    st.size <= 100ull * 1024 * 1024) // tileset.json guard (<=100MB)
                {
                    zip_file_t *f = zip_fopen_index(z, idx, 0);
                    if (f != nullptr)
                    {
                        out.resize(static_cast<size_t>(st.size));
                        const zip_int64_t nread =
                            zip_fread(f, out.data(), st.size);
                        zip_fclose(f);
                        ok = (nread >= 0 &&
                              static_cast<zip_uint64_t>(nread) == st.size);
                    }
                }
            }

            zip_close(z);
            return ok;
        }

        // Applies a 3D Tiles 4x4 column-major transform to a point. When the transform is
        // absent (size != 16) the point is returned unchanged.
        std::array<double, 3> applyTransform(const std::vector<double> &m,
                                             double x, double y, double z)
        {
            if (m.size() != 16)
                return {x, y, z};
            return {
                m[0] * x + m[4] * y + m[8] * z + m[12],
                m[1] * x + m[5] * y + m[9] * z + m[13],
                m[2] * x + m[6] * y + m[10] * z + m[14]};
        }

        // Transforms a geocentric ECEF point (EPSG:4978) to WGS84 lon/lat/alt (degrees,
        // meters) using traditional GIS axis order (lon, lat), matching the rest of the
        // codebase. Returns false on failure (e.g. PROJ unavailable).
        bool ecefToWgs84(double ecefX, double ecefY, double ecefZ,
                         double &lon, double &lat, double &alt)
        {
            OGRSpatialReferenceH hEcef = OSRNewSpatialReference(nullptr);
            OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);
            bool ok = false;

            if (OSRImportFromEPSG(hEcef, 4978) == OGRERR_NONE &&
                OSRImportFromEPSG(hWgs84, 4326) == OGRERR_NONE)
            {
                OSRSetAxisMappingStrategy(hEcef, OAMS_TRADITIONAL_GIS_ORDER);
                OSRSetAxisMappingStrategy(hWgs84, OAMS_TRADITIONAL_GIS_ORDER);

                OGRCoordinateTransformationH hT =
                    OCTNewCoordinateTransformation(hEcef, hWgs84);
                if (hT != nullptr)
                {
                    lon = ecefX;
                    lat = ecefY;
                    alt = ecefZ;
                    ok = OCTTransform(hT, 1, &lon, &lat, &alt) == TRUE;
                    OCTDestroyCoordinateTransformation(hT);
                }
            }

            OSRDestroySpatialReference(hWgs84);
            OSRDestroySpatialReference(hEcef);
            return ok;
        }

        // Builds an equirectangular footprint (approximate at tileset scale) of the given
        // ground radius, centred on lon/lat, and writes it into info.
        void setFootprintFromRadius(Tiles3DInfo &info, double lon, double lat, double alt,
                                    double radiusMeters)
        {
            const double cosLat = std::cos(lat * (1.0 / RAD2DEG));
            const double metersPerDegLon =
                METERS_PER_DEG_LAT * std::max(0.01, std::abs(cosLat));

            const double dLon = radiusMeters / metersPerDegLon;
            const double dLat = radiusMeters / METERS_PER_DEG_LAT;

            info.centerLon = lon;
            info.centerLat = lat;
            info.centerAlt = alt;
            info.west = lon - dLon;
            info.east = lon + dLon;
            info.south = lat - dLat;
            info.north = lat + dLat;
            info.hasBounds = true;
        }
    }

    bool getTiles3DInfo(const std::string &ttzPath, Tiles3DInfo &info)
    {
        std::string jsonStr;
        if (!readEntryFromZip(ttzPath, "tileset.json", jsonStr))
        {
            LOGD << "Tiles3D: tileset.json not found at root of " << ttzPath;
            return false;
        }

        json j;
        try
        {
            j = json::parse(jsonStr);
        }
        catch (const std::exception &e)
        {
            LOGD << "Tiles3D: cannot parse tileset.json: " << e.what();
            return false;
        }

        // Asset metadata (best-effort).
        if (j.contains("asset") && j["asset"].is_object() &&
            j["asset"].contains("version") && j["asset"]["version"].is_string())
            info.assetVersion = j["asset"]["version"].get<std::string>();

        if (!j.contains("root") || !j["root"].is_object())
        {
            LOGD << "Tiles3D: tileset.json has no root tile";
            return false;
        }
        const json &root = j["root"];

        if (root.contains("geometricError") && root["geometricError"].is_number())
            info.geometricError = root["geometricError"].get<double>();

        if (!root.contains("boundingVolume") || !root["boundingVolume"].is_object())
        {
            LOGD << "Tiles3D: root tile has no boundingVolume";
            return false;
        }
        const json &bv = root["boundingVolume"];

        // Optional root transform (column-major 4x4).
        std::vector<double> transform;
        if (root.contains("transform") && root["transform"].is_array() &&
            root["transform"].size() == 16)
            transform = root["transform"].get<std::vector<double>>();

        // 1) region: [west, south, east, north, minHeight, maxHeight] in WGS84 radians.
        if (bv.contains("region") && bv["region"].is_array() &&
            bv["region"].size() >= 4)
        {
            const auto &r = bv["region"];
            info.west = r[0].get<double>() * RAD2DEG;
            info.south = r[1].get<double>() * RAD2DEG;
            info.east = r[2].get<double>() * RAD2DEG;
            info.north = r[3].get<double>() * RAD2DEG;
            info.centerLon = (info.west + info.east) / 2.0;
            info.centerLat = (info.south + info.north) / 2.0;
            info.centerAlt = (r.size() >= 6)
                                 ? (r[4].get<double>() + r[5].get<double>()) / 2.0
                                 : 0.0;
            info.georeferenced = true;
            info.hasBounds = true;
            return true;
        }

        // 2) box: [cx, cy, cz, xHalf(3), yHalf(3), zHalf(3)] in the (transformed) frame.
        if (bv.contains("box") && bv["box"].is_array() && bv["box"].size() >= 12)
        {
            const auto &b = bv["box"];
            const auto c = applyTransform(transform, b[0].get<double>(),
                                          b[1].get<double>(), b[2].get<double>());
            // Bounding radius from the three half-axis vectors.
            auto axisLen = [&](int off) {
                const double ax = b[off].get<double>();
                const double ay = b[off + 1].get<double>();
                const double az = b[off + 2].get<double>();
                return std::sqrt(ax * ax + ay * ay + az * az);
            };
            const double radius = axisLen(3) + axisLen(6) + axisLen(9);

            const double dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            // ECEF centres sit ~1 Earth radius from the origin; local frames are near 0.
            if (dist > 1.0e6)
            {
                double lon, lat, alt;
                if (ecefToWgs84(c[0], c[1], c[2], lon, lat, alt))
                {
                    info.georeferenced = true;
                    setFootprintFromRadius(info, lon, lat, alt, radius);
                    return true;
                }
            }
            // Local/engineering frame: indexed without a footprint.
            info.georeferenced = false;
            return true;
        }

        // 3) sphere: [cx, cy, cz, radius] in the (transformed) frame.
        if (bv.contains("sphere") && bv["sphere"].is_array() &&
            bv["sphere"].size() >= 4)
        {
            const auto &s = bv["sphere"];
            const auto c = applyTransform(transform, s[0].get<double>(),
                                          s[1].get<double>(), s[2].get<double>());
            const double radius = s[3].get<double>();
            const double dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (dist > 1.0e6)
            {
                double lon, lat, alt;
                if (ecefToWgs84(c[0], c[1], c[2], lon, lat, alt))
                {
                    info.georeferenced = true;
                    setFootprintFromRadius(info, lon, lat, alt, radius);
                    return true;
                }
            }
            info.georeferenced = false;
            return true;
        }

        LOGD << "Tiles3D: unsupported boundingVolume type";
        return false;
    }

}
