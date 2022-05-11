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

namespace ddb {

bool isBuildableInternal(const Entry& e, std::string& subfolder) {

    if (e.type == PointCloud) {
        // Special case: do not build if this entry is in a "ept-data" folder
        // as it indicates an EPT dataset file
        if (fs::path(e.path).parent_path().filename().string() == "ept-data") return false;

        subfolder = "ept";
        return true;
    }else if (e.type == GeoRaster){
        subfolder = "cog";
        return true;
    }else if (e.type == Model){
        subfolder = "nxs";
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

    {
        ThreadLock lock("build-" + (db->rootDirectory() / e.hash).string());

        if (fs::exists(outputFolder) && !force) {
            return;
        }

        const auto tempFolder = outputFolder + "-temp-" + utils::generateRandomString(16);

        io::assureFolderExists(tempFolder);

        auto relativePath =
            (fs::path(db->getOpenFile()).parent_path().parent_path() / e.path)
                .string();

        std::string pendFile = baseOutputPath.string() + ".pending";
        io::assureIsRemoved(pendFile);

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
            }else if (e.type == Model){
                buildNexus(relativePath, (fs::path(tempFolder) / "model.nxz").string());
                built = true;
            }

            if (built){
                LOGD << "Build complete, moving temp folder to " << outputFolder;
                if (fs::exists(outputFolder)) io::assureIsRemoved(outputFolder);

                io::assureFolderExists(fs::path(outputFolder).parent_path());
                io::rename(tempFolder, outputFolder);

                output << outputFolder << std::endl;
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
}

void buildAll(Database* db, const std::string& outputPath,
               std::ostream& output, bool force) {

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

void buildPending(Database *db, const std::string &outputPath, std::ostream &output, bool force){
    auto buildDir = db->buildDirectory();
    if (!fs::exists(buildDir)) return;

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
                buildInternal(db, e, outputPath, output, force);
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
