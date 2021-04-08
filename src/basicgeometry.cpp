/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "basicgeometry.h"
#include "exceptions.h"
#include "utils.h"

namespace ddb{

std::string BasicPointGeometry::toWkt() const{
    if (empty()) return "";
    return utils::stringFormat("POINT Z (%lf %lf %lf)", points[0].x, points[0].y, points[0].z);
}

json BasicPointGeometry::toGeoJSON() const{
    json j;
    initGeoJsonBase(j);
    j["geometry"]["type"] = "Point";
    j["geometry"]["coordinates"] = json::array();

    if (!empty()){
        j["geometry"]["coordinates"] += points[0].x;
        j["geometry"]["coordinates"] += points[0].y;
        j["geometry"]["coordinates"] += points[0].z;
    }
    return j;
}

std::string BasicPolygonGeometry::toWkt() const{
    if (empty()) return "";

    std::ostringstream os;
    os << "POLYGONZ ((";
    bool first = true;
    for (auto &p : points){
        if (!first) os << ", ";
        os << std::setprecision(13) << p.x << " " << p.y << " " << p.z;
        first = false;
    }
    os << "))";
    return os.str();
}

json BasicPolygonGeometry::toGeoJSON() const{
    json j;
    initGeoJsonBase(j);
    j["geometry"]["type"] = "Polygon";
    j["geometry"]["coordinates"] = json::array();
    json poly = json::array();

    for (auto &p : points){
        json c = json::array();
        c += p.x;
        c += p.y;
        c += p.z;

        poly += c;
    }

    j["geometry"]["coordinates"] += poly;

    return j;
}

void BasicGeometry::addPoint(const Point &p){
    points.push_back(p);
}

void BasicGeometry::addPoint(double x, double y, double z){
    points.push_back(Point(x, y, z));
}

Point BasicGeometry::getPoint(int index){
    if (index >= static_cast<int>(points.size())) throw AppException("Out of bounds exception");
    return points[index];
}

bool BasicGeometry::empty() const{
    return points.empty();
}

void BasicGeometry::clear(){
    points.clear();
}

int BasicGeometry::size() const{
    return static_cast<int>(points.size());
}

void BasicGeometry::initGeoJsonBase(json &j) const{
    j["type"] = "Feature";
    j["crs"] = json();
    j["crs"]["type"] = "name";
    j["crs"]["properties"] = json();
    j["crs"]["properties"]["name"] = "EPSG:4326";

    j["geometry"] = json({});
    j["properties"] = json({});
}

BasicGeometryType getBasicGeometryTypeFromName(const std::string &name){
    if (name == "auto") return BasicGeometryType::BGAuto;
    else if (name == "point") return BasicGeometryType::BGPoint;
    else if (name == "polygon") return BasicGeometryType::BGPolygon;

    throw InvalidArgsException("Invalid basic geometry type " + name);
}

}
