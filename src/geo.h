/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GEO_H
#define GEO_H

#include <iostream>
#include "utils.h"
#include "ddb_export.h"

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

template <typename T>
struct Projected2D_t{
    T x;
    T y;
    DDB_DLL Projected2D_t() : x(0.0), y(0.0) {};
    DDB_DLL Projected2D_t(T x, T y) : x(x), y(y) {};

    DDB_DLL void rotate(const Projected2D_t &center, double degrees){
        double px = this->x;
        double py = this->y;
        double radians = utils::deg2rad(degrees);
        x = cos(radians) * (px - center.x) - sin(radians) * (py - center.y) + center.x;
        y = sin(radians) * (px - center.x) + cos(radians) * (py - center.y) + center.y;
    }

    DDB_DLL void transform(double *affine){
        x = static_cast<T>(affine[0] + x*affine[1] + y*affine[2]);
        y = static_cast<T>(affine[3] + x*affine[4] + y*affine[5]);
    }
};
typedef Projected2D_t<double> Projected2D;
typedef Projected2D_t<double> Point2D;
typedef Projected2D_t<int> Projected2Di;


inline std::ostream& operator<<(std::ostream& os, const Projected2D& p)
{
    os << "[" << p.x << ", " << p.y << "]";
    return os;
}

template <typename T>
struct BoundingBox{
    T max;
    T min;

    DDB_DLL BoundingBox(): max(T()), min(T()){}
    DDB_DLL BoundingBox(T min, T max): max(max), min(min){}
    DDB_DLL bool contains(const T &p){
        return contains(p.x, p.y);
    }
    template <typename N>
    DDB_DLL bool contains(N x, N y){
        return x >= min.x &&
                x <= max.x &&
                y >= min.y &&
                y <= max.y;
    }
};

template <typename T>
inline std::ostream& operator<<(std::ostream& os, const BoundingBox<T>& b)
{
    os << "[" << b.min << "],[" << b.max << "]";
    return os;
}

struct Geographic2D{
    double latitude;
    double longitude;
    DDB_DLL Geographic2D(){ latitude = longitude = 0.0; }
    DDB_DLL Geographic2D(double longitude, double latitude) : longitude(longitude), latitude(latitude) {};
};
inline std::ostream& operator<<(std::ostream& os, const Geographic2D& p)
{
    os << "[" << p.latitude << ", " << p.longitude << "]";
    return os;
}

DDB_DLL double copysignx(double x, double y);
DDB_DLL double remainderx(double x, double y);
DDB_DLL double angNormalize(double x);
DDB_DLL int latitudeBand(double latitude);
DDB_DLL int standardUTMZone(double latitude, double longitude);

DDB_DLL UTMZone getUTMZone(double latitude, double longitude);
DDB_DLL std::string getProjForUTM(const UTMZone &zone);
DDB_DLL Projected2D toUTM(double latitude, double longitude, const UTMZone &zone);

DDB_DLL Geographic2D fromUTM(const Projected2D &p, const UTMZone &zone);
DDB_DLL Geographic2D fromUTM(double x, double y, const UTMZone &zone);

}

#endif // GEO_H
