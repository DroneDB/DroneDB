/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include "ddb.h"
#include "pointcloud.h"
#include "cog.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb {

bool isBuildableInternal(const Entry& e, std::string& subfolder) {

    if (e.type == PointCloud) {
        subfolder = "ept";
        return true;
    }else if (e.type == GeoRaster){
        subfolder = "cog";
        return true;
    }

    return false;

}

bool isBuildable(Database* db, const std::string& path, std::string& subfolder) {
    
    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists) throw InvalidArgsException(path + " is not a valid path in the database.");

    return isBuildableInternal(e, subfolder);
}

void buildInternal(Database* db, const Entry& e,
                           const std::string& outputPath, std::ostream& output,
                           bool force) {
                               
    LOGD << "Building entry " << e.path << " type " << e.type;

    const auto baseOutputPath = fs::path(outputPath) / e.hash;
    std::string outputFolder;
    std::string subfolder;

    if (isBuildableInternal(e, subfolder)) {
        outputFolder = (baseOutputPath / subfolder).string();
    }else{
        LOGD << "No build needed";
        return;
    }

    if (fs::exists(outputFolder) && !force) {
        LOGD << "Output folder already existing and no force parameter provided: no build needed";
        return;
    }

    const auto tempFolder = outputFolder + "-temp"; 

    LOGD << "Temp folder " << tempFolder;

    io::assureFolderExists(tempFolder);
    const auto hardlink = baseOutputPath.string() + "_link" + fs::path(e.path).extension().string();
    io::assureIsRemoved(hardlink);

    auto relativePath =
        (fs::path(db->getOpenFile()).parent_path().parent_path() / e.path)
            .string();

    LOGD << "Relative path " << relativePath;

    try {
        io::hardlink(relativePath, hardlink);
        LOGD << "Linked " << relativePath << " --> " << hardlink;
        relativePath = hardlink;
    } catch(const FSException &e){
        LOGD << e.what();
        // Will build directly from path
    }

    // We could vectorize this logic, but it's an overkill by now
    try {
        bool built = false;

        if (e.type == PointCloud) {
            const std::vector vec = {relativePath};
            buildEpt(vec, tempFolder);
            built = true;
        }else if (e.type == GeoRaster){
            buildCog(relativePath, (fs::path(tempFolder) / "cog.tif").string());
            built = true;
        }

        if (built){
            LOGD << "Build complete, moving temp folder to " << outputFolder;
            io::assureFolderExists(fs::path(outputFolder).parent_path());
            std::filesystem::rename(tempFolder, outputFolder);

            output << outputFolder << std::endl;
        }

        io::assureIsRemoved(tempFolder);
        io::assureIsRemoved(hardlink);
    
    // Catch block is now detailed because we want to keep track of the exception
    // catches pdal, gdal, filesystem_error, etc...
    } catch(const std::exception& ex){

        LOGD << ex.what();

        io::assureIsRemoved(tempFolder);
        io::assureIsRemoved(hardlink);

        throw;

    // This way we are sure that all the exceptions are caught
    }
    catch(...){

        io::assureIsRemoved(tempFolder);
        io::assureIsRemoved(hardlink);

        throw;
    }
}

void buildAll(Database* db, const std::string& outputPath,
               std::ostream& output, bool force) {

    LOGD << "In buildAll('" << outputPath << "')";

    // List all buildable files in DB
    auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE type = ? OR type = ?");
    q->bind(1, PointCloud);
    q->bind(2, GeoRaster);

    while (q->fetch()) {
        Entry e(q->getText(0), q->getText(1), q->getInt(2), q->getText(3),
                q->getInt64(4), q->getInt64(5), q->getInt(6));

        // Call build on each of them
        buildInternal(db, e, outputPath, output, force);
    }
}

void build(Database* db, const std::string& path, const std::string& outputPath,
           std::ostream& output, bool force) {

    LOGD << "In build('" << path << "','" << outputPath << "')";

    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists) throw InvalidArgsException(path + " is not a valid path in the database.");

    buildInternal(db, e, outputPath, output, force);
}

}  // namespace ddb
