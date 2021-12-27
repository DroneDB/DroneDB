#include "metamanager.h"
#include "database.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"
#include "json.h"

namespace ddb {

std::string MetaManager::entryPath(const std::string &path, const std::string &cwd) const{
    if (path.empty()) return "";

    auto p = io::Path(path);
    if (!cwd.empty() && p.isRelative()) p = io::Path(fs::path(cwd) / fs::path(path));

    std::string relPath = p.relativeTo(db->rootDirectory()).generic();

    const auto q = db->query("SELECT 1 FROM entries WHERE path = ?");
    q->bind(1, relPath);
    if (!q->fetch()) throw InvalidArgsException("Path " + relPath + " not available in index");

    return relPath;
}

std::string MetaManager::getKey(const std::string &key, bool isList) const{

    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    if (!utils::isLowerCase(key)) throw InvalidArgsException("Metadata key must be lowercase");

    if (isList && key[key.length() - 1] != 's') 
        throw InvalidArgsException("Invalid metadata key (must be plural, for example: " + key + "s)");

    if (!isList && key[key.length() - 1] == 's') 
        throw InvalidArgsException("Invalid metadata key (must be singular, for example: " + key.substr(0, key.length() - 1) + ")");

    return key;
}

json MetaManager::getMetaJson(Statement *q) const{

    if (!q->fetch()) 
        throw DBException("Cannot fetch meta with query: " + q->getQuery());
    
    return metaStmtToJson(q);

}

json MetaManager::metaStmtToJson(Statement *q) const{
    json j;
    j["id"] = q->getText(0);

    try {
        j["data"] = json::parse(q->getText(1));
    } catch (const json::parse_error &e) {
        LOGD << "Warning, corrupted metadata: " << q->getText(1) << " ("
             << e.what() << ")";
        j["data"] = "";
    }

    j["mtime"] = q->getInt64(2);
    return j;
}

json MetaManager::getMetaJson(const std::string &q) const{
    return getMetaJson(db->query(q).get());
}

std::string MetaManager::validateData(const std::string &data) const{
    try {
        return json::parse(data).dump();
    } catch (const json::parse_error &e) {
        try{
            // Try with quotes since this is probably a string
            return json::parse("\"" + data + "\"").dump();
        } catch (const json::parse_error &e) {
            throw JSONException("Invalid JSON (" + std::string(e.what()) + "): " + data);
        }
    }
}

bool MetaManager::isList(const std::string &key) const{
    return key.length() > 0 && key[key.length() - 1] == 's';
}

json MetaManager::add(const std::string &key, const std::string &data, const std::string &path, const std::string &cwd){
    const auto ePath = entryPath(path, cwd);
    const auto eKey = getKey(key, true);
    const auto eData = validateData(data);
    const long long eMtime = utils::currentUnixTimestamp();

    const auto q = db->query("INSERT INTO entries_meta (path, key, data, mtime) VALUES (?, ?, ?, ?)");
    q->bind(1, ePath);
    q->bind(2, eKey);
    q->bind(3, eData);
    q->bind(4, eMtime);
    q->execute();

    auto result = getMetaJson("SELECT id, data, mtime FROM entries_meta WHERE rowid = last_insert_rowid()");

    return result;
}

json MetaManager::set(const std::string &key, const std::string &data, const std::string &path, const std::string &cwd){
    const auto ePath = entryPath(path, cwd);
    const auto eKey = getKey(key, false);
    const auto eData = validateData(data);
    const long long eMtime = utils::currentUnixTimestamp();

    // Delete previous meta first (we need to generate a new ID)
    const auto q = db->query("DELETE FROM entries_meta WHERE path = ? and key = ?");
    q->bind(1, ePath);
    q->bind(2, eKey);
    q->execute();

    // Insert
    const auto iq = db->query("INSERT INTO entries_meta (path, key, data, mtime) VALUES (?, ?, ?, ?)");
    iq->bind(1, ePath);
    iq->bind(2, eKey);
    iq->bind(3, eData);
    iq->bind(4, eMtime);
    iq->execute();
    return getMetaJson("SELECT id, data, mtime FROM entries_meta WHERE rowid = last_insert_rowid()");
}

json MetaManager::remove(const std::string &id){
    if (id.empty()) throw InvalidArgsException("Invalid metadata id empty");
    const auto q = db->query("DELETE FROM entries_meta WHERE id = ?");
    q->bind(1, id);
    q->execute();
    json j;
    j["removed"] = db->changes();
    return j;
}

json MetaManager::get(const std::string &key, const std::string &path, const std::string &cwd){
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    const auto ePath = entryPath(path, cwd);
    json result = json::array();

    const auto q = db->query("SELECT id, data, mtime FROM entries_meta WHERE key = ? AND path = ?");
    q->bind(1, key);
    q->bind(2, ePath);

    while (q->fetch()){
        result.push_back(metaStmtToJson(q.get()));
    }

    if (result.empty()) 
        throw InvalidArgsException("No metadata found for key " + key + (path.empty() ? "" : " and path " + path));

    return isList(key) ? result : result[0].empty() ? json::object() : result[0];

}

json MetaManager::unset(const std::string &key, const std::string &path, const std::string &cwd){
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    const auto ePath = entryPath(path, cwd);
    const auto q = db->query("DELETE FROM entries_meta WHERE key = ? AND path = ?");
    q->bind(1, key);
    q->bind(2, ePath);
    q->execute();

    json j;
    j["removed"] = db->changes();
    return j;
}

json MetaManager::list(const std::string &path, const std::string &cwd) const{
    const auto ePath = entryPath(path, cwd);

    const auto sql = ePath.empty()
                                ? "SELECT key, path, COUNT(id) as 'count' FROM entries_meta GROUP BY path, key"
                                : "SELECT key, path, COUNT(id) as 'count' FROM entries_meta WHERE path = ? GROUP BY path, key";

    const auto q = db->query(sql);
    if (!ePath.empty()) q->bind(1, ePath);

    json result = json::array();
    while(q->fetch()){
        result.push_back(json::object({{"key", q->getText(0)},
                                       {"path", q->getText(1)},
                                       {"count", q->getInt(2)}}));
    }
    return result;
}

json MetaManager::dump(const json &ids){
    if (!ids.is_array()) throw InvalidArgsException("ids must be an array");

    json result = json::array();
    std::string sql = "SELECT id,path,key,data,mtime FROM entries_meta";
    if (ids.size() > 0){
        sql += " WHERE id IN (";
        for (unsigned long i = 0; i < ids.size(); i++){
            sql += "?";
            if (i < ids.size() - 1) sql += ",";
        }
        sql += ")";
    }
    sql += " ORDER by id ASC";

    const auto q = db->query(sql);

    if (ids.size() > 0){
        unsigned long i = 1;
        for (auto &id : ids){
            q->bind(i, id.is_string() ? id.get<std::string>() : "");
            i++;
        }
    }

    while(q->fetch()){
        result.push_back(json::object({{"id", q->getText(0)},
                                       {"path", q->getText(1)},
                                       {"key", q->getText(2)},
                                       {"data", q->getText(3)},
                                       {"mtime", q->getInt64(4)},
                                      }));
    }
    return result;
}

json MetaManager::restore(const json &metaDump){
    if (!metaDump.is_array()) throw InvalidArgsException("metaDump must be an array");

    db->exec("BEGIN EXCLUSIVE TRANSACTION");

    const auto q = db->query("INSERT OR REPLACE INTO entries_meta(id, path, key, data, mtime) VALUES (?, ?, ?, ?, ?)");

    int i = 0;
    for (auto &meta : metaDump){
        // Quick validation
        if (!meta.contains("id") ||
                !meta.contains("path") ||
                !meta.contains("key") ||
                !meta.contains("data") ||
                !meta.contains("mtime")){
            throw InvalidArgsException("Invalid meta: " + meta.dump());
        }

        q->bind(1, meta["id"].get<std::string>());
        q->bind(2, meta["path"].get<std::string>());
        q->bind(3, meta["key"].get<std::string>());
        q->bind(4, meta["data"].get<std::string>());
        q->bind(5, meta["mtime"].get<long long>());
        q->execute();
        i++;
    }

    db->exec("COMMIT");
    json j;
    j["restored"] = i;
    return j;
}

DDB_DLL json MetaManager::bulkRemove(const std::vector<std::string> &ids){
    const auto q = db->query("DELETE FROM entries_meta WHERE id = ?");

    db->exec("BEGIN EXCLUSIVE TRANSACTION");

    int i = 0;
    for (auto &id : ids){
        q->bind(1, id);
        q->execute();
        i++;
    }

    db->exec("COMMIT");

    json j;
    j["removed"] = i;
    return j;
}
	

}
