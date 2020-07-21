/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BASICGEOMETRY_H
#define BASICGEOMETRY_H

#include <string>
#include <iomanip>
#include "json.h"

namespace ddb{

struct Point{
    double x;
    double y;
    double z;
    Point(double x, double y, double z) : x(x), y(y), z(z) {}
    Point() : x(0.0), y(0.0), z(0.0) {}
};
inline std::ostream& operator<<(std::ostream& os, const Point& p){
     os << "[" << p.x << ", " << p.y << ", " << p.z << "]";
    return os;
}

struct BasicGeometry{
	BasicGeometry() {}

    void addPoint(const Point &p);
    void addPoint(double x, double y, double z);
    Point getPoint(int index);
    bool empty() const;
    int size() const;

    virtual std::string toWkt() const = 0;
    virtual json toGeoJSON() const = 0;

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
    virtual std::string toWkt() const override;
    virtual json toGeoJSON() const override;
};

struct BasicPolygonGeometry : BasicGeometry{
    virtual std::string toWkt() const override;
    virtual json toGeoJSON() const override;
};

enum BasicGeometryType {
    BGAuto, BGPoint, BGPolygon
};

BasicGeometryType getBasicGeometryTypeFromName(const std::string &name);

}

#endif // BASICGEOMETRY_H
