/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GEO_H
#define GEO_H

#include <iostream>
#include "utils.h"

namespace ddb{

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

    void rotate(const Projected2D &center, double degrees);
    void transform(double *affine);
};
typedef Projected2D Point2D;

inline std::ostream& operator<<(std::ostream& os, const Projected2D& p)
{
    os << "[" << p.x << ", " << p.y << "]";
    return os;
}

template <typename T>
struct BoundingBox{
    T max;
    T min;

    BoundingBox(): max(T()), min(T()){}
    BoundingBox(T min, T max): max(max), min(min){}
    bool contains(const T &p){
        return contains(p.x, p.y);
    }
    template <typename N>
    bool contains(N x, N y){
        return x >= min.x &&
                x <= max.x &&
                y >= min.y &&
                y <= max.y;
    }
};

template <typename T>
inline std::ostream& operator<<(std::ostream& os, const BoundingBox<T>& b)
{
    os << "[" << b.min << "],[" << b.max << "]]";
    return os;
}

struct Geographic2D{
    double latitude;
    double longitude;
    Geographic2D(){ latitude = longitude = 0.0; }
    Geographic2D(double longitude, double latitude) : longitude(longitude), latitude(latitude) {};
};
inline std::ostream& operator<<(std::ostream& os, const Geographic2D& p)
{
    os << "[" << p.latitude << ", " << p.longitude << "]";
    return os;
}

double copysignx(double x, double y);
double remainderx(double x, double y);
double angNormalize(double x);
int latitudeBand(double latitude);
int standardUTMZone(double latitude, double longitude);

UTMZone getUTMZone(double latitude, double longitude);
std::string getProjForUTM(const UTMZone &zone);
Projected2D toUTM(double latitude, double longitude, const UTMZone &zone);

Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone);
Geographic2D fromUTM(double x, double y, const UTMZone &zone);

}

#endif // GEO_H
