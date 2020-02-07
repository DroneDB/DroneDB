#ifndef ENTRY_H
#define ENTRY_H

#include <filesystem>
#include "json_fwd.hpp"
#include "json.hpp"
#include "types.h"
#include "../logger.h"
#include "../classes/exceptions.h"
#include "../utils.h"
#include "../classes/hash.h"
#include "../classes/exif.h"
#include "../libs/geo.h"

namespace fs = std::filesystem;

namespace entry {

using json = nlohmann::json;

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
    BasicGeometry(){}

    void addPoint(const Point &p){
        points.push_back(p);
    }

    void addPoint(double x, double y, double z){
        points.push_back(Point(x, y, z));
    }

    Point getPoint(int index){
        if (index >= points.size()) throw AppException("Out of bounds exception");
        return points[index];
    }

    std::string toWktPointZ() const{
        if (empty()) return "";
        else{
            return utils::stringFormat("POINT Z (%lf %lf %lf)", points[0].x, points[0].y, points[0].z);
        }
    }

    std::string toWktPolygonZ() const{
        if (empty()) return "";
        else{
            std::ostringstream os;
            os << "POLYGONZ ((";
            bool first = true;
            for (auto &p : points){
                if (!first) os << ", ";
                os << p.x << " " << p.y << " " << p.z;
                first = false;
            }
            os << "))";
            return os.str();
        }
    }

    bool empty() const{
        return points.empty();
    }
    int size() const{
        return points.size();
    }
    std::vector<Point> points;
};
inline std::ostream& operator<<(std::ostream& os, const BasicGeometry& g)
{
    os << "[";
    for (auto &p : g.points){
        os << p << " ";
    }
    os << "]";
    return os;
}

struct Entry {
    std::string path = "";
    std::string hash = "";
    Type type = Type::Undefined;
    json meta;
    time_t mtime = 0;
    off_t size = 0;
    int depth = 0;

    BasicGeometry point_geom;
    BasicGeometry polygon_geom;

    void toJSON(json &j){
        j["path"] = this->path;
        if (this->hash != "") j["hash"] = this->hash;
        j["type"] = this->type;
        j["meta"] = this->meta;
        j["mtime"] = this->mtime;
        j["size"] = this->size;
        //j["depth"] = this->depth;

        if (!this->point_geom.empty()) j["point_geom"] = this->point_geom.toWktPointZ();
        if (!this->polygon_geom.empty()) j["polygon_geom"] = this->polygon_geom.toWktPolygonZ();
    }

    std::string toString(){
        std::ostringstream s;
        s << "Path: " << this->path << "\n";
        if (this->hash != "") s << "SHA256: " << this->hash << "\n";
        s << "Type: " << typeToHuman(this->type) << " (" << this->type << ")" << "\n";

        for (json::iterator it = this->meta.begin(); it != this->meta.end(); ++it) {
            std::string k = it.key();
            if (k.length() > 0) k[0] = std::toupper(k[0]);

            s << k << ": " << (it.value().is_string() ?
                               it.value().get<std::string>() :
                               it.value().dump()) << "\n";
        }

        s << "Modified Time: " << this->mtime << "\n";
        s << "Size: " << utils::bytesToHuman(this->size) << "\n";
        //s << "Tree Depth: " << this->depth << "\n";
        if (!this->point_geom.empty()) s << "Point Geometry: " << this->point_geom  << "\n";
        if (!this->polygon_geom.empty()) s << "Polygon Geometry: " << this->polygon_geom << "\n";

        return s.str();
    }
};

bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool computeHash = true);
void calculateFootprint(const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);

}

#endif // ENTRY_H
