/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include "ddb.h"
#include "../classes/exif.h"
#include "../classes/hash.h"
#include "../classes/database.h"
#include "../classes/exceptions.h"
#include "../utils.h"

namespace ddb {

std::string create(const std::string &directory) {
    fs::path dirPath = directory;
    if (!fs::exists(dirPath)) throw FSException("Invalid directory: " + directory  + " (does not exist)");

    fs::path ddbDirPath = dirPath / ".ddb";
    if (directory == ".") ddbDirPath = ".ddb"; // Nicer to the eye
    fs::path dbasePath = ddbDirPath / "dbase";

    try {
        LOGD << "Checking if .ddb directory exists...";
        if (fs::exists(ddbDirPath)) {
            throw FSException("Cannot initialize database: " + ddbDirPath.string() + " already exists");
        } else {
            if (fs::create_directory(ddbDirPath)) {
                LOGD << ddbDirPath.string() + " created";
            } else {
                throw FSException("Cannot create directory: " + ddbDirPath.string() + ". Check that you have the proper permissions?");
            }
        }

        LOGD << "Checking if dbase exists...";
        if (fs::exists(dbasePath)) {
            throw FSException(ddbDirPath.string() + " already exists");
        } else {
            LOGD << "Creating " << dbasePath.string();

            // Create database
            std::unique_ptr<Database> db = std::make_unique<Database>();
            db->open(dbasePath);
            db->createTables();
            db->close();

            return ddbDirPath;
        }
    } catch (const AppException &exception) {
        LOGV << "Exception caught, cleaning up...";

        throw exception;
    }
}

std::unique_ptr<Database> open(const std::string &directory, bool traverseUp = false) {
    fs::path dirPath = directory;
    fs::path ddbDirPath = dirPath / ".ddb";
    fs::path dbasePath = ddbDirPath / "dbase";

    if (fs::exists(dbasePath)) {
        LOGD << dbasePath.string() + " exists";

        std::unique_ptr<Database> db = std::make_unique<Database>();
        db->open(dbasePath);
        if (!db->tableExists("entries")) {
            throw DBException("Table 'entries' not found (not a valid database: " + dbasePath.string() + ")");
        }
        return db;
    } else if (traverseUp && dirPath.parent_path() != dirPath) {
        return open(dirPath.parent_path(), true);
    } else {
        throw FSException("Not in a valid DroneDB directory, " + ddbDirPath.string() + " does not exist");
    }
}

fs::path rootDirectory(Database *db) {
    assert(db != nullptr);
    return fs::path(db->getOpenFile()).parent_path().parent_path();
}

void addToIndex(Database *db, const std::vector<std::string> &paths) {
    // Validate paths
    fs::path directory = rootDirectory(db);

    if (!utils::pathsAreChildren(directory, paths)) {
        throw FSException("Some paths cannot be added to the index because we couldn't find a parent .ddb folder.");
    }

    std::vector<fs::path> fileList;

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == ".ddb") continue;

        if (fs::is_directory(p)) {
            for(auto i = fs::recursive_directory_iterator(p);
                    i != fs::recursive_directory_iterator();
                    ++i ) {
                fs::path filename = i->path().filename();

                // Skip .ddb
                if(filename == ".ddb") i.disable_recursion_pending();

                // Skip directory entries (but recurse)
                else if (fs::is_directory(i->path())) continue;

                // Process files
                else {
                    fileList.push_back(i->path());
                }
            }
        } else if (fs::exists(p)) {
            // File
            fileList.push_back(p);
        } else {
            throw FSException("File does not exist: " + p.string());
        }
    }

    // TODO: there could be speed optimizations here?
    auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");

    for (auto &filePath : fileList) {
        fs::path relPath = fs::relative(filePath, directory);
        q->bind(1, relPath.generic_string());

        bool update = false;
        bool add = false;

        std::string hash;
        time_t mtime = -1;

        if (q->fetch()) {
            // Entry exist, update if necessary
            // (check modified date and hash)
            long int oldMtime = q->getInt(1);
            mtime = utils::getModifiedTime(filePath);

            if (mtime != oldMtime) {
                LOGD << filePath << " modified time differs from " << mtime;
                std::string oldHash = q->getText(2);
                hash = Hash::ingest(filePath);

                if (oldHash != hash) {
                    LOGD << filePath << " hash differs (old: " << oldHash << " | new: " << hash << ")";
                    update = true;
                }
            }
        } else {
            // Brand new, add
            add = true;
        }

        if (add || update) {
            // Parse file
            if (mtime == -1) mtime = utils::getModifiedTime(filePath);
            if (hash == "") hash = Hash::ingest(filePath);
            off_t size = utils::getSize(filePath);


            // Images
            if (utils::checkExtension(filePath.extension(), {"jpg", "jpeg", "tif", "tiff"})) {
                auto image = Exiv2::ImageFactory::open(filePath);
                if (!image.get()) throw new IndexException("Cannot open " + filePath);
                image->readMetadata();

                auto exifData = image->exifData();

                if (!exifData.empty()) {

                    exif::Parser p(exifData);

                    auto imageSize = p.extractImageSize();
                    LOGD << "Filename: " << filePath;
                    LOGD << "Image Size: " << imageSize.width << "x" << imageSize.height;
                    LOGD << "Make: " << p.extractMake();
                    LOGD << "Model: " << p.extractModel();
                    LOGD << "Sensor width: " << p.extractSensorWidth();
                    LOGD << "Sensor: " << p.extractSensor();
                    LOGD << "Focal35: " << p.computeFocal().f35;
                    LOGD << "FocalRatio: " << p.computeFocal().ratio;
                    LOGD << "Latitude: " << std::setprecision(14) << p.extractGeo().latitude;
                    LOGD << "Longitude: " << std::setprecision(14) << p.extractGeo().longitude;
                    LOGD << "Altitude: " << std::setprecision(14) << p.extractGeo().altitude;
                    LOGD << "Capture Time: " << p.extractCaptureTime();
                    LOGD << "Orientation: " << p.extractOrientation();

                    Exiv2::ExifData::const_iterator end = exifData.end();
                    for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
                        const char* tn = i->typeName();
                        std::cout
                                << i->key() << " "

                                << i->value()
                                << " | " << tn
                                << "\n";

                        if (i->key() == "Exif.GPSInfo.GPSLatitude") {
                            //                            std::cout << "===========" << std::endl;
                            //                            std::cout << std::setw(44) << std::setfill(' ') << std::left
                            //                                      << i->key() << " "
                            //                                      << "0x" << std::setw(4) << std::setfill('0') << std::right
                            //                                      << std::hex << i->tag() << " "
                            //                                      << std::setw(9) << std::setfill(' ') << std::left
                            //                                      << (tn ? tn : "Unknown") << " "
                            //                                      << std::dec << std::setw(3)
                            //                                      << std::setfill(' ') << std::right
                            //                                      << i->count() << "  "
                            //                                      << std::dec << i->value()
                            //                                      << "\n";
                        }


                    }

                } else {
                    LOGW << "No EXIF data found in " << filePath;
                }

//                auto xmpData = image->xmpData();
//                if (!xmpData.empty()) {
//                    for (Exiv2::XmpData::const_iterator md = xmpData.begin();
//                            md != xmpData.end(); ++md) {
//                        std::cout << std::setfill(' ') << std::left
//                                  << std::setw(44)
//                                  << md->key() << " "
//                                  << std::setw(9) << std::setfill(' ') << std::left
//                                  << md->typeName() << " "
//                                  << std::dec << std::setw(3)
//                                  << std::setfill(' ') << std::right
//                                  << md->count() << "  "
//                                  << std::dec << md->value()
//                                  << std::endl;
//                    }
//                }
            } else {
                // Other files
            }


            if (add) {
                LOGV << "Adding " << filePath.generic_string() << "\n";
            } else {
                LOGV << "Updating " << filePath.generic_string() << "\n";
            }
        }

        q->reset();
    }
}

void updateIndex(const std::string &directory) {


}

}

