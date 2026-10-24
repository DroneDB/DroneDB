/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DELTA_H
#define DELTA_H

#include <iostream>
#include <optional>

#include "dbops.h"
#include "output_format.h"
#include "simpleentry.h"

namespace ddb
{

    struct RemoveAction
    {
        std::string path;
        std::string hash;

        RemoveAction(const std::string &path, const std::string &hash)
        {
            this->path = path;
            this->hash = hash;
        }

        std::string toString() const
        {
            return std::string("DEL -> [") + (isDirectory() ? "D" : "F") + "] " + path;
        }

        bool isDirectory() const
        {
            return this->hash.empty();
        }
    };

    struct AddAction
    {
        std::string path;
        std::string hash;

        // Hash of the path in the destination stamp, when the path is there (a modification).
        // applyDelta only reports a conflict when the local entry changed since then.
        std::optional<std::string> baseHash;

        AddAction(const std::string &path, const std::string &hash,
                  std::optional<std::string> baseHash = std::nullopt)
        {
            this->path = path;
            this->hash = hash;
            this->baseHash = std::move(baseHash);
        }

        std::string toString() const
        {
            return std::string("ADD -> [") + (isDirectory() ? "D" : "F") + "] " + path;
        }

        bool isDirectory() const
        {
            return this->hash.empty();
        }
    };

    struct Delta
    {
        std::vector<AddAction> adds;
        std::vector<RemoveAction> removes;

        std::vector<std::string> metaAdds;
        std::vector<std::string> metaRemoves;

        std::vector<std::string> modifiedPathList() const
        {
            std::vector<std::string> result;
            for (unsigned long i = 0; i < adds.size(); i++)
                result.push_back(adds[i].path);
            for (unsigned long i = 0; i < removes.size(); i++)
                result.push_back(removes[i].path);
            return result;
        }

        bool empty() const
        {
            return adds.empty() && removes.empty() &&
                   metaAdds.empty() && metaRemoves.empty();
        }
    };

    void to_json(json &j, const SimpleEntry &e);
    void to_json(json &j, const RemoveAction &e);
    void to_json(json &j, const AddAction &e);
    void to_json(json &j, const Delta &d);

    void from_json(const json &j, Delta &d);

    Delta getDelta(const json &sourceDbStamp,
                   const json &destinationDbStamp);

    /**
     * Appends the entry changes between two entry lists, both sorted by path (byte order).
     * A source entry is added when no destination entry has the same path and hash.
     * A destination entry is removed when no source entry has the same path and the
     * same kind (file or folder). Runs in O(n + m).
     */
    void diffSortedEntries(const std::vector<SimpleEntry> &source,
                           const std::vector<SimpleEntry> &destination,
                           std::vector<AddAction> &adds,
                           std::vector<RemoveAction> &removes);

    Delta getDelta(Database *sourceDb, Database *targetDb);

    /// Writes the delta as text (one "A"/"D" line per change) or JSON; any other format throws an
    /// InvalidArgsException
    void delta(Database *sourceDb, Database *targetDb, std::ostream &output, OutputFormat format);
    void delta(const json &sourceDbStamp, const json &targetDbStamp, std::ostream &output, OutputFormat format);

    std::vector<SimpleEntry> parseStampEntries(const json &stamp);

} // namespace ddb

#endif // DELTA_H
