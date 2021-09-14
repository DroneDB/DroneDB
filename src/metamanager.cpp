#include "metamanager.h"
#include "database.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"
#include "json.h"

namespace ddb {

std::string MetaManager::entryPath(const std::string &path) const{
    if (path.empty()) return "";

    std::string relPath = io::Path(path).relativeTo(db->rootDirectory()).generic();

    auto q = db->query("SELECT 1 FROM entries WHERE path = ?");
    q->bind(1, relPath);
    if (!q->fetch()) throw InvalidArgsException("Path " + relPath + " not available in index");

    return relPath;
}

std::string MetaManager::getKey(const std::string &key, bool isList) const{
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    if (!utils::isLowerCase(key)) throw InvalidArgsException("Metadata key must be lowercase");

    if (isList && key[key.length() - 1] != 's') throw InvalidArgsException("Invalid metadata key (must be plural, for example: " + key + "s)");
    else if (!isList && key[key.length() - 1] == 's') throw InvalidArgsException("Invalid metadata key (must be singular, for example: " + key.substr(0, key.length() - 1) + ")");
    else return key;
}

json MetaManager::getMetaJson(Statement *q) const{
    if (q->fetch()){
        return metaStmtToJson(q);
    }else throw DBException("Cannot fetch meta with query: " + q->getQuery());
}

json MetaManager::metaStmtToJson(Statement *q) const{
    json j;
    j["id"] = q->getText(0);

    try{
        j["data"] = json::parse(q->getText(1));
    }catch (const json::parse_error &e) {
        LOGD << "Warning, corrupted metadata: " << q->getText(1);
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

json MetaManager::add(const std::string &key, const std::string &data, const std::string &path){
    std::string ePath = entryPath(path);
    std::string eKey = getKey(key, true);
    std::string eData = validateData(data);
    long long eMtime = utils::currentUnixTimestamp();

    auto q = db->query("INSERT INTO entries_meta (path, key, data, mtime) VALUES (?, ?, ?, ?)");
    q->bind(1, ePath);
    q->bind(2, eKey);
    q->bind(3, eData);
    q->bind(4, eMtime);
    q->execute();

    json result = getMetaJson("SELECT id, data, mtime FROM entries_meta WHERE rowid = last_insert_rowid()");

    db->setLastUpdate();

    return result;
}

json MetaManager::set(const std::string &key, const std::string &data, const std::string &path){
    std::string ePath = entryPath(path);
    std::string eKey = getKey(key, false);
    std::string eData = validateData(data);
    long long eMtime = utils::currentUnixTimestamp();
    json result;

    auto q = db->query("SELECT id FROM entries_meta WHERE path = ? and key = ?");
    q->bind(1, ePath);
    q->bind(2, eKey);
    if (q->fetch()){
        // Entry exists, update
        std::string id = q->getText(0);
        auto uq = db->query("UPDATE entries_meta SET data = ?, mtime = ? WHERE id = ?");
        uq->bind(1, eData);
        uq->bind(2, eMtime);
        uq->bind(3, id);
        uq->execute();
        result = getMetaJson("SELECT id, data, mtime FROM entries_meta WHERE id = '" + id + "'");
    }else{
        // Insert
        auto iq = db->query("INSERT INTO entries_meta (path, key, data, mtime) VALUES (?, ?, ?, ?)");
        iq->bind(1, ePath);
        iq->bind(2, eKey);
        iq->bind(3, eData);
        iq->bind(4, eMtime);
        iq->execute();
        result = getMetaJson("SELECT id, data, mtime FROM entries_meta WHERE rowid = last_insert_rowid()");
    }

    db->setLastUpdate();

    return result;
}

json MetaManager::remove(const std::string &id){
    if (id.empty()) throw InvalidArgsException("Invalid metadata id empty");
    auto q = db->query("DELETE FROM entries_meta WHERE id = ?");
    q->bind(1, id);
    q->execute();
    json j;
    j["removed"] = db->changes();
    return j;
}

json MetaManager::get(const std::string &key, const std::string &path){
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    std::string ePath = entryPath(path);
    json result = json::array();

    auto q = db->query("SELECT id, data, mtime FROM entries_meta WHERE key = ? AND path = ?");
    q->bind(1, key);
    q->bind(2, ePath);

    while (q->fetch()){
        result.push_back(metaStmtToJson(q.get()));
    }

    if (isList(key)){
        return result;
    }else{
        return result[0].empty() ? json::object() : result[0];
    }
}

json MetaManager::unset(const std::string &key, const std::string &path){
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    std::string ePath = entryPath(path);
    auto q = db->query("DELETE FROM entries_meta WHERE key = ? AND path = ?");
    q->bind(1, key);
    q->bind(2, ePath);
    q->execute();

    json j;
    j["deleted"] = db->changes();
    return j;
}

json MetaManager::list(const std::string &path) const{
    std::string sql;
    std::string ePath = entryPath(path);

    if (ePath.empty()) sql = "SELECT key, path, COUNT(id) as 'count' FROM entries_meta GROUP BY path, key";
    else sql = "SELECT key, path, COUNT(id) as 'count' FROM entries_meta WHERE path = ? GROUP BY path, key";

    auto q = db->query(sql);
    if (!ePath.empty()) q->bind(1, ePath);

    json result = json::array();
    while(q->fetch()){
        result.push_back(json::object({{"key", q->getText(0)},
                                       {"path", q->getText(1)},
                                       {"count", q->getInt(2)}}));
    }
    return result;
}
	

}
