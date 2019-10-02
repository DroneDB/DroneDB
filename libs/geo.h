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
#ifndef GEO_H
#define GEO_H

#include <iostream>

namespace geo{

struct UTMZone{
    bool north;
    int zone;
};
inline std::ostream& operator<<(std::ostream& os, const UTMZone& z)
{
    os << z.zone << (z.north ? 'N' : 'S');
    return os;
}

struct Projected2D{
    double x;
    double y;
    Projected2D() : x(0.0), y(0.0) {};
    Projected2D(double x, double y) : x(x), y(y) {};
};
inline std::ostream& operator<<(std::ostream& os, const Projected2D& p)
{
    os << p.x << " " << p.y;
    return os;
}

struct Geographic2D{
    double latitude;
    double longitude;
};
inline std::ostream& operator<<(std::ostream& os, const Geographic2D& p)
{
    os << p.latitude << " " << p.longitude;
    return os;
}

UTMZone getUTMZone(double latitude, double longitude);
Projected2D toUTM(double latitude, double longitude, const UTMZone &zone);

Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone);
Geographic2D fromUTM(double x, double y, const UTMZone &zone);

}

#endif // GEO_H
