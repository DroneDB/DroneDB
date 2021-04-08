/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BASICGEOMETRY_H
#define BASICGEOMETRY_H

#include <string>
#include <iomanip>
#include "json.h"
#include "ddb_export.h"

namespace ddb{

struct Point{
    double x;
    double y;
    double z;
    DDB_DLL Point(double x, double y, double z) : x(x), y(y), z(z) {}
    DDB_DLL Point(double x, double y) : x(x), y(y), z(0.0) {}
    DDB_DLL Point() : x(0.0), y(0.0), z(0.0) {}
};
inline std::ostream& operator<<(std::ostream& os, const Point& p){
     os << "[" << p.x << ", " << p.y << ", " << p.z << "]";
    return os;
}

struct BasicGeometry{
    DDB_DLL BasicGeometry() {}

    DDB_DLL void addPoint(const Point &p);
    DDB_DLL void addPoint(double x, double y, double z);
    DDB_DLL Point getPoint(int index);
    DDB_DLL bool empty() const;
    DDB_DLL void clear();
    DDB_DLL int size() const;

    DDB_DLL virtual std::string toWkt() const = 0;
    DDB_DLL virtual json toGeoJSON() const = 0;

    std::vector<Point> points;
protected:
    void initGeoJsonBase(json &j) const;
};
inline std::ostream& operator<<(std::ostream& os, const BasicGeometry& g)
{
    os << "[";
    for (auto &p : g.points){
        os << std::setprecision(13) << p << " ";
    }
    os << "]";
    return os;
}

struct BasicPointGeometry : BasicGeometry{
    DDB_DLL virtual std::string toWkt() const override;
    DDB_DLL virtual json toGeoJSON() const override;
};

struct BasicPolygonGeometry : BasicGeometry{
    DDB_DLL virtual std::string toWkt() const override;
    DDB_DLL virtual json toGeoJSON() const override;
};

enum BasicGeometryType {
    BGAuto, BGPoint, BGPolygon
};

DDB_DLL BasicGeometryType getBasicGeometryTypeFromName(const std::string &name);

}

#endif // BASICGEOMETRY_H
