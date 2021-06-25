/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include <ddb.h>
#include <pointcloud.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

enum BuildResult { Built, Skipped, Unknown };

BuildResult build_internal(Database* db, const Entry& e,
                           const std::string& outputPath, std::ostream& output,
                           bool force) {
    LOGD << "Building entry " << e.path << " type " << e.type;

    // We could vectorize this logic, but it's an overkill by now
    if (e.type == PointCloud) {
        const auto o = (fs::path(outputPath) / e.hash).generic_string();

        if (fs::exists(o) && !force) {
            output << "Skipping point cloud '" << e.path << "' because folder '"
                   << o << "' already exists" << std::endl;
            return Skipped;
        }

        const auto relativePath =
            (fs::path(db->getOpenFile()).parent_path().parent_path() / e.path)
                .generic_string();

        LOGD << "Relative path " << relativePath;

        const std::vector vec = {relativePath};

        output << "Building point cloud '" << e.path << "' in folder '" << o
               << "'" << std::endl;

        buildEpt(vec, o);

        return Built;
    }

    return Unknown;
}

void build_all(Database* db, const std::string& outputPath,
               std::ostream& output, bool force) {
    LOGD << "In build_all('" << outputPath << "')";

    output << "Searching for buildable files" << std::endl;
    int cnt = 0;
    int skipped = 0;
    int unknown = 0;

    // List all matching files in DB
    auto q = db->query(ENTRY_MATCHER_QUERY);

    while (q->fetch()) {
        Entry e(*q);

        // Call build on every of them
        const auto res = build_internal(db, e, outputPath, output, force);

        switch (res) {
            case Built:
                cnt++;
                break;
            case Skipped:
                skipped++;
                break;
            case Unknown:
                unknown++;
                break;
        }
    }

    if (cnt + skipped + unknown == 0)
        output << "No buildable files found" << std::endl;
    else
        output << cnt << " file(s) built, " << skipped << " file(s) skipped, "
               << unknown << " file(s) unknown" << std::endl;
}

void build(Database* db, const std::string& path, const std::string& outputPath,
           std::ostream& output, bool force) {
    LOGD << "In build('" << path << "','" << outputPath << "')";

    Entry e;

    const bool entryExists = getEntry(db, path, &e) != nullptr;
    if (!entryExists) throw InvalidArgsException("Entry does not exist");

    const auto res = build_internal(db, e, outputPath, output, force);

    switch (res) {
        case Built:
            output << "File built" << std::endl;
            break;
        case Skipped:
            output << "File skipped (already existing)" << std::endl;
            break;
        case Unknown:
            output << "File unknown" << std::endl;
            break;
    }
}

}  // namespace ddb
