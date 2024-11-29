/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "build.h"

#include "ddb.h"
#include "pointcloud.h"
#include "cog.h"
#include "3d.h"
#include "dbops.h"
#include "exceptions.h"
#include "mio.h"
#include "threadlock.h"
#include "vector.h"

namespace ddb {

bool isBuildableInternal(const Entry& e, std::string& subfolder) {

    if (e.type == EntryType::PointCloud) {
        // Special case: do not build if this entry is in a "ept-data" folder
        // as it indicates an EPT dataset file
        if (fs::path(e.path).parent_path().filename().string() == "ept-data") return false;

        subfolder = "ept";
        return true;
    }else if (e.type == EntryType::GeoRaster) {
        subfolder = "cog";
        return true;
    }else if (e.type == EntryType::Model) {
        subfolder = "nxs";
        return true;
    } else if (e.type == EntryType::Vector) {
        subfolder = "vec";
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
                           const std::string& outputPath,
                           bool force, BuildCallback callback) {
    std::string outPath = outputPath;
    if (outPath.empty()) outPath = db->buildDirectory().string();

    LOGD << "Building entry " << e.path << " type " << e.type;

    const auto baseOutputPath = fs::path(outPath) / e.hash;
    std::string outputFolder;
    std::string subfolder;

    if (isBuildableInternal(e, subfolder)) {
        outputFolder = (baseOutputPath / subfolder).string();
    }else{
        LOGD << "No build needed";
        return;
    }

    ThreadLock lock("build-" + (db->rootDirectory() / e.hash).string());

    if (fs::exists(outputFolder) && !force) {
        LOGD << "Output folder " << outputFolder << " already exists, skipping";
        return;
    }

    const auto tempFolder = outputFolder + "-temp-" + utils::generateRandomString(16);

    io::assureFolderExists(tempFolder);

    auto relativePath = (db->rootDirectory() / e.path).string();

    std::string pendFile = baseOutputPath.string() + ".pending";
    io::assureIsRemoved(pendFile);

    // We could vectorize this logic, but it's an overkill by now
    try {
        bool built = false;

        if (e.type == EntryType::PointCloud) {
            const std::vector vec = {relativePath};
            buildEpt(vec, tempFolder);
            built = true;
        } else if (e.type == EntryType::GeoRaster){
            buildCog(relativePath, (fs::path(tempFolder) / "cog.tif").string());
            built = true;
        } else if (e.type == EntryType::Model){
            buildNexus(relativePath, (fs::path(tempFolder) / "model.nxz").string());
            built = true;
        } else if (e.type == EntryType::Vector){
            buildVector(relativePath, (fs::path(tempFolder) / "vector.fgb").string());
            built = true;
        }

        if (built){
            LOGD << "Build complete, moving temp folder to " << outputFolder;
            if (fs::exists(outputFolder)) io::assureIsRemoved(outputFolder);

            io::assureFolderExists(fs::path(outputFolder).parent_path());
            io::rename(tempFolder, outputFolder);

            if (callback != nullptr) callback(outputFolder);
        }

        io::assureIsRemoved(tempFolder);
    } catch(const BuildDepMissingException &e){

        // Create pending file
        std::ofstream pf(pendFile);
        if (pf){
            pf << utils::currentUnixTimestamp() << std::endl;
            pf.close();
        }else{
            LOGD << "Error! Cannot create pending file " << baseOutputPath.string() << ".pending";
        }

        io::assureIsRemoved(tempFolder);

        throw e;
    } catch(const AppException &e){
        io::assureIsRemoved(tempFolder);

        throw e;
    } catch(...){
        // Since we use third party libraries, some exceptions might not
        // get caught otherwise
        io::assureIsRemoved(tempFolder);

        throw AppException("Unknown build error failure for " + e.path + " (" + baseOutputPath.string() + ")");
    }
}

void buildAll(Database* db, const std::string& outputPath, bool force, BuildCallback callback) {
    std::string outPath = outputPath;
    if (outPath.empty()) outPath = db->buildDirectory().string();

    LOGD << "In buildAll('" << outputPath << "')";

    // List all buildable files in DB
    auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE type = ? OR type = ? OR type = ?");
    q->bind(1, PointCloud);
    q->bind(2, GeoRaster);
    q->bind(3, Model);

    while (q->fetch()) {
        Entry e(q->getText(0), q->getText(1), q->getInt(2), q->getText(3),
                q->getInt64(4), q->getInt64(5), q->getInt(6));

        // Call build on each of them
        try{
            buildInternal(db, e, outPath, force, callback);
        }catch(const AppException &err){
            LOGD << "Cannot build " << e.path << ": " << err.what();
        }
    }
}

void build(Database* db, const std::string& path, const std::string& outputPath, bool force, BuildCallback callback) {

    LOGD << "In build('" << path << "','" << outputPath << "')";

    Entry e;

    const bool entryExists = getEntry(db, path, e);
    if (!entryExists) throw InvalidArgsException(path + " is not a valid path in the database.");

    buildInternal(db, e, outputPath, force, callback);
}

void buildPending(Database *db, const std::string &outputPath, bool force, BuildCallback callback){
    auto buildDir = db->buildDirectory();
    if (!fs::exists(buildDir)) return;

    std::string outPath = outputPath;
    if (outPath.empty()) outPath = buildDir.string();

    for (auto i = fs::recursive_directory_iterator(buildDir);
         i != fs::recursive_directory_iterator(); ++i) {
        if (i->path().extension() == ".pending"){
            auto hash = i->path().filename().replace_extension("").string();

            // Check if file still exists in our index
            auto q = db->query("SELECT path, hash, type, properties, mtime, size, depth FROM entries WHERE hash = ?");
            q->bind(1, hash);
            bool found = false;

            while (q->fetch()) {
                found = true;
                Entry e(q->getText(0), q->getText(1), q->getInt(2), q->getText(3),
                        q->getInt64(4), q->getInt64(5), q->getInt(6));

                io::assureIsRemoved(i->path());

                // Call build
                try{
                    buildInternal(db, e, outPath, force, callback);
                }catch(const AppException &err){
                    LOGD << "Cannot build " << e.path << ": " << err.what();
                }
            }

            if (!found){
                io::assureIsRemoved(i->path());
            }
        }
    }
}

bool isBuildPending(Database *db){
    auto buildDir = db->buildDirectory();
    if (!fs::exists(buildDir)) return false;

    for (auto i = fs::recursive_directory_iterator(buildDir);
         i != fs::recursive_directory_iterator(); ++i) {
        if (i->path().extension() == ".pending"){
            return true;
        }
    }

    return false;
}

}  // namespace ddb
