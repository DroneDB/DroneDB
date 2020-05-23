/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <GeographicLib/UTMUPS.hpp>
#include "geo.h"

using namespace GeographicLib;

namespace geo {

UTMZone getUTMZone(double latitude, double longitude) {
    UTMZone z;
    z.zone = UTMUPS::StandardZone(latitude, longitude);
    z.north = latitude >= 0;
    return z;
}

Projected2D toUTM(double latitude, double longitude, const geo::UTMZone &zone) {
    int _z;
    bool _n;
    Projected2D result;

    UTMUPS::Forward(latitude, longitude, _z, _n, result.x, result.y, zone.zone);
    return result;
}

Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone) {
    return fromUTM(p.x, p.y, zone);
}

Geographic2D fromUTM(double x, double y, const UTMZone &zone) {
    Geographic2D result;
    UTMUPS::Reverse(zone.zone, zone.north, x, y, result.latitude, result.longitude);
    return result;
}

void Projected2D::rotate(const Projected2D &center, double degrees) {
    double px = this->x;
    double py = this->y;
    double radians = utils::deg2rad(degrees);
    x = cos(radians) * (px - center.x) - sin(radians) * (py - center.y) + center.x;
    y = sin(radians) * (px - center.x) + cos(radians) * (py - center.y) + center.y;
}

void Projected2D::transform(double *affine){
     x = affine[0] + x*affine[1] + y*affine[2];
     y = affine[3] + x*affine[4] + y*affine[5];
}

}

