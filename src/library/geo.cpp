/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"

#include <sstream>
#include "geo.h"

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

    Projected2D toUTM(double latitude, double longitude, const UTMZone &zone)
    {
        Projected2D result;

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);

        if (hSrs == nullptr)
            throw GDALException("Cannot create spatial reference system for UTM zone " + std::to_string(zone.zone) + (zone.north ? "N" : "S"));

        std::string proj = getProjForUTM(zone);


        if (OSRImportFromProj4(hSrs, proj.c_str()) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import spatial reference system " + proj + ". Is PROJ available?");
        }



        //OSRSetAxisMappingStrategy(hSrs, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);
        //LOGV << "Set dest axis mapping strategy";
        OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);

        if (hWgs84 == nullptr)
        {
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot create WGS84 spatial reference system for UTM zone " + std::to_string(zone.zone) + (zone.north ? "N" : "S"));
        }

        if (OSRImportFromEPSG(hWgs84, 4326) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(hWgs84);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import EPSG:4326 spatial reference system. Is PROJ available?");
        }

        //OSRSetAxisMappingStrategy(hWgs84, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);
        //LOGV << "Set dest axis mapping strategy";

        OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hWgs84, hSrs);

        double geoX = latitude;
        double geoY = longitude;

        const auto success = OCTTransform(hTransform, 1, &geoX, &geoY, nullptr) == TRUE;

        OCTDestroyCoordinateTransformation(hTransform);
        OSRDestroySpatialReference(hWgs84);
        OSRDestroySpatialReference(hSrs);

        if (!success)
            throw GDALException("Cannot transform coordinates to UTM " + std::to_string(latitude) + "," + std::to_string(longitude));

        return Projected2D(geoX, geoY);
    }

    Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone)
    {
        return fromUTM(p.x, p.y, zone);
    }

    Geographic2D fromUTM(double x, double y, const UTMZone &zone)
    {
        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);

        std::string proj = getProjForUTM(zone);
        if (OSRImportFromProj4(hSrs, proj.c_str()) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(hWgs84);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import spatial reference system " + proj + ". Is PROJ available?");
        }

        //OSRSetAxisMappingStrategy(hSrs, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);
        //LOGV << "Set dest axis mapping strategy";

        if (OSRImportFromEPSG(hWgs84, 4326) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(hWgs84);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import EPSG:4326 spatial reference system. Is PROJ available?");
        }

        //OSRSetAxisMappingStrategy(hWgs84, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);
        //LOGV << "Set dest axis mapping strategy";

        OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hWgs84);

        double geoX = x;
        double geoY = y;

        const auto success = OCTTransform(hTransform, 1, &geoX, &geoY, nullptr) == TRUE;

        OCTDestroyCoordinateTransformation(hTransform);
        OSRDestroySpatialReference(hWgs84);
        OSRDestroySpatialReference(hSrs);

        if (!success)
            throw GDALException("Cannot transform coordinates from UTM " + std::to_string(x) + "," + std::to_string(y));

        return Geographic2D(geoY, geoX);

    }

}
