/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_types.h"
#include <algorithm>
#include <cctype>
#include <vector>

#include "entry_handler.h"

namespace ddb
{

    // The names of the types are those of their handlers (entry_handler.h)

    std::string typeToHuman(EntryType t)
    {
        const EntryHandler *handler = findEntryHandler(t);
        return handler != nullptr ? handler->name() : "?";
    }

    static std::string lowercase(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    EntryType typeFromHuman(const std::string &s)
    {
        const std::string lower = lowercase(s);
        for (const EntryHandler *handler : entryHandlers())
        {
            if (lowercase(handler->name()) == lower)
                return handler->type();
        }
        return EntryType::Undefined;
    }

    std::vector<std::string> getEntryTypeNames()
    {
        std::vector<std::string> names;
        for (const EntryHandler *handler : entryHandlers())
        {
            // Skip Directory and Undefined as they cannot be rescanned
            if (handler->type() != EntryType::Directory && handler->type() != EntryType::Undefined)
                names.push_back(lowercase(handler->name()));
        }
        return names;
    }

}
