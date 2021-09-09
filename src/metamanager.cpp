#include "metamanager.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"
#include "json.h"

namespace ddb {

std::string MetaManager::entryPath(const std::string &path) const{
    if (path.empty() || path == ".") return "";

    std::string relPath = io::Path(path).relativeTo(db->rootDirectory()).generic();

    auto q = db->query("SELECT 1 FROM entries WHERE path = ?");
    q->bind(1, relPath);
    if (!q->fetch()) throw InvalidArgsException("Path " + path + " not available in index");

    return relPath;
}

std::string MetaManager::getKey(const std::string &key, bool isList) const{
    if (key.empty()) throw InvalidArgsException("Invalid empty metadata key");
    if (isList && key[key.length() - 1] != 's') throw InvalidArgsException("Invalid metadata key (must be plural, for example: " + key + "s)");
    else if (!isList && key[key.length() - 1] == 's') throw InvalidArgsException("Invalid metadata key (must be singular, for example: " + key.substr(0, key.length() - 1) + ")");
    else return key;
}

json MetaManager::getMetaJson(Statement *q) const{
    if (q->fetch()){
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
    }else throw DBException("Cannot fetch meta with query: " + q->getQuery());
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
        result["id"] = id;
        result["data"] = eData;
        result["mtime"] = eMtime;
    }else{
        // Insert
        auto iq = db->query("INSERT OR REPLACE INTO entries_meta (path, key, data, mtime) VALUES (?, ?, ?, ?)");
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

	

}
