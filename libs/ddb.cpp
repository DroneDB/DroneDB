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
    fs::path dirPath = fs::absolute(directory);
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
        throw FSException("Not a valid DroneDB directory, .ddb does not exist. Did you forget to run ./ddb init?");
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

    std::vector<fs::path> pathList;

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == ".ddb") continue;

        if (fs::is_directory(p)) {
            for(auto i = fs::recursive_directory_iterator(p);
                    i != fs::recursive_directory_iterator();
                    ++i ) {
                // Skip .ddb
                if(i->path().filename() == ".ddb") i.disable_recursion_pending();

                pathList.push_back(i->path());
            }

            pathList.push_back(p);
        } else if (fs::exists(p)) {
            // File
            pathList.push_back(p);
        } else {
            throw FSException("File does not exist: " + p.string());
        }
    }

    // TODO: there could be speed optimizations here?
    auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");
    auto insertQ = db->query("INSERT INTO entries (path, hash, type, meta, mtime, size) "
                             "VALUES (?, ?, ?, ?, ?, ?)");
    // TODO: execute in a transaction (speed up? bulk?)

    for (auto &p : pathList) {
        fs::path relPath = fs::relative(p, directory);
        q->bind(1, relPath.generic_string());

        bool update = false;
        bool add = false;
        bool folder = fs::is_directory(p);

        Entry e;

        if (q->fetch()) {
            // Entry exist, update if necessary

            // - Never need to update a folder, the meta doesn't change
            // If file, check modified date and hash

            if (!folder) {
                long long oldMtime = q->getInt64(0);
                e.mtime = utils::getModifiedTime(p);

                if (e.mtime != oldMtime) {
                    LOGD << p.string() << " modified time ( " << oldMtime << " ) differs from file value: " << e.mtime;
                    std::string oldHash = q->getText(1);
                    e.hash = Hash::ingest(p);

                    if (oldHash != e.hash) {
                        LOGD << p.string() << " hash differs (old: " << oldHash << " | new: " << e.hash << ")";
                        update = true;
                    }
                }
            }
        } else {
            // Brand new, add
            add = true;
        }

        if (add || update) {
            parseEntry(p, directory, e);

            insertQ->bind(1, e.path);
            insertQ->bind(2, e.hash);
            insertQ->bind(3, e.type);
            insertQ->bind(4, e.meta);
            insertQ->bind(5, static_cast<long long>(e.mtime));
            insertQ->bind(6, static_cast<long long>(e.size));

            if (add) {
                std::cout << "A\t" << e.path << std::endl;
            } else {
                std::cout << "U\t" << e.path << std::endl;
            }

            insertQ->fetch();
            insertQ->reset();
        }

        q->reset();
    }

}

void updateIndex(const std::string &directory) {
    LOGV << directory << " TODO!";
}

}

