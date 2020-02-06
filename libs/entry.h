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

namespace ddb {

using json = nlohmann::json;

struct Entry {
    std::string path = "";
    std::string hash = "";
    Type type = Type::Undefined;
    json meta;
    time_t mtime = 0;
    off_t size = 0;
    int depth = 0;

    std::string point_geom = "";
    std::string polygon_geom = "";

    void toJSON(json &j){
        j["path"] = this->path;
        j["hash"] = this->hash;
        j["type"] = this->type;
        j["meta"] = this->meta;
        j["mtime"] = this->mtime;
        j["size"] = this->size;
        j["depth"] = this->depth;
        j["point_geom"] = this->point_geom;
        j["polygon_geom"] = this->polygon_geom;
    }

    std::string toString(){
        std::ostringstream s;
        s << "Path: " << this->path << "\n";
        s << "SHA256: " << (this->hash != "" ? this->hash : "<none>") << "\n";
        s << "Type: " << typeToHuman(this->type) << " (" << this->type << ")" << "\n";

        for (json::iterator it = this->meta.begin(); it != this->meta.end(); ++it) {
            std::string k = it.key();
            if (k.length() > 0) k[0] = std::toupper(k[0]);

            s << k << ": " << it.value().dump() << "\n";
        }

        s << "Modified Time: " << this->mtime << "\n";
        s << "Size: " << this->size << "\n";
        s << "Directory Depth: " << this->depth << "\n";
        s << "Point Geometry: " << (this->point_geom != "" ? this->point_geom : "<none>") << "\n";
        s << "Polygon Geometry: " << (this->polygon_geom != "" ? this->polygon_geom : "<none>") << "\n";

        return s.str();
    }
};

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry);
std::string calculateFootprint(const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude);

}

#endif // ENTRY_H
