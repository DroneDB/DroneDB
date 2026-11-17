/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_CRS_H
#define POINTCLOUD_CRS_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ddb
{
    // What the coordinate reference system a point cloud states is, once it is checked against the
    // coordinates of the cloud (PointCloudCrsPolicy::classify)
    enum class PointCloudCrsStatus
    {
        Projected,   // projected, in any linear unit, or compound with a projected horizontal part
        Geographic,  // longitude and latitude (2-D, 3-D, or compound with a vertical CRS)
        Geocentric,  // earth-centred X, Y, Z
        Local,       // an engineering (site) CRS: no position on the earth
        None,        // no CRS, or one that cannot be read
        Implausible  // a CRS the coordinates do not fit: the cloud is handled as having none
    };

    // "projected", "geographic", "geocentric", "local", "none", "implausible": the `crsStatus`
    // property of a point cloud entry
    const char *toString(PointCloudCrsStatus status);
    std::optional<PointCloudCrsStatus> parsePointCloudCrsStatus(std::string_view name);

    // A longitude/latitude envelope in WGS 84
    struct LonLatEnvelope
    {
        double minLon = 0;
        double minLat = 0;
        double maxLon = 0;
        double maxLat = 0;
    };

    // The longitudes of a cloud as one range on the circle, in degrees: the narrower of their range in
    // [-180, 180) and in [0, 360), so that a cloud across the antimeridian is one narrow range (179.9
    // to 180.1) and not the whole globe. A cloud never spans more than 180 degrees of longitude.
    class LongitudeRange
    {
      public:
        void add(double lon);
        bool empty() const { return !(minA_ <= maxA_); }
        // West in [-180, 180), east = west + the width of the range (past 180 for a range across the
        // antimeridian)
        double west() const;
        double east() const;

      private:
        double minA_ = 1e300, maxA_ = -1e300; // in [-180, 180)
        double minB_ = 1e300, maxB_ = -1e300; // in [0, 360)
    };

    // The envelope of a cloud whose longitudes are only known by their least and greatest value (a
    // header), as a continuous range: west in [-180, 180), east = west + width. A range wider than 180
    // degrees is a cloud across the antimeridian whose longitudes are written from -180 to 180 (its
    // west is the greatest, its east the least plus 360); 0 to 360 is folded into -180 to 180.
    LonLatEnvelope continuousEnvelope(const LonLatEnvelope &envelope);

    // The status of a cloud's CRS and, for one the coordinates do not fit, the warning of its entry
    struct PointCloudCrsCheck
    {
        PointCloudCrsStatus status = PointCloudCrsStatus::None;
        // "coordinate system ignored: the coordinates do not fit <name>" for Implausible, else ""
        std::string warning;
    };

    // The one place that decides in which CRS the COPC of a point cloud is built. The raster helper
    // applies it when it reads a cloud (pointCloudInfo), and the host when it builds one.
    //
    // A COPC holds one isotropic cube in the units of its CRS: a cloud in degrees (geographic) has a
    // cube sized by its height range in metres read as degrees, and a geocentric one is tilted, so
    // their octrees do not split the points and their tiles cost seconds and gigabytes. Their COPC
    // is built in a metric, conformal projected CRS instead, chosen from the footprint of the cloud;
    // a cloud in a projected CRS (in metres or feet), in a local CRS or without one keeps its own.
    // The source file and its entry (projection, footprint) are not changed.
    class PointCloudCrsPolicy
    {
      public:
        // The version of the rules below. A COPC records it (CopcBuildRecord::crsPolicy); the COPC
        // of a cloud whose CRS it changes (changesCrs) that records an older version, or none, is
        // rebuilt.
        static constexpr int kVersion = 1;

        // The status of the CRS `wkt` states for a cloud with `bounds` (min x, min y, min z, max x,
        // max y, max z, in the units of the CRS; empty when they are not known, and then the CRS is
        // not checked against them). The CRS fits the coordinates when:
        //   - geographic: x is within [-180, 360] (-180 to 180 or 0 to 360, also across the
        //     antimeridian), spanning at most 360 degrees, and y within [-90, 90] (longitude first,
        //     as LAS stores them whatever the axis order of the authority);
        //   - geocentric: every corner of the bounds is 6,300 to 6,420 km from the centre of the
        //     earth;
        //   - projected: the corners of the bounds transform to WGS 84, to finite longitudes and
        //     latitudes within [-90, 90], no more than kProjectedMarginDegrees outside the area of
        //     use of the CRS (a CRS is used far past its area: Norway maps up to 31 E, Svalbard 34
        //     E, in EUREF89 / UTM 33, whose area ends at 18 E). A CRS without an area of use, or
        //     without a transformation to WGS 84, is not judged.
        static PointCloudCrsCheck classify(const std::string &wkt, const std::vector<double> &bounds);

        // How far outside the area of use of a projected CRS the coordinates may lie, in degrees
        static constexpr double kProjectedMarginDegrees = 30.0;

        // Whether the COPC of a cloud of this status is built in a CRS of its own (target())
        static bool reprojects(PointCloudCrsStatus status);

        // Whether the COPC of a cloud of this status is in another CRS than the one the cloud states:
        // a CRS of its own (reprojects), or none for a CRS the coordinates do not fit (the COPC states
        // none, so that no reader of it places the cloud where that CRS says)
        static bool changesCrs(PointCloudCrsStatus status);

        // The CRS the COPC of a cloud is built in: "" to keep the CRS of the cloud (any status but
        // Geographic and Geocentric). `footprint` is the cloud's envelope in WGS 84 (continuousEnvelope
        // reads it, so a cloud across the antimeridian is centred there). The horizontal part is:
        //   - UPS North (EPSG:5041) when the centre is north of 84 degrees, UPS South (EPSG:5042)
        //     south of 80 degrees south, where UTM ends;
        //   - else a transverse Mercator centred on the footprint when the footprint reaches more than
        //     4.5 degrees from the central meridian of the UTM zone of its centre (the zone of the
        //     6-degree band, without the Norway and Svalbard exceptions), where the scale of UTM
        //     is 0.25 % off at the equator: a small cloud across the edge of a zone stays in UTM;
        //   - else that UTM zone, on the datum of the cloud when EPSG has a UTM CRS of that datum
        //     (ETRS89 / UTM zone 33N, NAD83(2011) / UTM zone 17N), on WGS 84 otherwise.
        // A compound source keeps its vertical CRS, in metres (a vertical CRS in feet becomes the
        // one of its datum in metres, NAVD88 height (ftUS) EPSG:5703, or its heights are converted),
        // so that the three axes of the COPC are metric; a geographic 3-D or geocentric source gets
        // the horizontal part in 3-D (Z the ellipsoidal height).
        // The text is "EPSG:<code>", "EPSG:<horizontal>+<vertical>", or a WKT2 (2019) text for a
        // CRS without such codes.
        // @throws GDALException when PROJ cannot make the CRS
        static std::string target(const std::string &wkt, PointCloudCrsStatus status, const LonLatEnvelope &footprint);

        // Whether two CRS texts (WKT, "EPSG:<code>", any text GDAL reads) are the same CRS, whatever
        // their spelling. False when either cannot be read.
        static bool isSame(const std::string &a, const std::string &b);
    };

    // The name of a CRS text ("WGS 84 / UTM zone 33N") and its EPSG code, when it has one (a
    // compound CRS has none); nothing when it cannot be read
    struct CrsName
    {
        std::string name;
        std::optional<int> epsg;
    };
    std::optional<CrsName> crsNameOf(const std::string &crs);
}

#endif // POINTCLOUD_CRS_H
