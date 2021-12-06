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
    j = json{{"path", e.path}, {"hash", e.hash}};
}

void to_json(json& j, const RemoveAction& e) {
    j = json{{"path", e.path}, {"hash", e.hash}};
}

void to_json(json& j, const AddAction& e) {
    j = json{{"path", e.path}, {"hash", e.hash}};
}

void to_json(json& j, const Delta& d) {
    j = {{"adds", d.adds}, {"removes", d.removes}};
}

void from_json(const json &j, Delta &d){
    d.adds.clear();
    d.removes.clear();

    if (j.contains("adds")){
        for (auto &add : j["adds"]){
            d.adds.push_back(AddAction(add["path"], add["hash"]));
        }
    }
    if (j.contains("removes")){
        for (auto &remove : j["removes"]){
            d.removes.push_back(RemoveAction(remove["path"], remove["hash"]));
        }
    }
}

Delta getDelta(Database* sourceDb, Database* targetDb) {
    return getDelta(sourceDb->getStamp(), targetDb->getStamp());
}

void delta(Database* sourceDb, Database* targetDb, std::ostream& output, const std::string& format) {
    delta(sourceDb->getStamp(), targetDb->getStamp(), output, format);
}

void delta(const json &sourceDbStamp, const json &targetDbStamp, std::ostream& output, const std::string& format){
    auto delta = getDelta(sourceDbStamp, targetDbStamp);

    if (format == "json") {

        json j = delta;

        output << j.dump();

    } else if (format == "text") {
        for (const AddAction& add : delta.adds)
            output << "A\t" << add.path << (add.isDirectory() ? " (D)" : "") << std::endl;

        for (const RemoveAction& rem : delta.removes)
            output << "D\t" << rem.path << (rem.isDirectory() ? " (D)" : "") << std::endl;

    }
}

Delta getDelta(const json &sourceDbStamp,
               const json &destinationDbStamp) {
    std::vector<SimpleEntry> source = parseStampEntries(sourceDbStamp);
    std::vector<SimpleEntry> destination = parseStampEntries(destinationDbStamp);

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
                                    e.path == entry.path;
                         }) != destination.end();

        if (inDestWithSameHashAndPath) {
            LOGD << "SKIP -> " << entry.toString();
            continue;
        }

        LOGD << "ADD  -> " << entry.toString();
        adds.emplace_back(AddAction(entry.path, entry.hash));
    }

    for (const SimpleEntry& entry : destination) {
        const auto notInSourceWithSamePath = std::find_if(
            source.begin(), source.end(), [&entry](const SimpleEntry& e) {
                return e.path == entry.path && e.isDirectory() == entry.isDirectory();
            });

        if (notInSourceWithSamePath == source.end()) {
            LOGD << "DEL  -> " << entry.toString();
            removes.emplace_back(RemoveAction(entry.path, entry.hash));
        }
    }

    // Sort removes by path descending
    std::sort(removes.begin(), removes.end(),
              [](const RemoveAction& l, const RemoveAction& r) {
                  return l.path > r.path;
              });


    Delta d;
    d.removes = removes;
    d.adds = adds;

    return d;
}

std::vector<SimpleEntry> parseStampEntries(const json &stamp){
    std::vector<SimpleEntry> result;

    if (!stamp.contains("entries")) throw InvalidArgsException("Stamp entries not found");

    for (auto &i : stamp["entries"]){
        auto obj = i.begin();
        result.push_back(SimpleEntry(obj.key(), obj.value()));
    }

    return result;
}

}  // namespace ddb
