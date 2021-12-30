/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef SIMPLEENTRY_H
#define SIMPLEENTRY_H

#include <string>

namespace ddb{

struct SimpleEntry {
    std::string path;
    std::string hash;

    std::string toString() const {
        return path + " - " + hash;
    }

    SimpleEntry(std::string path, std::string hash) {
        this->path = std::move(path);
        this->hash = std::move(hash);
    }

    SimpleEntry(std::string path) {
        this->path = std::move(path);
        this->hash = "";
    }

    bool isDirectory() const{
        return this->hash.empty();
    }

    bool operator==(const SimpleEntry& rhs) const {
        return this->hash == rhs.hash && this->path == rhs.path;
    }
    bool operator!=(const SimpleEntry& rhs) const {
        return !(*this == rhs);
    }
};

}

#endif // SIMPLEENTRY_H
