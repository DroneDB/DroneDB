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
        os << p << " ";
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

struct Entry {
    std::string path = "";
    std::string hash = "";
    Type type = Type::Undefined;
    json meta;
    time_t mtime = 0;
    off_t size = 0;
    int depth = 0;

    BasicPointGeometry point_geom;
    BasicPolygonGeometry polygon_geom;

    void toJSON(json &j);
    bool toGeoJSON(json &j);
    std::string toString();
};

struct ParseEntryOpts{
    bool withHash = true;
    bool stopOnError = true;
};

bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, ParseEntryOpts &opts);
void calculateFootprint(const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom);

}

#endif // ENTRY_H
