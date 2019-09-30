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
};

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry);

}

#endif // ENTRY_H
