/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dbops.h"

#include <ddb.h>

#include "entry_types.h"
#include "exif.h"
#include "hash.h"
#include "exceptions.h"
#include "utils.h"
#include "net.h"
#include "logger.h"
#include "mio.h"
#include "version.h"
#include "userprofile.h"
#include <cstdlib>

namespace ddb {

#define UPDATE_QUERY "UPDATE entries SET hash=?, type=?, meta=?, mtime=?, size=?, depth=?, point_geom=GeomFromText(?, 4326), polygon_geom=GeomFromText(?, 4326) WHERE path=?"

std::unique_ptr<Database> open(const std::string &directory, bool traverseUp = false) {
    const fs::path dirPath = fs::absolute(directory);
    const fs::path ddbDirPath = dirPath / DDB_FOLDER;
    const fs::path dbasePath = ddbDirPath / "dbase.sqlite";

    if (exists(dbasePath)) {
        LOGD << dbasePath.string() + " exists";

        std::unique_ptr<Database> db = std::make_unique<Database>();
        db->open(dbasePath.string());
        if (!db->tableExists("entries")) {
            throw DBException("Table 'entries' not found (not a valid database: " + dbasePath.string() + ")");
        }
        return db;
    }

    if (traverseUp && dirPath.parent_path() != dirPath) {
        return open(dirPath.parent_path().string(), true);
    }

    throw FSException("Not a valid DroneDB directory, .ddb does not exist. Did you run ddb init?");
}

fs::path rootDirectory(Database *db) {
    assert(db != nullptr);
    return fs::path(db->getOpenFile()).parent_path().parent_path();
}

// Computes a list of paths inside rootDirectory
// all paths must be subfolders/files within rootDirectory
// or an exception is thrown
// If includeDirs is true and the list includes paths to directories that are in paths
// eg. if path/to/file is in paths, both "path/" and "path/to"
// are includes in the result.
// ".ddb" files/dirs are always ignored and skipped.
// If a directory is in the input paths, they are included regardless of includeDirs
std::vector<fs::path> getIndexPathList(const fs::path& rootDirectory, const std::vector<std::string> &paths, bool includeDirs) {
    std::vector<fs::path> result;
    std::unordered_map<std::string, bool> directories;

    for (const std::string &p : paths){
        if (p.empty()) throw FSException("Some paths are empty");
    }

    if (!io::Path(rootDirectory).hasChildren(paths)) {
        throw FSException("Some paths are not contained within: " + rootDirectory.string() + ". Did you run ddb init?");
    }

    io::Path rootDir = rootDirectory;

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == DDB_FOLDER) continue;

        if (fs::is_directory(p)) {
            try{
                for(auto i = fs::recursive_directory_iterator(p);
                        i != fs::recursive_directory_iterator();
                        ++i ) {

                    fs::path rp = i->path();

                    // Skip .ddb
                    if (rp.filename() == DDB_FOLDER)
                        i.disable_recursion_pending();

                    if (fs::is_directory(rp) && includeDirs) {
                        directories[rp.string()] = true;
                    } else {
                        result.push_back(rp);
                    }

                    if (includeDirs) {
                        while(rp.has_parent_path() &&
                              rootDir.isParentOf(rp.parent_path()) &&
                              rp.string() != rp.parent_path().string()) {
                            rp = rp.parent_path();
                            directories[rp.string()] = true;
                        }
                    }
                }
            }catch(const fs::filesystem_error &e){
                throw FSException(e.what());
            }

            directories[p.string()] = true;
        } else if (fs::exists(p)) {
            // File
            result.push_back(p);

            if (includeDirs) {
                while(p.has_parent_path() &&
                      rootDir.isParentOf(p.parent_path()) &&
                      p.string() != p.parent_path().string()) {
                    p = p.parent_path();
                    directories[p.string()] = true;
                }
            }
        } else {
            throw FSException("Path does not exist: " + p.string());
        }
    }

    for (auto [fst, snd] : directories) {
        result.emplace_back(fst);
    }

    return result;
}

std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth) {
    std::vector<fs::path> result;
    std::unordered_map<std::string, bool> directories;

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == DDB_FOLDER) continue;
        
        try {
            if (fs::is_directory(p)) {
                for(auto i = fs::recursive_directory_iterator(p);
                        i != fs::recursive_directory_iterator();
                        ++i ) {

                    fs::path rp = i->path();

                    // Ignore system files on Windows
                    #ifdef WIN32
                    const DWORD attrs = GetFileAttributesW(rp.wstring().c_str());
                    if (attrs & FILE_ATTRIBUTE_HIDDEN || attrs & FILE_ATTRIBUTE_SYSTEM) {
                        i.disable_recursion_pending();
                        continue;
                    }
                    #endif

                    // Skip .ddb recursion
                    if (rp.filename() == DDB_FOLDER)
                        i.disable_recursion_pending();

                    // Max depth
                    if (maxDepth > 0 && i.depth() >= (maxDepth - 1)) i.disable_recursion_pending();

                    if (fs::is_directory(rp)) {
                        if (includeDirs) result.push_back(rp);
                    }else{
                        result.push_back(rp);
                    }
                }
            } else if (fs::exists(p)) {
                // File
                result.push_back(p);
            } else {
                throw FSException("Path does not exist: " + p.string());
            }
        } catch (const fs::filesystem_error &e) {
            throw FSException(e.what());
        }
    }

    return result;
}


std::vector<std::string> expandPathList(const std::vector<std::string> &paths, bool recursive, int maxRecursionDepth) {

	if (!recursive) return paths;
    	
    std::vector<std::string> result;
    auto pl = getPathList(paths, true, maxRecursionDepth);
    for (auto& p : pl) {
	    result.push_back(p.string());
    }
    return result;
}


bool checkUpdate(Entry &e, const fs::path &p, long long dbMtime, const std::string &dbHash) {
    const bool folder = fs::is_directory(p);

    // Did it change?
    e.mtime = io::Path(p).getModifiedTime();

    if (e.mtime != dbMtime) {
        LOGD << p.string() << " modified time ( " << dbMtime << " ) differs from file value: " << e.mtime;

        if (folder) {
            // Don't check hashes for folders
            return true;
        } else {
            e.hash = Hash::fileSHA256(p.string());

            if (dbHash != e.hash) {
                LOGD << p.string() << " hash differs (old: " << dbHash << " | new: " << e.hash << ")";
                return true;
            }
        }
    }

    return false;
}

void doUpdate(Statement *updateQ, const Entry &e) {
    // Fields
    updateQ->bind(1, e.hash);
    updateQ->bind(2, e.type);
    updateQ->bind(3, e.meta.dump());
    updateQ->bind(4, static_cast<long long>(e.mtime));
    updateQ->bind(5, static_cast<long long>(e.size));
    updateQ->bind(6, e.depth);
    updateQ->bind(7, e.point_geom.toWkt());
    updateQ->bind(8, e.polygon_geom.toWkt());

    // Where
    updateQ->bind(9, e.path);

    updateQ->execute();
}


void addToIndex(Database *db, const std::vector<std::string> &paths, AddCallback callback) {
    if (paths.empty()) return; // Nothing to do
    const fs::path directory = rootDirectory(db);
    auto pathList = getIndexPathList(directory, paths, true);

    auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");
    auto insertQ = db->query("INSERT INTO entries (path, hash, type, meta, mtime, size, depth, point_geom, polygon_geom) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?, GeomFromText(?, 4326), GeomFromText(?, 4326))");
    const auto updateQ = db->query(UPDATE_QUERY);
    db->exec("BEGIN EXCLUSIVE TRANSACTION");

    for (auto &p : pathList) {
        io::Path relPath = io::Path(p).relativeTo(directory);
        q->bind(1, relPath.generic());

        bool update = false;
        bool add = false;
        Entry e;

        if (q->fetch()) {
            // Entry exist, update if necessary
            update = checkUpdate(e, p, q->getInt64(0), q->getText(1));
        } else {
            // Brand new, add
            add = true;
        }

        if (add || update) {
            parseEntry(p, directory, e, true);

            if (add) {

                insertQ->bind(1, e.path);
                insertQ->bind(2, e.hash);
                insertQ->bind(3, e.type);
                insertQ->bind(4, e.meta.dump());
                insertQ->bind(5, static_cast<long long>(e.mtime));
                insertQ->bind(6, static_cast<long long>(e.size));
                insertQ->bind(7, e.depth);
                insertQ->bind(8, e.point_geom.toWkt());
                insertQ->bind(9, e.polygon_geom.toWkt());

                insertQ->execute();
            } else {
                doUpdate(updateQ.get(), e);
            }

            if (callback != nullptr) if (!callback(e, !add)) return; // cancel
        }

        q->reset();
    }

    db->exec("COMMIT");
}

void removeFromIndex(Database *db, const std::vector<std::string> &paths) {

    if (paths.empty())
    {
    	// Nothing to do
        LOGD << "No paths provided";
	    return;
    }
	
    const fs::path directory = rootDirectory(db);

    auto pathList = std::vector<fs::path>(paths.begin(), paths.end()); 

    for (auto &p : pathList) {

        LOGD << "Deleting path: " << p;

        auto relPath = io::Path(p).relativeTo(directory);

        LOGD << "Rel path: " << relPath.generic();

        auto entryMatches = getMatchingEntries(db, relPath.generic());
    	
        int tot = 0;

    	for (auto &e : entryMatches)
    	{
            auto cnt = deleteFromIndex(db, e.path);

    		if (e.type == Directory)    		
                cnt += deleteFromIndex(db, e.path, true);

            // if (!cnt)            
            //     std::cout << "No matching entries" << std::endl;

            tot += cnt;
            
    	}

        if (!tot)
            throw FSException("No matching entries");
    }
}


std::string sanitize_query_param(const std::string& str)
{
	std::string res(str);

	// TAKES INTO ACCOUNT PATHS THAT CONTAINS EVERY SORT OF STUFF
    utils::string_replace(res, "/", "//");
    utils::string_replace(res, "%", "/%");
    utils::string_replace(res, "_", "/_");
    //utils::string_replace(res, "?", "/?");
    //utils::string_replace(res, "*", "/*");
    utils::string_replace(res, "*", "%");

    return res;

}
	
int deleteFromIndex(Database* db, const std::string &query, bool isFolder)
{

    int count = 0;

    LOGD << "Query: " << query;

    auto str = sanitize_query_param(query);

    LOGD << "Sanitized: " << str;

	if (isFolder) {
		str += "//%";

        LOGD << "Folder: " << str;
    }
		
    auto q = db->query("SELECT path, type FROM entries WHERE path LIKE ? ESCAPE '/'");

    q->bind(1, str);

    bool res = false;

	while(q->fetch())
	{
        res = true;
                
        std::cout << "D\t" << q->getText(0) << std::endl;
        count++;        
	}
	
    q->reset();

    if (res) {
        q = db->query("DELETE FROM entries WHERE path LIKE ? ESCAPE '/'");

        q->bind(1, str);
        q->execute();

        q->reset();
    } 

    return count;
}

	
std::vector<Entry> getMatchingEntries(Database* db, const fs::path& path, int maxRecursionDepth, bool isFolder) {

	// 0 is ALL_DEPTHS
	if (maxRecursionDepth < 0)
        throw FSException("Max recursion depth cannot be negative");
	
	const auto query = path.string();

    LOGD << "Query: " << query;

    auto sanitized = sanitize_query_param(query);

    if (sanitized.length() == 0)
        sanitized = "%";
	
	LOGD << "Sanitized: " << sanitized;

    if (isFolder) {
        sanitized += "//%";

        LOGD << "Folder: " << sanitized;

    }

    std::string sql = "SELECT path, hash, type, meta, mtime, size, depth, AsGeoJSON(point_geom), AsGeoJSON(polygon_geom) FROM entries WHERE path LIKE ? ESCAPE '/'";

    if (maxRecursionDepth > 0)
        sql += " AND depth <= " + std::to_string(maxRecursionDepth - 1);
	
    auto q = db->query(sql);
    	
    std::vector<Entry> entries;

    q->bind(1, sanitized);

	while(q->fetch())
	{
		Entry e(*q);		
        entries.push_back(e);		
    }

    q->reset();

    return entries;

}

void syncIndex(Database *db) {
    const fs::path directory = rootDirectory(db);

    auto q = db->query("SELECT path,mtime,hash FROM entries");
    auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
    const auto updateQ = db->query(UPDATE_QUERY);

    db->exec("BEGIN EXCLUSIVE TRANSACTION");

    while(q->fetch()) {
        io::Path relPath = fs::path(q->getText(0));
        fs::path p = directory / relPath.get(); // TODO: does this work on Windows?
        Entry e;

        if (fs::exists(p)) {
            if (checkUpdate(e, p, q->getInt64(1), q->getText(2))) {
                parseEntry(p, directory, e, true);
                doUpdate(updateQ.get(), e);
                std::cout << "U\t" << e.path << std::endl;
            }
        } else {
            // Removed
            deleteQ->bind(1, relPath.generic());
            deleteQ->execute();
            std::cout << "D\t" << relPath.generic() << std::endl;
        }
    }

    db->exec("COMMIT");
}

std::string initIndex(const std::string &directory, bool fromScratch){
    const fs::path dirPath = directory;
    if (!exists(dirPath)) throw FSException("Invalid directory: " + dirPath.string() + " (does not exist)");

    auto ddbDirPath = dirPath / DDB_FOLDER;
    if (std::string(directory) == ".")
        ddbDirPath = DDB_FOLDER;  // Nicer to the eye
    const auto dbasePath = ddbDirPath / "dbase.sqlite";

    LOGD << "Checking if .ddb directory exists...";
    if (exists(ddbDirPath)) {
        throw FSException("Cannot initialize database: " + ddbDirPath.string() + " already exists");
    }

    if (create_directory(ddbDirPath)) {
        LOGD << ddbDirPath.string() + " created";
    }
    else {
        throw FSException("Cannot create directory: " + ddbDirPath.string() + ". Check that you have the proper permissions?");
    }

    LOGD << "Checking if database exists...";
    if (exists(dbasePath))
    {
        throw FSException(ddbDirPath.string() + " already exists");
    }

    if (!fromScratch){
        // "Fast" init by copying the pre-built empty database index
        // this prevents the slow table generation process
        const fs::path emptyDbPath = UserProfile::get()->getTemplatesDir() / ("empty-dbase-" APP_REVISION ".sqlite");

        // Need to create?
        if (!fs::exists(emptyDbPath)){
            LOGD << "Creating " << emptyDbPath.string();

            // Create database
            auto db = std::make_unique<Database>();
            db->open(emptyDbPath.string());
            db->createTables();
            db->close();
        }

        if (fs::exists(emptyDbPath)){
            // Copy
            try{
                fs::copy(emptyDbPath, dbasePath);
            }catch(fs::filesystem_error &e){
                throw FSException(e.what());
            }

            LOGD << "Copied " << emptyDbPath.string() << " to " << dbasePath.string();
        }else{
            // For some reason it's missing, generate from scratch
            LOGD << "Cannot find empty-dbase.sqlite in data path, strange! Building from scratch instead";
            fromScratch = true;
        }
    }

    if (fromScratch){
        LOGD << "Creating " << dbasePath.string();

        // Create database
        auto db = std::make_unique<Database>();
        db->open(dbasePath.string());
        db->createTables();
        db->close();
    }

    return ddbDirPath.string();
}

}

