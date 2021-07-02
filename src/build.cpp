/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include <ddb.h>
#include <pointcloud.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

void build_internal(Database* db, const Entry& e,
                           const std::string& outputPath, std::ostream& output,
                           bool force) {
                               
    LOGD << "Building entry " << e.path << " type " << e.type;

    // We could vectorize this logic, but it's an overkill by now
    if (e.type == PointCloud) {
        const auto o = (fs::path(outputPath) / e.hash).generic_string();

        if (fs::exists(o) && !force) {
            return;
        }

        const auto relativePath =
            (fs::path(db->getOpenFile()).parent_path().parent_path() / e.path)
                .generic_string();

        LOGD << "Relative path " << relativePath;

        const std::vector vec = {relativePath};

        buildEpt(vec, o);

        output << o << std::endl;
    }
}

void build_all(Database* db, const std::string& outputPath,
               std::ostream& output, bool force) {

    LOGD << "In build_all('" << outputPath << "')";

    // List all matching files in DB
    auto q = db->query("SELECT path, hash, type, meta, mtime, size, depth FROM entries WHERE type = ?");
    q->bind(1, EntryType::PointCloud);

    while (q->fetch()) {
        Entry e(*q);

        // Call build on each of them
        build_internal(db, e, outputPath, output, force);
    }
}

void build(Database* db, const std::string& path, const std::string& outputPath,
           std::ostream& output, bool force) {

    LOGD << "In build('" << path << "','" << outputPath << "')";

    Entry e;

    const bool entryExists = getEntry(db, path, &e) != nullptr;
    if (!entryExists) throw InvalidArgsException(path + " is not a valid path in the database.");

    build_internal(db, e, outputPath, output, force);
}

}  // namespace ddb
