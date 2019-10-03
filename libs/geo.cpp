/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
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

}

