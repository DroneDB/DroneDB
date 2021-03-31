/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


#include <mio.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"
#include "delta.h"


namespace ddb {

void to_json(json& j, const SimpleEntry& e) {
    j = json{{"path", e.path}, {"hash", e.hash}, {"type", e.type}};
}

void to_json(json& j, const CopyAction& e) {
    j = json::array({e.source, e.destination});
}

void to_json(json& j, const RemoveAction& e) {
    j = json{{"path", e.path}, {"type", e.type}};
}

void to_json(json& j, const AddAction& e) {
    j = json{{"path", e.path}, {"type", e.type}};
}

void to_json(json& j, const Delta& d) {
    j = {{"adds", d.adds}, {"removes", d.removes}, {"copies", d.copies}};
}

std::vector<SimpleEntry> getAllSimpleEntries(Database* db) {
    auto q = db->query("SELECT path, hash, type FROM entries");

    std::vector<SimpleEntry> entries;

    while (q->fetch()) {
        SimpleEntry e(q->getText(0), q->getText(1),
                      static_cast<EntryType>(q->getInt(2)));

        entries.push_back(e);
    }

    q->reset();

    return entries;
}


Delta getDelta(Database* sourceDb, Database* targetDb) {
    const auto source = getAllSimpleEntries(sourceDb);
    const auto destination = getAllSimpleEntries(targetDb);

    return getDelta(source, destination);
}

void delta(Database* sourceDb, Database* targetDb, std::ostream& output,
           const std::string& format) {

    auto delta = getDelta(sourceDb, targetDb);

    if (format == "json") {

        json j = delta;
        
        output << j.dump();

    } else if (format == "text") {
        for (const CopyAction& cpy : delta.copies)
            output << "C\t" << cpy.source << " => " << cpy.destination << std::endl;

        for (const AddAction& add : delta.adds)
            output << "A\t" << add.path << (add.type == Directory ? " (D)" : "") << std::endl;

        for (const RemoveAction& rem : delta.removes)
            output << "D\t" << rem.path << (rem.type == Directory ? " (D)" : "") << std::endl;

    }
}


Delta getDelta(std::vector<SimpleEntry> source,
               std::vector<SimpleEntry> destination) {
                   
    std::vector<CopyAction> copies;
    std::vector<RemoveAction> removes;
    std::vector<AddAction> adds;

    // Sort by path
    std::sort(source.begin(), source.end(),
              [](const SimpleEntry& l, const SimpleEntry& r) {
                  return l.path < r.path;
              });

    // Sort by path
    std::sort(destination.begin(), destination.end(),
              [](const SimpleEntry& l, const SimpleEntry& r) {
                  return l.path < r.path;
              });

    for (const SimpleEntry& entry : source) {
        const auto inDestWithSameHashAndPath =
            std::find_if(destination.begin(), destination.end(),
                         [&entry](const SimpleEntry& e) {
                             return e.hash == entry.hash &&
                                    e.type == entry.type &&
                                    e.path == entry.path;
                         }) != destination.end();

        if (inDestWithSameHashAndPath) {
            LOGD << "SKIP -> " << entry.toString();
            continue;
        }

        const auto inDestWithSameHashEntry = std::find_if(
            destination.begin(), destination.end(),
            [&entry](const SimpleEntry& e) {
                return e.hash == entry.hash && e.type == entry.type;
            });

        if (inDestWithSameHashEntry == destination.end()) {
            LOGD << "ADD  -> " << entry.toString();
            adds.emplace_back(AddAction(entry.path, entry.type));
            continue;
        }

        if (inDestWithSameHashEntry->type != Directory) {
            LOGD << "COPY -> " << inDestWithSameHashEntry->toString() << " => "
                 << entry.toString();
            copies.emplace_back(
                CopyAction(inDestWithSameHashEntry->path, entry.path));
        } else {
            LOGD << "ADD FOLDER -> " << entry.toString();
            adds.emplace_back(AddAction(entry.path, entry.type));
        }
    }

    for (const SimpleEntry& entry : destination) {
        const auto notInSourceWithSamePath = std::find_if(
            source.begin(), source.end(), [&entry](const SimpleEntry& e) {
                return e.path == entry.path && e.type == entry.type;
            });

        if (notInSourceWithSamePath == source.end()) {
            LOGD << "DEL  -> " << entry.toString();
            removes.emplace_back(RemoveAction(entry.path, entry.type));
        }
    }

    // Sort removes by path descending
    std::sort(removes.begin(), removes.end(),
              [](const RemoveAction& l, const RemoveAction& r) {
                  return l.path > r.path;
              });


    Delta d;
    d.copies = copies;
    d.removes = removes;
    d.adds = adds;

    return d;
}

}  // namespace ddb
