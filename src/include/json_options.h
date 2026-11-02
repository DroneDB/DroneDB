/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef JSON_OPTIONS_H
#define JSON_OPTIONS_H

// Checks of the JSON objects a host passes as options (DDBConfigure, DDBBuildEx): an option this
// libddb does not know is refused, never ignored, since the caller would believe it took effect.

#include <initializer_list>
#include <string>
#include <string_view>

#include "exceptions.h"
#include "json.h"

namespace ddb {

// Throws InvalidArgsException: Unknown <what> "<name>" (known: <known>); `known` is the list of
// accepted names, already joined
[[noreturn]] inline void throwUnknownMember(std::string_view what, const std::string& name,
                                            const std::string& known) {
    throw InvalidArgsException("Unknown " + std::string(what) + " \"" + name + "\" (known: " + known + ")");
}

// Throws InvalidArgsException for the first member of the JSON object that is not in `known`
inline void rejectUnknownMembers(const json& object, std::initializer_list<std::string_view> known,
                                 std::string_view what) {
    for (const auto& [name, value] : object.items()) {
        bool found = false;
        std::string names;
        for (const auto candidate : known) {
            found = found || candidate == name;
            if (!names.empty())
                names += ", ";
            names.append(candidate);
        }
        if (!found)
            throwUnknownMember(what, name, names);
    }
}

}  // namespace ddb

#endif  // JSON_OPTIONS_H
