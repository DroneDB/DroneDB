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

struct Entry {
    std::string path = "";
    std::string hash = "";
    Type type = Type::Undefined;
    std::string meta = "";
    time_t mtime = 0;
    off_t size = 0;
    int depth = 0;

    std::string point_geom = "";
    std::string polygon_geom = "";
};

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry);
std::string calculateFootprint(const exif::ImageSize &imsize, const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude);

}

#endif // ENTRY_H
