/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include "coordstransformer.h"
#include "gdal_raii.h"
#include "geo.h"
#include "exceptions.h"

namespace ddb
{

    // Adapted from GeographicLib
    int latitudeBand(double latitude)
    {
        using std::floor;
        int ilat = int(floor(latitude));
        return (std::max)(-10, (std::min)(9, (ilat + 80) / 8 - 10));
    }

    double copysignx(double x, double y)
    {
        /* 1/y trick to get the sign of -0.0 */
        return fabs(x) * (y < 0 || (y == 0 && 1 / y < 0) ? -1 : 1);
    }

    double remainderx(double x, double y)
    {
        double z;
        y = fabs(y); /* The result doesn't depend on the sign of y */
        z = fmod(x, y);
        if (z == 0)
            /* This shouldn't be necessary.  However, before version 14 (2015),
             * Visual Studio had problems dealing with -0.0.  Specifically
             *   VC 10,11,12 and 32-bit compile: fmod(-0.0, 360.0) -> +0.0
             * python 2.7 on Windows 32-bit machines has the same problem. */
            z = copysignx(z, x);
        else if (2 * fabs(z) == y)
            z -= fmod(x, 2 * y) - z; /* Implement ties to even */
        else if (2 * fabs(z) > y)
            z += (z < 0 ? y : -y); /* Fold remaining cases to (-y/2, y/2) */
        return z;
    }

    double angNormalize(double x)
    {
        x = remainderx(x, (double)(360));
        return x != -180 ? x : 180;
    }

    int standardUTMZone(double latitude, double longitude)
    {
        int ilon = int(floor(angNormalize(longitude)));
        if (ilon == 180)
            ilon = -180; // ilon now in [-180,180)
        int zone = (ilon + 186) / 6;
        int band = latitudeBand(latitude);
        if (band == 7 && zone == 31 && ilon >= 3) // The Norway exception
            zone = 32;
        else if (band == 9 && ilon >= 0 && ilon < 42) // The Svalbard exception
            zone = 2 * ((ilon + 183) / 12) + 1;
        return zone;
    }

    // Helper functions
    UTMZone getUTMZone(double latitude, double longitude)
    {
        UTMZone z;
        z.zone = standardUTMZone(latitude, longitude);
        z.north = latitude >= 0;
        return z;
    }

    std::string getProjForUTM(const UTMZone &zone)
    {
        std::stringstream ss;
        ss << "+proj=utm +zone=" << zone.zone << " +datum=WGS84 " << (zone.north ? "" : "+south ") << "+units=m +no_defs";
        return ss.str();
    }

    namespace
    {
        std::string zoneLabel(const UTMZone &zone)
        {
            return std::to_string(zone.zone) + (zone.north ? "N" : "S");
        }

        // WGS84 <-> UTM zone transform; every failure throws and nothing leaks.
        std::unique_ptr<CoordsTransformer> createUtmTransform(const UTMZone &zone, bool toUtm)
        {
            const std::string proj = getProjForUTM(zone);
            const SrsPtr utm = srsFromProj4(proj.c_str());
            if (!utm)
                throw GDALException("Cannot import spatial reference system " + proj + ". Is PROJ available?");

            const SrsPtr wgs84 = srsFromEpsg(4326);
            if (!wgs84)
                throw GDALException("Cannot import EPSG:4326 spatial reference system. Is PROJ available?");

            auto ct = toUtm ? CoordsTransformer::tryCreate(srsOf(wgs84), srsOf(utm))
                            : CoordsTransformer::tryCreate(srsOf(utm), srsOf(wgs84));
            if (!ct)
                throw GDALException("Cannot create coordinate transformation " +
                                    std::string(toUtm ? "to" : "from") + " UTM zone " + zoneLabel(zone));
            return ct;
        }
    }

    Projected2D toUTM(double latitude, double longitude, const UTMZone &zone)
    {
        const auto ct = createUtmTransform(zone, true);

        double geoX = longitude;
        double geoY = latitude;
        if (!ct->tryTransform(1, &geoX, &geoY))
            throw GDALException("Cannot transform coordinates to UTM " + std::to_string(latitude) + "," + std::to_string(longitude));

        return Projected2D(geoX, geoY);
    }

    Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone)
    {
        return fromUTM(p.x, p.y, zone);
    }

    Geographic2D fromUTM(double x, double y, const UTMZone &zone)
    {
        const auto ct = createUtmTransform(zone, false);

        double geoX = x;
        double geoY = y;
        if (!ct->tryTransform(1, &geoX, &geoY))
            throw GDALException("Cannot transform coordinates from UTM " + std::to_string(x) + "," + std::to_string(y));

        return Geographic2D(geoX, geoY);
    }

}
