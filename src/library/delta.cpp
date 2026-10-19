/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mio.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"
#include "delta.h"

namespace ddb
{

    void to_json(json &j, const SimpleEntry &e)
    {
        j = json{{"path", e.path}, {"hash", e.hash}};
    }

    void to_json(json &j, const RemoveAction &e)
    {
        j = json{{"path", e.path}, {"hash", e.hash}};
    }

    void to_json(json &j, const AddAction &e)
    {
        j = json{{"path", e.path}, {"hash", e.hash}};
    }

    void to_json(json &j, const Delta &d)
    {
        j = {{"adds", d.adds}, {"removes", d.removes}, {"metaAdds", d.metaAdds}, {"metaRemoves", d.metaRemoves}};
    }

    void from_json(const json &j, Delta &d)
    {
        d.adds.clear();
        d.removes.clear();

        if (j.contains("adds"))
        {
            for (auto &add : j["adds"])
            {
                d.adds.push_back(AddAction(add["path"], add["hash"]));
            }
        }
        if (j.contains("removes"))
        {
            for (auto &remove : j["removes"])
            {
                d.removes.push_back(RemoveAction(remove["path"], remove["hash"]));
            }
        }
        if (j.contains("metaAdds"))
        {
            for (auto &id : j["metaAdds"])
            {
                d.metaAdds.push_back(id);
            }
        }
        if (j.contains("metaRemoves"))
        {
            for (auto &id : j["metaRemoves"])
            {
                d.metaRemoves.push_back(id);
            }
        }
    }

    Delta getDelta(Database *sourceDb, Database *targetDb)
    {
        return getDelta(sourceDb->getStamp(), targetDb->getStamp());
    }

    void delta(Database *sourceDb, Database *targetDb, std::ostream &output, const std::string &format)
    {
        delta(sourceDb->getStamp(), targetDb->getStamp(), output, format);
    }

    void delta(const json &sourceDbStamp, const json &targetDbStamp, std::ostream &output, const std::string &format)
    {
        auto delta = getDelta(sourceDbStamp, targetDbStamp);

        if (format == "json")
        {

            json j = delta;

            output << j.dump();
        }
        else if (format == "text")
        {
            for (const AddAction &add : delta.adds)
                output << "A\t" << add.path << (add.isDirectory() ? " (D)" : "") << std::endl;

            for (const RemoveAction &rem : delta.removes)
                output << "D\t" << rem.path << (rem.isDirectory() ? " (D)" : "") << std::endl;
        }
    }

    void diffSortedEntries(const std::vector<SimpleEntry> &source,
                           const std::vector<SimpleEntry> &destination,
                           std::vector<AddAction> &adds,
                           std::vector<RemoveAction> &removes)
    {
        // Merge-join over both lists (sorted by path): O(n + m).
        size_t i = 0, j = 0;
        while (i < source.size() || j < destination.size())
        {
            if (j == destination.size() || (i < source.size() && source[i].path < destination[j].path))
            {
                LOGD << "ADD  -> " << source[i].toString();
                adds.emplace_back(source[i].path, source[i].hash);
                i++;
                continue;
            }

            if (i == source.size() || destination[j].path < source[i].path)
            {
                LOGD << "DEL  -> " << destination[j].toString();
                removes.emplace_back(destination[j].path, destination[j].hash);
                j++;
                continue;
            }

            // Same path on both sides. A stamp read from an index has one entry per path,
            // but a stamp built by hand can repeat one: compare the two groups.
            const std::string &path = source[i].path;
            size_t iEnd = i, jEnd = j;
            while (iEnd < source.size() && source[iEnd].path == path)
                iEnd++;
            while (jEnd < destination.size() && destination[jEnd].path == path)
                jEnd++;

            for (size_t s = i; s < iEnd; s++)
            {
                bool sameHash = false;
                for (size_t d = j; d < jEnd && !sameHash; d++)
                    sameHash = destination[d].hash == source[s].hash;

                if (!sameHash)
                {
                    LOGD << "ADD  -> " << source[s].toString();
                    adds.emplace_back(source[s].path, source[s].hash);
                }
            }

            for (size_t d = j; d < jEnd; d++)
            {
                bool sameKind = false;
                for (size_t s = i; s < iEnd && !sameKind; s++)
                    sameKind = source[s].isDirectory() == destination[d].isDirectory();

                if (!sameKind)
                {
                    LOGD << "DEL  -> " << destination[d].toString();
                    removes.emplace_back(destination[d].path, destination[d].hash);
                }
            }

            i = iEnd;
            j = jEnd;
        }
    }

    Delta getDelta(const json &sourceDbStamp,
                   const json &destinationDbStamp)
    {
        std::vector<SimpleEntry> source = parseStampEntries(sourceDbStamp);
        std::vector<SimpleEntry> destination = parseStampEntries(destinationDbStamp);

        std::vector<RemoveAction> removes;
        std::vector<AddAction> adds;

        std::vector<std::string> metaAdds;
        std::vector<std::string> metaRemoves;

        const auto byPath = [](const SimpleEntry &l, const SimpleEntry &r)
        { return l.path < r.path; };
        std::sort(source.begin(), source.end(), byPath);
        std::sort(destination.begin(), destination.end(), byPath);

        diffSortedEntries(source, destination, adds, removes);

        // Sort removes by path descending
        std::sort(removes.begin(), removes.end(),
                  [](const RemoveAction &l, const RemoveAction &r)
                  {
                      return l.path > r.path;
                  });

        // Compute meta adds/removes
        if (!sourceDbStamp.contains("meta"))
            throw InvalidArgsException("Stamp meta not found");
        if (!destinationDbStamp.contains("meta"))
            throw InvalidArgsException("Stamp meta not found");

        // Find meta IDs in source that are not in destination
        std::unordered_map<std::string, bool> metaIds;
        std::vector<std::string> sourceMetaIds = sourceDbStamp["meta"].get<std::vector<std::string>>();
        std::vector<std::string> destinationMetaIds = destinationDbStamp["meta"].get<std::vector<std::string>>();

        for (auto &id : sourceMetaIds)
            metaIds[id] = true;
        for (auto &id : destinationMetaIds)
            metaIds.erase(id);
        for (auto &it : metaIds)
            metaAdds.push_back(it.first);

        metaIds.clear();

        // Find meta IDs in destination that are not in source
        for (auto &id : destinationMetaIds)
            metaIds[id] = true;
        for (auto &id : sourceMetaIds)
            metaIds.erase(id);
        for (auto &it : metaIds)
            metaRemoves.push_back(it.first);

        Delta d;
        d.removes = removes;
        d.adds = adds;
        d.metaAdds = metaAdds;
        d.metaRemoves = metaRemoves;

        return d;
    }

    std::vector<SimpleEntry> parseStampEntries(const json &stamp)
    {
        std::vector<SimpleEntry> result;

        if (!stamp.contains("entries"))
            throw InvalidArgsException("Stamp entries not found");

        const auto &entries = stamp["entries"];
        result.reserve(entries.size());
        for (auto &i : entries)
        {
            auto obj = i.begin();
            result.push_back(SimpleEntry(obj.key(), obj.value()));
        }

        return result;
    }

} // namespace ddb
