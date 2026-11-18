/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pointcloud_crs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

#include "coordstransformer.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_raii.h"

namespace ddb
{
    namespace
    {
        struct StatusName
        {
            PointCloudCrsStatus status;
            const char *name;
        };

        constexpr StatusName kStatusNames[] = {
            {PointCloudCrsStatus::Projected, "projected"},   {PointCloudCrsStatus::Geographic, "geographic"},
            {PointCloudCrsStatus::Geocentric, "geocentric"}, {PointCloudCrsStatus::Local, "local"},
            {PointCloudCrsStatus::None, "none"},             {PointCloudCrsStatus::Implausible, "implausible"}};

        // The distance from the centre of the earth of a point on it, in metres: the polar radius
        // less some 60 km, the equatorial radius plus some 40 km (a mine, a mountain, a plane)
        constexpr double kMinEarthDistance = 6.30e6;
        constexpr double kMaxEarthDistance = 6.42e6;

        // Where UTM ends (and the Universal Polar Stereographic starts)
        constexpr double kUtmNorthLimit = 84.0;
        constexpr double kUtmSouthLimit = -80.0;
        constexpr int kUpsNorthEpsg = 5041;
        constexpr int kUpsSouthEpsg = 5042;

        // How far from the central meridian of its zone a footprint may reach and stay in UTM: 1.5
        // degrees into the next zone, where the scale of UTM is 0.25 % off at the equator and less
        // elsewhere. A small cloud across the edge of a zone stays in the zone of its centre (an
        // EPSG code the viewers know); a wider one is in a centred transverse Mercator.
        constexpr double kUtmReachDegrees = 4.5;

        // The confidence PROJ gives a CRS that is equivalent to the one asked for under another name
        // (proj_identify): a UTM CRS built here is unnamed
        constexpr int kEquivalentConfidence = 70;

        const char *const kIsSameOptions[] = {"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES",
                                              "CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS", nullptr};

        std::string nameOf(const OGRSpatialReference &srs)
        {
            const char *name = srs.GetName();
            return name != nullptr && name[0] != '\0' ? std::string(name) : std::string("the coordinate system of the file");
        }

        // The kind of CRS srs is, whatever the coordinates
        PointCloudCrsStatus kindOf(const OGRSpatialReference &srs)
        {
            if (srs.IsLocal())
                return PointCloudCrsStatus::Local;
            if (srs.IsGeocentric())
                return PointCloudCrsStatus::Geocentric;
            if (srs.IsProjected())
                return PointCloudCrsStatus::Projected;
            if (srs.IsGeographic())
                return PointCloudCrsStatus::Geographic;
            // A vertical CRS alone places nothing on the earth either
            return PointCloudCrsStatus::Local;
        }

        // lon in [-180, 180)
        double normalizedLongitude(double lon)
        {
            const double turned = std::fmod(lon + 180.0, 360.0);
            return (turned < 0.0 ? turned + 360.0 : turned) - 180.0;
        }

        bool within(double value, double low, double high)
        {
            return std::isfinite(value) && value >= low && value <= high;
        }

        bool geographicFits(const OGRSpatialReference &srs, const std::vector<double> &b)
        {
            // In degrees, whatever the angular unit of the CRS
            constexpr double Pi = 3.14159265358979323846;
            const double toDegrees = srs.GetAngularUnits() * 180.0 / Pi;
            constexpr double Slack = 1e-9;
            const double west = b[0] * toDegrees, east = b[3] * toDegrees;
            return within(west, -180.0 - Slack, 360.0 + Slack) && within(east, -180.0 - Slack, 360.0 + Slack) &&
                   east - west <= 360.0 + Slack && within(b[1] * toDegrees, -90.0 - Slack, 90.0 + Slack) &&
                   within(b[4] * toDegrees, -90.0 - Slack, 90.0 + Slack);
        }

        bool geocentricFits(const OGRSpatialReference &srs, const std::vector<double> &b)
        {
            const double toMetres = srs.GetLinearUnits();
            for (int corner = 0; corner < 8; corner++)
            {
                const double x = b[(corner & 1) != 0 ? 3 : 0] * toMetres;
                const double y = b[(corner & 2) != 0 ? 4 : 1] * toMetres;
                const double z = b[(corner & 4) != 0 ? 5 : 2] * toMetres;
                if (!within(std::sqrt(x * x + y * y + z * z), kMinEarthDistance, kMaxEarthDistance))
                    return false;
            }
            return true;
        }

        // Whether lon is within [west, east] widened by margin, an area that may cross the
        // antimeridian (west > east)
        bool withinLongitudes(double lon, double west, double east, double margin)
        {
            if (!std::isfinite(lon))
                return false;
            const auto distanceEast = [](double from, double to) { return std::fmod(std::fmod(to - from, 360.0) + 360.0, 360.0); };
            const double width = west <= east ? east - west : 360.0 - (west - east);
            if (width + 2 * margin >= 360.0)
                return true;
            return distanceEast(west - margin, lon) <= width + 2 * margin;
        }

        struct AreaOfUse
        {
            double west = 0, south = 0, east = 0, north = 0;
        };

        std::optional<AreaOfUse> areaOfUseOf(const OGRSpatialReference &srs)
        {
            AreaOfUse area;
            const char *areaName = nullptr;
            // -1000 is GDAL's value for a bound that is not known
            if (srs.GetAreaOfUse(&area.west, &area.south, &area.east, &area.north, &areaName) && area.west >= -180.0 &&
                area.south >= -90.0)
                return area;
            return std::nullopt;
        }

        std::optional<int> epsgOf(const OGRSpatialReference &srs, const char *node = nullptr);

        // The area of use of a projected CRS: its own, or, for a WKT1 text that names an EPSG code
        // and has no area (WKT1 cannot hold one), the one of that code
        std::optional<AreaOfUse> projectedAreaOfUse(const OGRSpatialReference &srs)
        {
            if (const auto area = areaOfUseOf(srs))
                return area;
            auto code = epsgOf(srs);
            if (!code)
                code = epsgOf(srs, "PROJCS");
            OGRSpatialReference registered;
            if (!code || registered.importFromEPSG(*code) != OGRERR_NONE)
                return std::nullopt;
            return areaOfUseOf(registered);
        }

        bool projectedFits(const OGRSpatialReference &srs, const std::vector<double> &b)
        {
            const auto area = projectedAreaOfUse(srs);
            if (!area)
                return true;
            const auto [west, south, east, north] = *area;

            std::unique_ptr<CoordsTransformer> toWgs84;
            try
            {
                const SrsPtr wgs84 = srsFromEpsg(4326);
                if (!wgs84)
                    return true;
                toWgs84 = CoordsTransformer::tryCreate(horizontalSrs(srs), srsOf(wgs84));
            }
            catch (const GDALException &)
            {
                return true;
            }
            if (!toWgs84)
                return true;

            std::array<double, 4> x = {b[0], b[3], b[3], b[0]};
            std::array<double, 4> y = {b[1], b[1], b[4], b[4]};
            if (!toWgs84->tryTransform(x.size(), x.data(), y.data()))
                return false;
            const double m = PointCloudCrsPolicy::kProjectedMarginDegrees;
            for (size_t i = 0; i < x.size(); i++)
                if (!within(y[i], -90.0, 90.0) || !withinLongitudes(x[i], west, east, m) ||
                    !within(y[i], south - m, north + m))
                    return false;
            return true;
        }

        // srs read from any text GDAL reads ("EPSG:n", WKT), in the traditional GIS axis order
        std::optional<OGRSpatialReference> readCrs(const std::string &text)
        {
            if (text.empty())
                return std::nullopt;
            OGRSpatialReference srs;
            if (setSrsFromUserInput(srs, text.c_str()) != OGRERR_NONE)
                return std::nullopt;
            srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            return srs;
        }

        std::optional<int> epsgOf(const OGRSpatialReference &srs, const char *node)
        {
            const char *authority = srs.GetAuthorityName(node);
            const char *code = srs.GetAuthorityCode(node);
            if (authority == nullptr || code == nullptr || std::strcmp(authority, "EPSG") != 0)
                return std::nullopt;
            const int value = std::atoi(code);
            return value > 0 ? std::optional<int>(value) : std::nullopt;
        }

        OGRSpatialReference fromEpsg(int code)
        {
            OGRSpatialReference srs;
            if (srs.importFromEPSG(code) != OGRERR_NONE)
                throw GDALException("Cannot read EPSG:" + std::to_string(code) + ". Is PROJ available?");
            return srs;
        }

        // The EPSG CRS equivalent to srs, when there is one
        std::optional<int> identifyEpsg(const OGRSpatialReference &srs)
        {
            int count = 0;
            int *confidences = nullptr;
            OGRSpatialReferenceH *matches = srs.FindMatches(nullptr, &count, &confidences);
            std::optional<int> found;
            for (int i = 0; i < count && !found; i++)
                if (confidences[i] >= kEquivalentConfidence)
                    found = epsgOf(*OGRSpatialReference::FromHandle(matches[i]));
            OSRFreeSRSArray(matches);
            CPLFree(confidences);
            return found;
        }

        // The geographic 2-D CRS of the datum of srs (geographic, compound or geocentric)
        OGRSpatialReference geographic2D(const OGRSpatialReference &srs)
        {
            std::unique_ptr<OGRSpatialReference> geog(srs.CloneGeogCS());
            if (!geog)
                throw GDALException("Cannot find the geographic coordinate system of " + nameOf(srs));
            if (geog->GetAxesCount() == 3 && geog->DemoteTo2D(nullptr) != OGRERR_NONE)
                throw GDALException("Cannot take the height off " + nameOf(*geog));
            return *geog;
        }

        // The 6-degree UTM zone of a longitude
        int utmZone(double lon)
        {
            const int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
            return std::min(60, std::max(1, zone));
        }

        // UTM zone `zone` on the datum of geog when EPSG has it, else on WGS 84
        OGRSpatialReference utm(const OGRSpatialReference &geog, int zone, bool north)
        {
            OGRSpatialReference onDatum;
            if (onDatum.CopyGeogCSFrom(&geog) == OGRERR_NONE && onDatum.SetUTM(zone, north ? TRUE : FALSE) == OGRERR_NONE)
                if (const auto code = identifyEpsg(onDatum))
                    return fromEpsg(*code);
            return fromEpsg((north ? 32600 : 32700) + zone);
        }

        // A transverse Mercator on the datum of geog, of scale 1 at its central meridian
        OGRSpatialReference centredTransverseMercator(const OGRSpatialReference &geog, double centralMeridian)
        {
            // Rounded, so that the same footprint always gives the same text
            const double cm = std::round(centralMeridian * 1e6) / 1e6;
            std::ostringstream name;
            name << nameOf(geog) << " / Transverse Mercator " << std::fixed << std::setprecision(6) << cm;
            OGRSpatialReference tm;
            if (tm.SetProjCS(name.str().c_str()) != OGRERR_NONE || tm.CopyGeogCSFrom(&geog) != OGRERR_NONE ||
                tm.SetTM(0.0, cm, 1.0, 500000.0, 0.0) != OGRERR_NONE ||
                tm.SetLinearUnits(SRS_UL_METER, 1.0) != OGRERR_NONE)
                throw GDALException("Cannot make a transverse Mercator centred on " + std::to_string(cm));
            return tm;
        }

        // The vertical CRS of a compound srs
        std::optional<OGRSpatialReference> verticalOf(const OGRSpatialReference &srs)
        {
            if (!srs.IsCompound())
                return std::nullopt;
            const OGR_SRSNode *node = srs.GetAttrNode("VERT_CS");
            if (node == nullptr)
                return std::nullopt;
            char *wkt = nullptr;
            if (node->exportToWkt(&wkt) != OGRERR_NONE)
                return std::nullopt;
            const std::string text = takeGdalString(wkt);
            OGRSpatialReference vertical;
            if (vertical.importFromWkt(text.c_str()) != OGRERR_NONE)
                return std::nullopt;
            return vertical;
        }

        // The vertical CRS of the datum of `vertical` in metres: the COPC is metric on its three axes,
        // so that its octree is a cube on the ground too (NAVD88 height (ftUS) becomes NAVD88 height,
        // EPSG:5703). EPSG names the variants of a datum in another unit with the unit in brackets:
        // the CRS without them, in metres, is the one EPSG has, when it has one. Otherwise the vertical
        // CRS itself, its heights converted to metres.
        OGRSpatialReference metricVertical(const OGRSpatialReference &vertical)
        {
            if (std::abs(vertical.GetLinearUnits() - 1.0) < 1e-12)
                return vertical;
            OGRSpatialReference metres(vertical);
            if (metres.SetLinearUnits(SRS_UL_METER, 1.0) != OGRERR_NONE)
                throw GDALException("Cannot convert the heights of " + nameOf(vertical) + " to metres");

            std::string name = nameOf(vertical);
            const size_t bracket = name.rfind(" (");
            if (bracket != std::string::npos && name.back() == ')')
                name.erase(bracket);
            char *wkt = nullptr;
            if (metres.exportToWkt(&wkt) != OGRERR_NONE)
            {
                CPLFree(wkt);
                return metres;
            }
            std::string text = takeGdalString(wkt);
            const std::string head = "VERT_CS[\"" + nameOf(vertical) + "\"";
            OGRSpatialReference renamed;
            if (text.rfind(head, 0) == 0 &&
                renamed.importFromWkt(text.replace(0, head.size(), "VERT_CS[\"" + name + "\"").c_str()) == OGRERR_NONE)
            {
                if (const auto code = identifyEpsg(renamed))
                    return fromEpsg(*code);
                return renamed;
            }
            return metres;
        }

        // The text a builder reads: "EPSG:<code>", "EPSG:<h>+<v>" or WKT2
        std::string textOf(const OGRSpatialReference &srs, const OGRSpatialReference *horizontal,
                           const OGRSpatialReference *vertical)
        {
            if (const auto code = epsgOf(srs))
                return "EPSG:" + std::to_string(*code);
            if (horizontal != nullptr && vertical != nullptr)
            {
                const auto h = epsgOf(*horizontal);
                const auto v = epsgOf(*vertical);
                if (h && v)
                    return "EPSG:" + std::to_string(*h) + "+" + std::to_string(*v);
            }
            char *wkt = nullptr;
            const char *const options[] = {"FORMAT=WKT2_2019", "MULTILINE=NO", nullptr};
            if (srs.exportToWkt(&wkt, options) != OGRERR_NONE)
            {
                CPLFree(wkt);
                throw GDALException("Cannot write " + nameOf(srs) + " as WKT");
            }
            return takeGdalString(wkt);
        }
    }

    void LongitudeRange::add(double lon)
    {
        if (!std::isfinite(lon))
            return;
        const double a = normalizedLongitude(lon);
        const double b = a < 0.0 ? a + 360.0 : a;
        minA_ = std::min(minA_, a);
        maxA_ = std::max(maxA_, a);
        minB_ = std::min(minB_, b);
        maxB_ = std::max(maxB_, b);
    }

    double LongitudeRange::west() const
    {
        if (maxB_ - minB_ < maxA_ - minA_)
            return normalizedLongitude(minB_);
        return minA_;
    }

    double LongitudeRange::east() const
    {
        if (maxB_ - minB_ < maxA_ - minA_)
            return normalizedLongitude(minB_) + (maxB_ - minB_);
        return maxA_;
    }

    LonLatEnvelope continuousEnvelope(const LonLatEnvelope &envelope)
    {
        LonLatEnvelope out = envelope;
        double west = envelope.minLon, east = envelope.maxLon;
        if (east - west > 180.0)
        {
            // Written from -180 to 180 across the antimeridian: the cloud is between the greatest
            // longitude and the least
            west = envelope.maxLon;
            east = envelope.minLon + 360.0;
        }
        const double shift = normalizedLongitude(west) - west;
        out.minLon = west + shift;
        out.maxLon = east + shift;
        return out;
    }

    const char *toString(PointCloudCrsStatus status)
    {
        for (const auto &s : kStatusNames)
            if (s.status == status)
                return s.name;
        return "none";
    }

    std::optional<PointCloudCrsStatus> parsePointCloudCrsStatus(std::string_view name)
    {
        for (const auto &s : kStatusNames)
            if (name == s.name)
                return s.status;
        return std::nullopt;
    }

    PointCloudCrsCheck PointCloudCrsPolicy::classify(const std::string &wkt, const std::vector<double> &bounds)
    {
        PointCloudCrsCheck check;
        const SrsPtr srs = srsFromWkt(wkt.c_str());
        if (!srs)
            return check;
        const OGRSpatialReference &s = srsOf(srs);
        check.status = kindOf(s);
        if (bounds.size() != 6)
            return check;

        bool fits = true;
        switch (check.status)
        {
        case PointCloudCrsStatus::Geographic:
            fits = geographicFits(s, bounds);
            break;
        case PointCloudCrsStatus::Geocentric:
            fits = geocentricFits(s, bounds);
            break;
        case PointCloudCrsStatus::Projected:
            fits = projectedFits(s, bounds);
            break;
        default:
            break;
        }
        if (!fits)
        {
            check.status = PointCloudCrsStatus::Implausible;
            check.warning = "coordinate system ignored: the coordinates do not fit " + nameOf(s);
        }
        return check;
    }

    bool PointCloudCrsPolicy::reprojects(PointCloudCrsStatus status)
    {
        return status == PointCloudCrsStatus::Geographic || status == PointCloudCrsStatus::Geocentric;
    }

    bool PointCloudCrsPolicy::changesCrs(PointCloudCrsStatus status)
    {
        return reprojects(status) || status == PointCloudCrsStatus::Implausible;
    }

    std::string PointCloudCrsPolicy::target(const std::string &wkt, PointCloudCrsStatus status,
                                            const LonLatEnvelope &footprint)
    {
        if (!reprojects(status))
            return {};
        const SrsPtr srs = srsFromWkt(wkt.c_str());
        if (!srs)
            throw GDALException("Cannot read the coordinate reference system of the file. Is PROJ available?");
        const OGRSpatialReference &source = srsOf(srs);
        const OGRSpatialReference geog = geographic2D(source);

        // On the circle: the centre of a cloud across the antimeridian is there, and its distance
        // to a central meridian is measured the short way
        const LonLatEnvelope continuous = continuousEnvelope(footprint);
        const double centre = (continuous.minLon + continuous.maxLon) / 2.0;
        const double lon = normalizedLongitude(centre);
        const double lat = (continuous.minLat + continuous.maxLat) / 2.0;
        OGRSpatialReference horizontal;
        if (lat > kUtmNorthLimit)
            horizontal = fromEpsg(kUpsNorthEpsg);
        else if (lat < kUtmSouthLimit)
            horizontal = fromEpsg(kUpsSouthEpsg);
        else
        {
            const int zone = utmZone(lon);
            // The central meridian on the side of the circle of the continuous range
            const double centralMeridian = zone * 6.0 - 183.0 + (centre - lon);
            if (continuous.minLon < centralMeridian - kUtmReachDegrees ||
                continuous.maxLon > centralMeridian + kUtmReachDegrees)
                horizontal = centredTransverseMercator(geog, lon);
            else
                horizontal = utm(geog, zone, lat >= 0.0);
        }

        // Z: the height of a geocentric cloud, or of a geographic 3-D one, is the ellipsoidal height
        if (status == PointCloudCrsStatus::Geocentric || (!source.IsCompound() && source.GetAxesCount() == 3))
        {
            OGRSpatialReference threeD(horizontal);
            if (threeD.PromoteTo3D(nullptr) != OGRERR_NONE)
                throw GDALException("Cannot add the ellipsoidal height to " + nameOf(horizontal));
            return textOf(threeD, nullptr, nullptr);
        }
        if (const auto stated = verticalOf(source))
        {
            const std::optional<OGRSpatialReference> vertical = metricVertical(*stated);
            OGRSpatialReference compound;
            const std::string name = nameOf(horizontal) + " + " + nameOf(*vertical);
            if (compound.SetCompoundCS(name.c_str(), &horizontal, &*vertical) != OGRERR_NONE)
                throw GDALException("Cannot make the compound CRS " + name);
            return textOf(compound, &horizontal, &*vertical);
        }
        return textOf(horizontal, nullptr, nullptr);
    }

    bool PointCloudCrsPolicy::isSame(const std::string &a, const std::string &b)
    {
        const auto first = readCrs(a);
        const auto second = readCrs(b);
        return first && second && first->IsSame(&*second, kIsSameOptions);
    }

    std::optional<CrsName> crsNameOf(const std::string &crs)
    {
        const auto srs = readCrs(crs);
        if (!srs)
            return std::nullopt;
        return CrsName{nameOf(*srs), epsgOf(*srs)};
    }
}
